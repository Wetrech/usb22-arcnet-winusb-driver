/*
 * arcnet.c  --  USB22-485 WinUSB user-mode library implementation.
 *
 * Protocol reference: reference/usb22-protocol-notes.md (UPDATE 1-6)
 *
 * Startup sequence:
 *   arc_open()  ->  CreateFile + WinUsb_Initialize  (per context)
 *   arc_init()  ->  cmd04 -> handshake -> COM20020 config (~2.5 s)
 *
 * Multiple devices are supported: each arc_open() allocates an independent
 * arc_ctx_t on the heap.  There is no global state.
 *
 * Thread safety: each context carries its own CRITICAL_SECTION.
 *   - Concurrent calls on DIFFERENT contexts are fully parallel (no shared lock).
 *   - Concurrent calls on the SAME context serialize: the second caller waits.
 *   - CRITICAL_SECTION is recursive on Windows, so internal calls
 *     (e.g. arc_transmit -> arc_register for ACK polling) never deadlock.
 *   - arc_close() must not be called while another thread may still be inside
 *     any API function on the same context (no reference counting).
 */

#include "arcnet.h"
#include <windows.h>
#include <setupapi.h>
#include <winusb.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

#define RX_BUF_SIZE  512u

/* -----------------------------------------------------------------------
 * Per-device context (opaque to callers; defined here only)
 * --------------------------------------------------------------------- */
struct arc_ctx_s {
    CRITICAL_SECTION        lock;         /* per-context recursive mutex  */
    HANDLE                  dev_handle;
    WINUSB_INTERFACE_HANDLE usb_handle;
    bool                    verbose;
    bool                    device_gone;  /* set on fatal USB error; cleared by arc_reopen */
    char                    device_path[256]; /* stored for arc_reopen */
};

/* -----------------------------------------------------------------------
 * Logging — require a `arc_ctx_t *ctx` in scope.
 * --------------------------------------------------------------------- */
#define VLOG(ctx, fmt, ...) \
    do { if ((ctx)->verbose) printf("[arcnet] " fmt, ##__VA_ARGS__); } while (0)

#define VLOG_ERR(ctx, fmt, ...) \
    do { if ((ctx)->verbose) fprintf(stderr, "[arcnet] " fmt, ##__VA_ARGS__); } while (0)

/* -----------------------------------------------------------------------
 * is_gone_error  --  returns true when GetLastError indicates the USB
 * device has been physically removed or its handle has become invalid.
 * --------------------------------------------------------------------- */
static bool is_gone_error(DWORD err)
{
    switch (err) {
    case ERROR_INVALID_HANDLE:        /*   6 */
    case ERROR_GEN_FAILURE:           /*  31 */
    case ERROR_BAD_COMMAND:           /*  22 */
    case ERROR_DEVICE_NOT_CONNECTED:  /* 1167 */
    case ERROR_NOT_FOUND:             /* 1168 */
    case ERROR_NO_SUCH_DEVICE:        /*  433 */
        return true;
    default:
        return false;
    }
}

/* -----------------------------------------------------------------------
 * pipe_flush  --  cancel pending I/O and reset data toggle on a pipe.
 *
 * Must be called after any transfer error or timeout to leave the pipe in
 * a clean state for the next operation.  Both steps are logged.
 *   AbortPipe  -- cancels any in-flight IRP / USB transaction
 *   ResetPipe  -- sends CLEAR_FEATURE(ENDPOINT_HALT), resets data toggle
 *
 * Caller must hold ctx->lock.
 * --------------------------------------------------------------------- */
static void pipe_flush(arc_ctx_t *ctx, UCHAR ep)
{
    DWORD err;
    if (ctx->device_gone) return;
    if (!WinUsb_AbortPipe(ctx->usb_handle, ep)) {
        err = GetLastError();
        if (is_gone_error(err)) {
            ctx->device_gone = true;
            VLOG_ERR(ctx, "pipe_flush: device gone on AbortPipe EP0x%02X GLE=%lu\n", ep, err);
            return;
        }
        VLOG_ERR(ctx, "pipe_flush: AbortPipe EP0x%02X FAIL err=%lu\n", ep, err);
    } else {
        VLOG(ctx, "pipe_flush: AbortPipe EP0x%02X OK\n", ep);
    }
    if (ctx->device_gone) return;
    if (!WinUsb_ResetPipe(ctx->usb_handle, ep)) {
        err = GetLastError();
        if (is_gone_error(err)) {
            ctx->device_gone = true;
            VLOG_ERR(ctx, "pipe_flush: device gone on ResetPipe EP0x%02X GLE=%lu\n", ep, err);
            return;
        }
        VLOG_ERR(ctx, "pipe_flush: ResetPipe EP0x%02X FAIL err=%lu\n", ep, err);
    } else {
        VLOG(ctx, "pipe_flush: ResetPipe EP0x%02X OK\n", ep);
    }
}

/* Project-specific DeviceInterfaceGUID — must match driver/usb22_winusb.inf [Dev_AddReg].
 * {E6B4B5C0-F74E-4A1D-9B8F-2C3D4E5F6A7B} */
static const GUID GUID_USB_DEVICE = {
    0xE6B4B5C0u, 0xF74Eu, 0x4A1Du,
    { 0x9Bu, 0x8Fu, 0x2Cu, 0x3Du, 0x4Eu, 0x5Fu, 0x6Au, 0x7Bu }
};

/* =======================================================================
 * arc_result_str
 * ===================================================================== */
const char *arc_result_str(arc_result_t r)
{
    switch (r) {
    case ARC_OK:              return "ARC_OK";
    case ARC_NO_PACKET:       return "ARC_NO_PACKET";
    case ARC_NOT_ACKED:       return "ARC_NOT_ACKED";
    case ARC_ERR_DEVICE_GONE: return "ARC_ERR_DEVICE_GONE";
    case ARC_ERR_NET_BUSY:    return "ARC_ERR_NET_BUSY (network busy / transmitter not available, transient)";
    case ARC_ERR_OPEN:        return "ARC_ERR_OPEN";
    case ARC_ERR_IO:          return "ARC_ERR_IO";
    case ARC_ERR_TIMEOUT:     return "ARC_ERR_TIMEOUT";
    case ARC_ERR_PARAM:       return "ARC_ERR_PARAM";
    case ARC_ERR_ECHO:        return "ARC_ERR_ECHO";
    default:                  return "(unknown)";
    }
}

/* =======================================================================
 * enum_device_paths  --  internal; no context needed
 * ===================================================================== */
static int enum_device_paths(unsigned int vid, unsigned int pid,
                              char paths[][256], int maxDevices)
{
    HDEVINFO                           devs;
    SP_DEVICE_INTERFACE_DATA           iface_data;
    PSP_DEVICE_INTERFACE_DETAIL_DATA_A detail;
    DWORD                              idx, needed;
    char                               token[32];
    char                               upper[512];
    char                              *p;
    int                                count = 0;

    snprintf(token, sizeof(token), "VID_%04X&PID_%04X", vid, pid);

    devs = SetupDiGetClassDevsA(&GUID_USB_DEVICE, NULL, NULL,
                                 DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE) return 0;

    iface_data.cbSize = sizeof(iface_data);
    for (idx = 0;
         count < maxDevices &&
         SetupDiEnumDeviceInterfaces(devs, NULL, &GUID_USB_DEVICE, idx, &iface_data);
         idx++) {

        SetupDiGetDeviceInterfaceDetailA(devs, &iface_data, NULL, 0, &needed, NULL);
        detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)malloc(needed);
        if (!detail) continue;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);

        if (!SetupDiGetDeviceInterfaceDetailA(devs, &iface_data, detail,
                                               needed, NULL, NULL)) {
            free(detail); continue;
        }

        strncpy(upper, detail->DevicePath, sizeof(upper) - 1);
        upper[sizeof(upper) - 1] = '\0';
        for (p = upper; *p; p++) *p = (char)toupper((unsigned char)*p);

        if (strstr(upper, token)) {
            strncpy(paths[count], detail->DevicePath, 255);
            paths[count][255] = '\0';
            count++;
        }
        free(detail);
    }

    SetupDiDestroyDeviceInfoList(devs);
    return count;
}

/* =======================================================================
 * arc_list_devices
 * ===================================================================== */
int arc_list_devices(char paths[][256], int maxDevices)
{
    if (!paths || maxDevices <= 0) return 0;
    return enum_device_paths(ARC_VID, ARC_PID, paths, maxDevices);
}

/* =======================================================================
 * arc_open
 *   Allocates and returns a new context for the specified device path.
 *   Returns NULL on any failure; the caller need not check error codes.
 * ===================================================================== */
arc_ctx_t *arc_open(const char *devicePath, bool verbose)
{
    arc_ctx_t  *ctx;
    char        auto_path[256];
    const char *path;
    ULONG       timeout_ms = ARC_READ_TIMEOUT_MS;

    if (devicePath) {
        path = devicePath;
    } else {
        char found[1][256];
        if (enum_device_paths(ARC_VID, ARC_PID, found, 1) == 0) {
            if (verbose)
                fprintf(stderr,
                        "[arcnet] arc_open: device not found (VID=%04X PID=%04X)\n",
                        ARC_VID, ARC_PID);
            return NULL;
        }
        strncpy(auto_path, found[0], sizeof(auto_path) - 1);
        auto_path[sizeof(auto_path) - 1] = '\0';
        path = auto_path;
    }

    ctx = (arc_ctx_t *)calloc(1, sizeof(arc_ctx_t));
    if (!ctx) return NULL;
    InitializeCriticalSection(&ctx->lock);
    ctx->verbose = verbose;
    strncpy(ctx->device_path, path, sizeof(ctx->device_path) - 1);
    ctx->device_path[sizeof(ctx->device_path) - 1] = '\0';

    VLOG(ctx, "arc_open: %s\n", path);

    ctx->dev_handle = CreateFileA(path,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);

    if (ctx->dev_handle == INVALID_HANDLE_VALUE) {
        VLOG_ERR(ctx, "arc_open: CreateFile failed: %lu\n", GetLastError());
        DeleteCriticalSection(&ctx->lock);
        free(ctx);
        return NULL;
    }

    if (!WinUsb_Initialize(ctx->dev_handle, &ctx->usb_handle)) {
        VLOG_ERR(ctx, "arc_open: WinUsb_Initialize failed: %lu\n", GetLastError());
        CloseHandle(ctx->dev_handle);
        DeleteCriticalSection(&ctx->lock);
        free(ctx);
        return NULL;
    }

    if (!WinUsb_SetPipePolicy(ctx->usb_handle, ARC_EP_EVT_IN,
                               PIPE_TRANSFER_TIMEOUT,
                               sizeof(timeout_ms), &timeout_ms))
        VLOG_ERR(ctx, "arc_open: SetPipePolicy EP0x81 warning: %lu\n", GetLastError());

    VLOG(ctx, "arc_open: OK\n");
    return ctx;
}

/* =======================================================================
 * arc_set_verbose
 * ===================================================================== */
void arc_set_verbose(arc_ctx_t *ctx, bool enable)
{
    if (!ctx) return;
    EnterCriticalSection(&ctx->lock);
    ctx->verbose = enable;
    LeaveCriticalSection(&ctx->lock);
}

/* =======================================================================
 * arc_close
 *   Waits for any in-progress operation to finish (via the lock), then
 *   closes handles, releases the lock, deletes the CS, and frees memory.
 *   Calling arc_close() while another thread is actively blocked inside
 *   an API call on the same context is not supported.
 * ===================================================================== */
void arc_close(arc_ctx_t *ctx)
{
    if (!ctx) return;
    EnterCriticalSection(&ctx->lock);
    if (ctx->usb_handle) { WinUsb_Free(ctx->usb_handle); ctx->usb_handle = NULL; }
    if (ctx->dev_handle) { CloseHandle(ctx->dev_handle);  ctx->dev_handle = NULL; }
    VLOG(ctx, "arc_close: done\n");
    LeaveCriticalSection(&ctx->lock);
    DeleteCriticalSection(&ctx->lock);
    free(ctx);
}

/* =======================================================================
 * write_cmd  --  internal; caller must hold ctx->lock.
 * ===================================================================== */
static arc_result_t write_cmd(arc_ctx_t *ctx, const BYTE *cmd, ULONG len)
{
    ULONG xferred = 0;
    DWORD err;
    if (ctx->device_gone) return ARC_ERR_DEVICE_GONE;
    if (!WinUsb_WritePipe(ctx->usb_handle, ARC_EP_CMD_OUT,
                           (PUCHAR)cmd, len, &xferred, NULL)) {
        err = GetLastError();
        if (is_gone_error(err)) {
            ctx->device_gone = true;
            VLOG_ERR(ctx, "write_cmd: device gone GLE=%lu\n", err);
            return ARC_ERR_DEVICE_GONE;
        }
        VLOG_ERR(ctx, "write_cmd: WritePipe EP0x01 failed: %lu\n", err);
        return ARC_ERR_IO;
    }
    if (xferred != len) {
        VLOG_ERR(ctx, "write_cmd: short write %lu/%lu\n", xferred, len);
        return ARC_ERR_IO;
    }
    return ARC_OK;
}

/* =======================================================================
 * read_response  --  internal; caller must hold ctx->lock.
 *   Reads from EP_EVT_IN (0x81) until a packet with byte[0]==expected
 *   arrives or the time budget expires.
 *   Per-read timeout = ARC_READ_TIMEOUT_MS; budget allows multiple reads.
 * ===================================================================== */
static arc_result_t read_response(arc_ctx_t *ctx,
                                   BYTE expected_opcode,
                                   BYTE *buf, ULONG buf_size,
                                   DWORD budget_ms, ULONG *out_len)
{
    ULONGLONG start = GetTickCount64();
    ULONG     xferred;
    DWORD     err;

    while ((GetTickCount64() - start) < (ULONGLONG)budget_ms) {
        if (ctx->device_gone) return ARC_ERR_DEVICE_GONE;
        xferred = 0;
        if (!WinUsb_ReadPipe(ctx->usb_handle, ARC_EP_EVT_IN,
                              buf, buf_size, &xferred, NULL)) {
            err = GetLastError();
            if (err == ERROR_SEM_TIMEOUT) continue;
            if (is_gone_error(err)) {
                ctx->device_gone = true;
                VLOG_ERR(ctx, "read_response: device gone GLE=%lu\n", err);
                return ARC_ERR_DEVICE_GONE;
            }
            VLOG_ERR(ctx, "read_response: ReadPipe EP0x81 failed: %lu\n", err);
            return ARC_ERR_IO;
        }

        if (xferred == 0) continue;

        if (buf[0] == expected_opcode) {
            *out_len = xferred;
            return ARC_OK;
        }

        if (buf[0] == ARC_OPCODE_EVENT)
            VLOG(ctx, "read_response: skipping 0x20 event at %.1f s\n",
                 (double)(GetTickCount64() - start) / 1000.0);
        else
            VLOG(ctx, "read_response: skipping unexpected opcode 0x%02X at %.1f s\n",
                 buf[0], (double)(GetTickCount64() - start) / 1000.0);
    }

    VLOG_ERR(ctx, "read_response: budget %lu ms exhausted, opcode 0x%02X not received\n",
             budget_ms, expected_opcode);
    return ARC_ERR_TIMEOUT;
}

/* =======================================================================
 * cmd04_internal  --  session-start (UPDATE 4); caller must hold ctx->lock.
 * ===================================================================== */
static arc_result_t cmd04_internal(arc_ctx_t *ctx)
{
    BYTE         cmd[2] = { ARC_OPCODE_CMD04, 0x00 };
    BYTE         resp[ARC_EP_EVT_MAXPACKET];
    ULONG        xferred;
    arc_result_t r;
    ULONG        i;

    VLOG(ctx, "cmd04: sending 04 00\n");

    r = write_cmd(ctx, cmd, sizeof(cmd));
    if (r != ARC_OK) return r;

    memset(resp, 0, sizeof(resp));
    r = read_response(ctx, ARC_OPCODE_CMD04, resp, sizeof(resp),
                      ARC_BUDGET_SHORT_MS, &xferred);
    if (r != ARC_OK) return r;

    if (ctx->verbose) {
        printf("[arcnet] cmd04: response (%lu bytes):", xferred);
        for (i = 0; i < xferred; i++) printf(" %02X", resp[i]);
        printf("\n");
    }

    if (xferred < 4 || resp[1] || resp[2] || resp[3])
        VLOG(ctx, "cmd04: response differs from 04 00 00 00 (continuing)\n");

    return ARC_OK;
}

/* =======================================================================
 * handshake_internal  --  data-channel probe (UPDATE 5); caller must hold ctx->lock.
 * ===================================================================== */
static arc_result_t handshake_internal(arc_ctx_t *ctx)
{
    BYTE  out_buf[10];
    BYTE  in_buf[ARC_EP_EVT_MAXPACKET];
    ULONG xferred;
    ULONG hs_timeout = 500u;
    DWORD err;
    ULONG i;

    if (!WinUsb_SetPipePolicy(ctx->usb_handle, ARC_EP_RX_IN,
                               PIPE_TRANSFER_TIMEOUT,
                               sizeof(hs_timeout), &hs_timeout))
        VLOG_ERR(ctx, "handshake: SetPipePolicy EP0x86 warning: %lu\n", GetLastError());

    memset(out_buf, 0x00, sizeof(out_buf));
    xferred = 0;
    if (!WinUsb_WritePipe(ctx->usb_handle, ARC_EP_TX_OUT,
                           out_buf, sizeof(out_buf), &xferred, NULL)) {
        VLOG_ERR(ctx, "handshake: WritePipe EP0x02 failed: %lu\n", GetLastError());
        return ARC_ERR_IO;
    }
    VLOG(ctx, "handshake: sent %lu zero bytes to EP0x02\n", xferred);

    memset(in_buf, 0, sizeof(in_buf));
    xferred = 0;
    if (!WinUsb_ReadPipe(ctx->usb_handle, ARC_EP_RX_IN,
                          in_buf, sizeof(in_buf), &xferred, NULL)) {
        err = GetLastError();
        if (err == ERROR_SEM_TIMEOUT) {
            VLOG(ctx, "handshake: EP0x86 timeout (empty channel, OK)\n");
            return ARC_OK;
        }
        VLOG_ERR(ctx, "handshake: ReadPipe EP0x86 failed: %lu\n", err);
        return ARC_ERR_IO;
    }

    if (ctx->verbose) {
        printf("[arcnet] handshake: received %lu bytes:", xferred);
        for (i = 0; i < xferred; i++) printf(" %02X", in_buf[i]);
        printf("\n");
    }

    return ARC_OK;
}

/* =======================================================================
 * arc_init
 *   Full startup sequence: cmd04 -> handshake -> COM20020 config.
 * ===================================================================== */
arc_result_t arc_init(arc_ctx_t *ctx, uint8_t nodeID, uint8_t timeout,
                      uint8_t clockPrescaler, bool recvBroadcasts)
{
    arc_result_t r;
    BYTE         cmd[12];
    BYTE         resp[ARC_EP_EVT_MAXPACKET];
    ULONG        xferred;
    ULONG        rx_timeout = ARC_RECEIVE_TIMEOUT_MS;
    ULONG        i;

    if (!ctx) return ARC_ERR_OPEN;
    EnterCriticalSection(&ctx->lock);

    if (ctx->device_gone) { r = ARC_ERR_DEVICE_GONE; goto done; }

    r = cmd04_internal(ctx);
    if (r != ARC_OK) goto done;

    r = handshake_internal(ctx);
    if (r != ARC_OK) goto done;

    /* COM20020 configuration command (12 bytes) — see protocol notes UPDATE 6 */
    cmd[0]  = ARC_OPCODE_INIT;
    cmd[1]  = 0x00;
    cmd[2]  = 0x00;  cmd[3]  = 0x00;
    cmd[4]  = 0x00;
    cmd[5]  = timeout;
    cmd[6]  = nodeID;
    cmd[7]  = 0x01;
    cmd[8]  = 0x00;
    cmd[9]  = clockPrescaler;
    cmd[10] = (clockPrescaler > 5u) ? 0x01u : 0x00u;
    cmd[11] = recvBroadcasts ? 0x01u : 0x00u;

    if (ctx->verbose) {
        printf("[arcnet] arc_init: command (%lu bytes):", (ULONG)sizeof(cmd));
        for (i = 0; i < sizeof(cmd); i++) printf(" %02X", cmd[i]);
        printf("\n");
    }

    r = write_cmd(ctx, cmd, sizeof(cmd));
    if (r != ARC_OK) goto done;

    memset(resp, 0, sizeof(resp));
    r = read_response(ctx, ARC_OPCODE_INIT, resp, sizeof(resp),
                      ARC_BUDGET_INIT_MS, &xferred);
    if (r != ARC_OK) goto done;

    if (ctx->verbose) {
        printf("[arcnet] arc_init: response (%lu bytes):", xferred);
        for (i = 0; i < xferred; i++) printf(" %02X", resp[i]);
        printf("\n");
        if (xferred >= 6)
            printf("[arcnet] arc_init: status byte = 0x%02X%s\n",
                   resp[4], resp[4] == 0x00 ? " (zero)" : " (non-zero, OK)");
    }

    if (!WinUsb_SetPipePolicy(ctx->usb_handle, ARC_EP_RX_IN,
                               PIPE_TRANSFER_TIMEOUT,
                               sizeof(rx_timeout), &rx_timeout))
        VLOG_ERR(ctx, "arc_init: SetPipePolicy EP0x86 warning: %lu\n", GetLastError());

    VLOG(ctx, "arc_init: OK (nodeID=0x%02X)\n", nodeID);
    r = ARC_OK;

done:
    LeaveCriticalSection(&ctx->lock);
    return r;
}

/* =======================================================================
 * arc_reopen
 *   Closes dead handles, clears device_gone, and tries to re-establish
 *   the WinUSB session using the path stored by arc_open().
 *   Returns ARC_OK on success (caller must still call arc_init()).
 *   Returns ARC_ERR_DEVICE_GONE if the device is still absent.
 * ===================================================================== */
arc_result_t arc_reopen(arc_ctx_t *ctx)
{
    arc_result_t r;
    ULONG        timeout_ms = ARC_READ_TIMEOUT_MS;
    DWORD        err;

    if (!ctx) return ARC_ERR_OPEN;
    EnterCriticalSection(&ctx->lock);

    VLOG(ctx, "arc_reopen: closing dead handles\n");

    /* Release existing (possibly dead) handles — ignore errors */
    if (ctx->usb_handle) {
        WinUsb_Free(ctx->usb_handle);
        ctx->usb_handle = NULL;
    }
    if (ctx->dev_handle && ctx->dev_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(ctx->dev_handle);
        ctx->dev_handle = NULL;
    }
    ctx->device_gone = false;

    /* If no path was stored (shouldn't happen), try auto-detect */
    if (ctx->device_path[0] == '\0') {
        char found[1][256];
        if (enum_device_paths(ARC_VID, ARC_PID, found, 1) == 0) {
            VLOG(ctx, "arc_reopen: no device found (still absent)\n");
            ctx->device_gone = true;
            r = ARC_ERR_DEVICE_GONE;
            goto done;
        }
        strncpy(ctx->device_path, found[0], sizeof(ctx->device_path) - 1);
        ctx->device_path[sizeof(ctx->device_path) - 1] = '\0';
    }

    VLOG(ctx, "arc_reopen: trying %s\n", ctx->device_path);

    ctx->dev_handle = CreateFileA(ctx->device_path,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);

    if (ctx->dev_handle == INVALID_HANDLE_VALUE) {
        err = GetLastError();
        VLOG(ctx, "arc_reopen: CreateFile failed GLE=%lu (device still absent)\n", err);
        ctx->dev_handle  = NULL;
        ctx->device_gone = true;
        r = ARC_ERR_DEVICE_GONE;
        goto done;
    }

    if (!WinUsb_Initialize(ctx->dev_handle, &ctx->usb_handle)) {
        err = GetLastError();
        VLOG_ERR(ctx, "arc_reopen: WinUsb_Initialize failed: %lu\n", err);
        CloseHandle(ctx->dev_handle);
        ctx->dev_handle  = NULL;
        ctx->usb_handle  = NULL;
        ctx->device_gone = true;
        r = ARC_ERR_DEVICE_GONE;
        goto done;
    }

    /* Restore EP0x81 timeout policy */
    if (!WinUsb_SetPipePolicy(ctx->usb_handle, ARC_EP_EVT_IN,
                               PIPE_TRANSFER_TIMEOUT,
                               sizeof(timeout_ms), &timeout_ms))
        VLOG_ERR(ctx, "arc_reopen: SetPipePolicy EP0x81 warning: %lu\n", GetLastError());

    VLOG(ctx, "arc_reopen: OK — call arc_init() to reinitialize the node\n");
    r = ARC_OK;

done:
    LeaveCriticalSection(&ctx->lock);
    return r;
}

/* =======================================================================
 * arc_register
 * ===================================================================== */
arc_result_t arc_register(arc_ctx_t *ctx, bool bWrite, uint8_t reg, uint8_t *value)
{
    arc_result_t r;
    BYTE         cmd[5];
    BYTE         resp[ARC_EP_EVT_MAXPACKET];
    ULONG        xferred;

    if (!ctx)   return ARC_ERR_OPEN;
    if (!value) return ARC_ERR_PARAM;
    EnterCriticalSection(&ctx->lock);

    if (ctx->device_gone) { r = ARC_ERR_DEVICE_GONE; goto done; }

    cmd[0] = ARC_OPCODE_REGISTER;
    cmd[1] = 0x00;
    cmd[2] = bWrite ? 0x01u : 0x00u;
    cmd[3] = reg;
    cmd[4] = bWrite ? *value : 0x00u;

    r = write_cmd(ctx, cmd, sizeof(cmd));
    if (r != ARC_OK) goto done;

    memset(resp, 0, sizeof(resp));
    r = read_response(ctx, ARC_OPCODE_REGISTER, resp, sizeof(resp),
                      ARC_BUDGET_SHORT_MS, &xferred);
    if (r != ARC_OK) goto done;

    if (xferred < 7) {
        VLOG_ERR(ctx, "arc_register: response too short: %lu bytes\n", xferred);
        r = ARC_ERR_IO;
        goto done;
    }

    if (resp[4] != cmd[2] || resp[5] != reg) {
        VLOG_ERR(ctx, "arc_register: echo mismatch "
                 "(bWrite_echo=0x%02X reg_echo=0x%02X, sent 0x%02X 0x%02X)\n",
                 resp[4], resp[5], cmd[2], reg);
        r = ARC_ERR_ECHO;
        goto done;
    }

    if (!bWrite)
        *value = resp[6];

    r = ARC_OK;

done:
    LeaveCriticalSection(&ctx->lock);
    return r;
}

/* =======================================================================
 * arc_transmit
 *
 * The ACK poll loop calls arc_register(), which re-enters ctx->lock.
 * This is safe because Windows CRITICAL_SECTION is recursive: the same
 * thread can Enter multiple times and must Leave an equal number of times.
 * ===================================================================== */
arc_result_t arc_transmit(arc_ctx_t *ctx, uint8_t destNode,
                          const uint8_t *data, int len, bool waitAck)
{
    arc_result_t r;
    BYTE         buf[254];
    ULONG        xferred;
    ULONG        tx_timeout = ARC_TRANSMIT_TIMEOUT_MS;
    DWORD        err;
    int          i;
    ULONGLONG    ack_start;
    uint8_t      reg0;

    if (!ctx) return ARC_ERR_OPEN;
    if (!data || len < 1 || len > 252) {
        VLOG_ERR(ctx, "arc_transmit: invalid argument (data=%p len=%d)\n",
                 (void *)data, len);
        return ARC_ERR_PARAM;
    }
    EnterCriticalSection(&ctx->lock);

    if (ctx->device_gone) { r = ARC_ERR_DEVICE_GONE; goto done; }

    /*
     * arc_receive leaves EP_TX_OUT with a 150 ms PIPE_TRANSFER_TIMEOUT and may
     * have left it in a dirty state after a poll timeout.  Flush it and reset
     * the timeout to something generous before sending the real frame.
     */
    VLOG(ctx, "arc_transmit: flushing EP0x02 before transmit\n");
    pipe_flush(ctx, ARC_EP_TX_OUT);

    if (ctx->device_gone) { r = ARC_ERR_DEVICE_GONE; goto done; }

    if (!WinUsb_SetPipePolicy(ctx->usb_handle, ARC_EP_TX_OUT,
                               PIPE_TRANSFER_TIMEOUT,
                               sizeof(tx_timeout), &tx_timeout))
        VLOG_ERR(ctx, "arc_transmit: SetPipePolicy TX_OUT err=%lu\n", GetLastError());

    buf[0] = destNode;
    buf[1] = (BYTE)((256 - len) & 0xFF);
    memcpy(buf + 2, data, (size_t)len);

    if (ctx->verbose) {
        printf("[arcnet] arc_transmit: dest=0x%02X len=%d payload:", destNode, len);
        for (i = 0; i < len; i++) printf(" %02X", data[i]);
        printf("\n");
    }

    xferred = 0;
    if (!WinUsb_WritePipe(ctx->usb_handle, ARC_EP_TX_OUT,
                           buf, (ULONG)(len + 2), &xferred, NULL)) {
        err = GetLastError();
        if (is_gone_error(err)) {
            ctx->device_gone = true;
            VLOG_ERR(ctx, "arc_transmit: device gone GLE=%lu\n", err);
            r = ARC_ERR_DEVICE_GONE;
            goto done;
        }
        if (err == ERROR_SEM_TIMEOUT) {
            /* ARCNET RECON or transmitter not yet available — transient, not a hw error */
            VLOG(ctx, "arc_transmit: WritePipe EP0x02 timeout (GLE=121) -> NET_BUSY\n");
            pipe_flush(ctx, ARC_EP_TX_OUT);
            r = ARC_ERR_NET_BUSY;
            goto done;
        }
        VLOG_ERR(ctx, "arc_transmit: WritePipe EP0x02 failed: %lu\n", err);
        pipe_flush(ctx, ARC_EP_TX_OUT);
        r = ARC_ERR_IO;
        goto done;
    }
    if (xferred != (ULONG)(len + 2)) {
        VLOG_ERR(ctx, "arc_transmit: short write %lu/%d\n", xferred, len + 2);
        r = ARC_ERR_IO;
        goto done;
    }
    VLOG(ctx, "arc_transmit: sent %lu bytes to EP0x02\n", xferred);

    if (!waitAck) {
        VLOG(ctx, "arc_transmit: waitAck=false, returning ARC_OK without ACK check\n");
        r = ARC_OK;
        goto done;
    }

    /*
     * ACK detection: COM20022 status register 0, bit 1 = TMA
     * (Transmit Message Acknowledged — UPDATE 8).
     * Poll for up to ARC_ACK_POLL_BUDGET_MS; return ARC_OK on first
     * read where (reg0 & 0x02) is set, ARC_NOT_ACKED if budget expires.
     */
    ack_start = GetTickCount64();
    while ((GetTickCount64() - ack_start) < ARC_ACK_POLL_BUDGET_MS) {
        reg0 = 0;
        if (arc_register(ctx, false, 0, &reg0) == ARC_OK) {
            VLOG(ctx, "arc_transmit: ACK poll reg0=0x%02X TMA=%d (+%lu ms)\n",
                 reg0, (reg0 >> 1) & 1,
                 (ULONG)(GetTickCount64() - ack_start));
            if (reg0 & 0x02) {
                VLOG(ctx, "arc_transmit: ACK received (TMA set) -> ARC_OK\n");
                r = ARC_OK;
                goto done;
            }
        }
        Sleep(ARC_ACK_POLL_INTERVAL_MS);
    }

    VLOG(ctx, "arc_transmit: TMA never set in %u ms -> ARC_NOT_ACKED\n",
         ARC_ACK_POLL_BUDGET_MS);
    r = ARC_NOT_ACKED;

done:
    LeaveCriticalSection(&ctx->lock);
    return r;
}

/* =======================================================================
 * arc_read_event
 *   Passive read from EP_EVT_IN (0x81) — no command sent first.
 *   Used to capture unsolicited post-transmit status/event packets.
 * ===================================================================== */
arc_result_t arc_read_event(arc_ctx_t *ctx, uint8_t *buf, int bufsize,
                             uint32_t timeout_ms, int *out_len)
{
    arc_result_t r;
    ULONG        xferred = 0;
    ULONG        to      = (ULONG)timeout_ms;
    DWORD        err;

    if (!ctx || !buf || bufsize <= 0 || !out_len) return ARC_ERR_PARAM;
    EnterCriticalSection(&ctx->lock);

    if (ctx->device_gone) { r = ARC_ERR_DEVICE_GONE; goto done; }
    *out_len = 0;

    if (timeout_ms > 0) {
        if (!WinUsb_SetPipePolicy(ctx->usb_handle, ARC_EP_EVT_IN,
                                   PIPE_TRANSFER_TIMEOUT, sizeof(to), &to))
            VLOG_ERR(ctx, "arc_read_event: SetPipePolicy err=%lu\n", GetLastError());
    }

    if (!WinUsb_ReadPipe(ctx->usb_handle, ARC_EP_EVT_IN,
                          (PUCHAR)buf, (ULONG)bufsize, &xferred, NULL)) {
        err = GetLastError();
        if (err == ERROR_SEM_TIMEOUT) { r = ARC_NO_PACKET; goto done; }
        if (is_gone_error(err)) {
            ctx->device_gone = true;
            VLOG_ERR(ctx, "arc_read_event: device gone GLE=%lu\n", err);
            r = ARC_ERR_DEVICE_GONE;
            goto done;
        }
        VLOG_ERR(ctx, "arc_read_event: ReadPipe EP0x81 err=%lu\n", err);
        r = ARC_ERR_IO;
        goto done;
    }

    *out_len = (int)xferred;
    r = (xferred > 0) ? ARC_OK : ARC_NO_PACKET;

done:
    LeaveCriticalSection(&ctx->lock);
    return r;
}

/* =======================================================================
 * arc_receive
 *
 * Non-blocking contract: every call returns within ~2 × ARC_RECEIVE_TIMEOUT_MS.
 *
 * Pipe hygiene rule: any transfer error or timeout on either pipe leaves that
 * pipe in an undefined USB data-toggle state.  pipe_flush() (AbortPipe +
 * ResetPipe) is called immediately after every failure so the next call
 * always starts with a clean pipe.
 *
 * err=121 (ERROR_SEM_TIMEOUT) on EP0x86 is NORMAL — it simply means no
 * ARCNET packet was waiting.  It is not treated as a hardware error.
 * Only unexpected errors (not 121) on EP0x86 return ARC_ERR_IO.
 * ===================================================================== */
arc_result_t arc_receive(arc_ctx_t *ctx, uint8_t *src, uint8_t *dst,
                         uint8_t *data, int *len)
{
    arc_result_t r;
    BYTE         poll[10];
    BYTE         buf[RX_BUF_SIZE];
    ULONG        xferred;
    ULONG        pipe_to = ARC_RECEIVE_TIMEOUT_MS;
    DWORD        err;
    BOOL         ok;
    int          L;

    if (!ctx)                          return ARC_ERR_OPEN;
    if (!src || !dst || !data || !len) return ARC_ERR_PARAM;
    EnterCriticalSection(&ctx->lock);

    if (ctx->device_gone) { r = ARC_ERR_DEVICE_GONE; goto done; }

    /* ------------------------------------------------------------------ */
    /* Apply short timeouts so neither write nor read can block            */
    /* ------------------------------------------------------------------ */
    if (!WinUsb_SetPipePolicy(ctx->usb_handle, ARC_EP_TX_OUT,
                               PIPE_TRANSFER_TIMEOUT, sizeof(pipe_to), &pipe_to))
        VLOG_ERR(ctx, "arc_receive: SetPipePolicy TX_OUT err=%lu\n", GetLastError());

    if (!WinUsb_SetPipePolicy(ctx->usb_handle, ARC_EP_RX_IN,
                               PIPE_TRANSFER_TIMEOUT, sizeof(pipe_to), &pipe_to))
        VLOG_ERR(ctx, "arc_receive: SetPipePolicy RX_IN err=%lu\n", GetLastError());

    /* ------------------------------------------------------------------ */
    /* 1. Poll: write 10 zero bytes to EP 0x02 to trigger receive check   */
    /* ------------------------------------------------------------------ */
    memset(poll, 0x00, sizeof(poll));
    xferred = 0;
    ok  = WinUsb_WritePipe(ctx->usb_handle, ARC_EP_TX_OUT,
                            poll, sizeof(poll), &xferred, NULL);
    err = GetLastError();
    if (!ok) {
        if (is_gone_error(err)) {
            ctx->device_gone = true;
            VLOG_ERR(ctx, "arc_receive: device gone on WritePipe EP0x02 GLE=%lu\n", err);
            r = ARC_ERR_DEVICE_GONE;
            goto done;
        }
        VLOG(ctx, "arc_receive: WritePipe EP0x02 err=%lu -> flush+ARC_NO_PACKET\n", err);
        pipe_flush(ctx, ARC_EP_TX_OUT);
        r = ARC_NO_PACKET;
        goto done;
    }
    VLOG(ctx, "arc_receive: poll wrote %lu bytes to EP0x02\n", xferred);

    /* ------------------------------------------------------------------ */
    /* 2. Read from EP 0x86                                               */
    /* err=121 (timeout) = no ARCNET packet waiting — not a hw error.     */
    /* Any other error is unexpected; flush and report ARC_ERR_IO.        */
    /* ------------------------------------------------------------------ */
    memset(buf, 0, sizeof(buf));
    xferred = 0;
    ok  = WinUsb_ReadPipe(ctx->usb_handle, ARC_EP_RX_IN,
                           buf, sizeof(buf), &xferred, NULL);
    err = GetLastError();
    if (!ok) {
        if (err == ERROR_SEM_TIMEOUT) {
            VLOG(ctx, "arc_receive: ReadPipe EP0x86 timeout (normal, no packet) -> flush\n");
            pipe_flush(ctx, ARC_EP_RX_IN);
            r = ARC_NO_PACKET;
            goto done;
        }
        if (is_gone_error(err)) {
            ctx->device_gone = true;
            VLOG_ERR(ctx, "arc_receive: device gone on ReadPipe EP0x86 GLE=%lu\n", err);
            r = ARC_ERR_DEVICE_GONE;
            goto done;
        }
        VLOG_ERR(ctx, "arc_receive: ReadPipe EP0x86 err=%lu -> flush+ARC_ERR_IO\n", err);
        pipe_flush(ctx, ARC_EP_RX_IN);
        r = ARC_ERR_IO;
        goto done;
    }
    VLOG(ctx, "arc_receive: read %lu bytes from EP0x86\n", xferred);

    /* ------------------------------------------------------------------ */
    /* 3. Empty-response check                                            */
    /* ------------------------------------------------------------------ */
    if (xferred < 3 || (buf[0] == 0 && buf[1] == 0)) {
        r = ARC_NO_PACKET;
        goto done;
    }

    /* ------------------------------------------------------------------ */
    /* 4. Parse: [src][dst][count][data...]                               */
    /* ------------------------------------------------------------------ */
    *src = buf[0];
    *dst = buf[1];
    L    = (256 - (int)buf[2]) & 0xFF;

    if (L == 0 || (int)xferred < L + 3) {
        r = ARC_NO_PACKET;
        goto done;
    }

    *len = L;
    memcpy(data, buf + 3, (size_t)L);
    r = ARC_OK;

done:
    LeaveCriticalSection(&ctx->lock);
    return r;
}
