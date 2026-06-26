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
 *   - Concurrent calls on DIFFERENT contexts are fully parallel.
 *   - Concurrent calls on the SAME context serialize.
 *   - CRITICAL_SECTION is recursive — internal calls (e.g. arc_transmit
 *     calling arc_register for ACK polling) never deadlock.
 *   - arc_close() must not be called while another thread is still inside
 *     any API function on the same context.
 *
 * Logging: per-context level + optional callback (see arc_set_log_level,
 *   arc_set_log_callback).  Default level ARC_LOG_NONE = completely silent.
 */

#include "arcnet.h"
#include <windows.h>
#include <setupapi.h>
#include <winusb.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

#define RX_BUF_SIZE          512u
#define ARC_QUEUE_CAP        256u   /* ring buffer capacity (packets)      */
#define ARC_LISTEN_POLL_MS    20u   /* EP timeout per listener poll cycle  */
#define ARC_LISTEN_SLEEP_MS    5u   /* sleep after ARC_NO_PACKET           */

/* -----------------------------------------------------------------------
 * Internal ring buffer for the async listener
 * --------------------------------------------------------------------- */
typedef struct {
    arc_packet_t pkts[ARC_QUEUE_CAP];
    int          head;     /* next write index  */
    int          tail;     /* next read index   */
    int          count;    /* packets in queue  */
    int          dropped;  /* dropped (queue full) */
} arc_queue_t;

/* -----------------------------------------------------------------------
 * Per-device context (opaque to callers; defined here only)
 * --------------------------------------------------------------------- */
struct arc_ctx_s {
    CRITICAL_SECTION        lock;         /* per-context recursive mutex  */
    HANDLE                  dev_handle;
    WINUSB_INTERFACE_HANDLE usb_handle;
    arc_log_level_t         log_level;   /* ARC_LOG_NONE by default       */
    arc_log_fn              log_fn;      /* NULL = write to stderr        */
    void                   *log_user;   /* opaque for log_fn             */
    bool                    device_gone; /* set on fatal USB error        */
    char                    device_path[256];

    /* ---- async receive (arc_listen_start / arc_listen_stop) ---- */
    CRITICAL_SECTION        queue_lock;   /* protects queue, recv_fn/user */
    arc_queue_t             queue;
    arc_recv_fn             recv_fn;
    void                   *recv_user;
    HANDLE                  listen_thread; /* NULL = not running           */
    volatile LONG           listen_stop;   /* set to 1 to signal exit      */
};

/* -----------------------------------------------------------------------
 * arc_log  --  internal; may be called with ctx->lock held or (in arc_open)
 * before the context is visible to other threads.
 * --------------------------------------------------------------------- */
static void arc_log(arc_ctx_t *ctx, arc_log_level_t level,
                    const char *fmt, ...)
{
    char    buf[512];
    va_list ap;
    if (!ctx || level > ctx->log_level) return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (ctx->log_fn)
        ctx->log_fn(level, buf, ctx->log_user);
    else
        fputs(buf, stderr);
}

/* arc_log_hex  --  log a hex byte dump as a single message */
static void arc_log_hex(arc_ctx_t *ctx, arc_log_level_t level,
                         const char *prefix, const BYTE *data, ULONG len)
{
    char  buf[768];
    int   pos;
    ULONG i;
    if (!ctx || level > ctx->log_level) return;
    pos = snprintf(buf, sizeof(buf), "[arcnet] %s (%lu bytes):", prefix, len);
    for (i = 0; i < len && pos < (int)sizeof(buf) - 4; i++)
        pos += snprintf(buf + pos, sizeof(buf) - pos, " %02X", data[i]);
    if (pos < (int)sizeof(buf) - 1) { buf[pos++] = '\n'; buf[pos] = '\0'; }
    if (ctx->log_fn) ctx->log_fn(level, buf, ctx->log_user);
    else             fputs(buf, stderr);
}

#define LOG(ctx, lvl, fmt, ...)  arc_log((ctx), (lvl), "[arcnet] " fmt, ##__VA_ARGS__)
#define LERR(ctx, fmt, ...)      LOG((ctx), ARC_LOG_ERROR, fmt, ##__VA_ARGS__)
#define LINFO(ctx, fmt, ...)     LOG((ctx), ARC_LOG_INFO,  fmt, ##__VA_ARGS__)
#define LDBG(ctx, fmt, ...)      LOG((ctx), ARC_LOG_DEBUG, fmt, ##__VA_ARGS__)

/* -----------------------------------------------------------------------
 * is_gone_error
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
 * pipe_flush  --  caller must hold ctx->lock.
 * --------------------------------------------------------------------- */
static void pipe_flush(arc_ctx_t *ctx, UCHAR ep)
{
    DWORD err;
    if (ctx->device_gone) return;
    if (!WinUsb_AbortPipe(ctx->usb_handle, ep)) {
        err = GetLastError();
        if (is_gone_error(err)) {
            ctx->device_gone = true;
            LERR(ctx, "pipe_flush: device gone on AbortPipe EP0x%02X GLE=%lu\n", ep, err);
            return;
        }
        LERR(ctx, "pipe_flush: AbortPipe EP0x%02X FAIL GLE=%lu\n", ep, err);
    } else {
        LDBG(ctx, "pipe_flush: AbortPipe EP0x%02X OK\n", ep);
    }
    if (ctx->device_gone) return;
    if (!WinUsb_ResetPipe(ctx->usb_handle, ep)) {
        err = GetLastError();
        if (is_gone_error(err)) {
            ctx->device_gone = true;
            LERR(ctx, "pipe_flush: device gone on ResetPipe EP0x%02X GLE=%lu\n", ep, err);
            return;
        }
        LERR(ctx, "pipe_flush: ResetPipe EP0x%02X FAIL GLE=%lu\n", ep, err);
    } else {
        LDBG(ctx, "pipe_flush: ResetPipe EP0x%02X OK\n", ep);
    }
}

/* {E6B4B5C0-F74E-4A1D-9B8F-2C3D4E5F6A7B} */
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
    InitializeCriticalSection(&ctx->queue_lock);
    /* calloc zeroed all other fields (queue, listen_thread, recv_fn, …) */
    ctx->log_level = verbose ? ARC_LOG_DEBUG : ARC_LOG_NONE;

    strncpy(ctx->device_path, path, sizeof(ctx->device_path) - 1);
    ctx->device_path[sizeof(ctx->device_path) - 1] = '\0';

    LDBG(ctx, "arc_open: %s\n", path);

    ctx->dev_handle = CreateFileA(path,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);

    if (ctx->dev_handle == INVALID_HANDLE_VALUE) {
        LERR(ctx, "arc_open: CreateFile failed GLE=%lu\n", GetLastError());
        DeleteCriticalSection(&ctx->lock);
        free(ctx);
        return NULL;
    }

    if (!WinUsb_Initialize(ctx->dev_handle, &ctx->usb_handle)) {
        LERR(ctx, "arc_open: WinUsb_Initialize failed GLE=%lu\n", GetLastError());
        CloseHandle(ctx->dev_handle);
        DeleteCriticalSection(&ctx->lock);
        free(ctx);
        return NULL;
    }

    if (!WinUsb_SetPipePolicy(ctx->usb_handle, ARC_EP_EVT_IN,
                               PIPE_TRANSFER_TIMEOUT,
                               sizeof(timeout_ms), &timeout_ms))
        LERR(ctx, "arc_open: SetPipePolicy EP0x81 warning GLE=%lu\n", GetLastError());

    LINFO(ctx, "arc_open: OK\n");
    return ctx;
}

/* =======================================================================
 * arc_set_log_level
 * ===================================================================== */
void arc_set_log_level(arc_ctx_t *ctx, arc_log_level_t level)
{
    if (!ctx) return;
    EnterCriticalSection(&ctx->lock);
    ctx->log_level = level;
    LeaveCriticalSection(&ctx->lock);
}

/* =======================================================================
 * arc_set_log_callback
 * ===================================================================== */
void arc_set_log_callback(arc_ctx_t *ctx, arc_log_fn fn, void *user)
{
    if (!ctx) return;
    EnterCriticalSection(&ctx->lock);
    ctx->log_fn   = fn;
    ctx->log_user = user;
    LeaveCriticalSection(&ctx->lock);
}

/* =======================================================================
 * arc_set_verbose  --  backward-compatible wrapper
 * ===================================================================== */
void arc_set_verbose(arc_ctx_t *ctx, bool enable)
{
    arc_set_log_level(ctx, enable ? ARC_LOG_DEBUG : ARC_LOG_NONE);
}

/* =======================================================================
 * arc_close
 * ===================================================================== */
void arc_close(arc_ctx_t *ctx)
{
    if (!ctx) return;
    arc_listen_stop(ctx);  /* no-op if listener is not running */

    EnterCriticalSection(&ctx->lock);
    if (ctx->usb_handle) { WinUsb_Free(ctx->usb_handle); ctx->usb_handle = NULL; }
    if (ctx->dev_handle) { CloseHandle(ctx->dev_handle);  ctx->dev_handle = NULL; }
    LINFO(ctx, "arc_close: done\n");
    LeaveCriticalSection(&ctx->lock);
    DeleteCriticalSection(&ctx->lock);
    DeleteCriticalSection(&ctx->queue_lock);
    free(ctx);
}

/* =======================================================================
 * arc_shutdown
 * ===================================================================== */
arc_result_t arc_shutdown(arc_ctx_t *ctx)
{
    uint8_t rst;
    arc_result_t r;

    if (!ctx) return ARC_ERR_OPEN;

    /* Best-effort RST: arc_register checks device_gone internally and
     * returns ARC_ERR_DEVICE_GONE immediately if the device is gone. */
    rst = 0x80u;  /* COM20022 reg0 bit7 = RST (software reset) */
    r = arc_register(ctx, true, 0, &rst);
    if (r == ARC_OK) {
        LINFO(ctx, "arc_shutdown: RST written -> node off ARCNET bus\n");
        Sleep(50u);  /* allow RECON to propagate before USB teardown */
    } else {
        LINFO(ctx, "arc_shutdown: RST skipped (%s) -> close anyway\n",
              arc_result_str(r));
    }

    arc_close(ctx);  /* releases USB handles, frees ctx */
    return ARC_OK;
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
            LERR(ctx, "write_cmd: device gone GLE=%lu\n", err);
            return ARC_ERR_DEVICE_GONE;
        }
        LERR(ctx, "write_cmd: WritePipe EP0x01 failed GLE=%lu\n", err);
        return ARC_ERR_IO;
    }
    if (xferred != len) {
        LERR(ctx, "write_cmd: short write %lu/%lu\n", xferred, len);
        return ARC_ERR_IO;
    }
    return ARC_OK;
}

/* =======================================================================
 * read_response  --  internal; caller must hold ctx->lock.
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
                LERR(ctx, "read_response: device gone GLE=%lu\n", err);
                return ARC_ERR_DEVICE_GONE;
            }
            LERR(ctx, "read_response: ReadPipe EP0x81 failed GLE=%lu\n", err);
            return ARC_ERR_IO;
        }
        if (xferred == 0) continue;
        if (buf[0] == expected_opcode) { *out_len = xferred; return ARC_OK; }
        if (buf[0] == ARC_OPCODE_EVENT)
            LDBG(ctx, "read_response: skipping 0x20 event at %.1f s\n",
                 (double)(GetTickCount64() - start) / 1000.0);
        else
            LDBG(ctx, "read_response: skipping opcode 0x%02X at %.1f s\n",
                 buf[0], (double)(GetTickCount64() - start) / 1000.0);
    }
    LERR(ctx, "read_response: budget %lu ms exhausted waiting for opcode 0x%02X\n",
         budget_ms, expected_opcode);
    return ARC_ERR_TIMEOUT;
}

/* =======================================================================
 * cmd04_internal  --  caller must hold ctx->lock.
 * ===================================================================== */
static arc_result_t cmd04_internal(arc_ctx_t *ctx)
{
    BYTE         cmd[2] = { ARC_OPCODE_CMD04, 0x00 };
    BYTE         resp[ARC_EP_EVT_MAXPACKET];
    ULONG        xferred;
    arc_result_t r;

    LDBG(ctx, "cmd04: sending 04 00\n");
    r = write_cmd(ctx, cmd, sizeof(cmd));
    if (r != ARC_OK) return r;

    memset(resp, 0, sizeof(resp));
    r = read_response(ctx, ARC_OPCODE_CMD04, resp, sizeof(resp),
                      ARC_BUDGET_SHORT_MS, &xferred);
    if (r != ARC_OK) return r;

    arc_log_hex(ctx, ARC_LOG_DEBUG, "cmd04: response", resp, xferred);

    if (xferred < 4 || resp[1] || resp[2] || resp[3])
        LDBG(ctx, "cmd04: response differs from 04 00 00 00 (continuing)\n");

    return ARC_OK;
}

/* =======================================================================
 * handshake_internal  --  caller must hold ctx->lock.
 * ===================================================================== */
static arc_result_t handshake_internal(arc_ctx_t *ctx)
{
    BYTE  out_buf[10];
    BYTE  in_buf[RX_BUF_SIZE];
    ULONG xferred;
    ULONG drain_timeout = 50u;   /* short per-read so the loop exits fast when empty */
    DWORD err;
    int   drained = 0;

    /* Drain loop: poll EP0x02/EP0x86 in pairs until the device replies with
     * an all-zero (no-pending-packet) response or the read times out.  Each
     * extra iteration discards one stale packet left in the device buffer by a
     * previous session.  50 ms per attempt means a clean channel exits in one
     * timeout instead of the original 500 ms single-read. */
    if (!WinUsb_SetPipePolicy(ctx->usb_handle, ARC_EP_RX_IN,
                               PIPE_TRANSFER_TIMEOUT,
                               sizeof(drain_timeout), &drain_timeout))
        LERR(ctx, "handshake: SetPipePolicy EP0x86 warning GLE=%lu\n", GetLastError());
    if (!WinUsb_SetPipePolicy(ctx->usb_handle, ARC_EP_TX_OUT,
                               PIPE_TRANSFER_TIMEOUT,
                               sizeof(drain_timeout), &drain_timeout))
        LERR(ctx, "handshake: SetPipePolicy EP0x02 warning GLE=%lu\n", GetLastError());

    memset(out_buf, 0x00, sizeof(out_buf));

    for (;;) {
        if (ctx->device_gone) return ARC_ERR_DEVICE_GONE;

        xferred = 0;
        if (!WinUsb_WritePipe(ctx->usb_handle, ARC_EP_TX_OUT,
                               out_buf, sizeof(out_buf), &xferred, NULL)) {
            err = GetLastError();
            if (err == ERROR_SEM_TIMEOUT) {
                LDBG(ctx, "handshake: EP0x02 poll timeout -- stopping drain\n");
                break;
            }
            if (is_gone_error(err)) {
                ctx->device_gone = true;
                LERR(ctx, "handshake: device gone on WritePipe GLE=%lu\n", err);
                return ARC_ERR_DEVICE_GONE;
            }
            LDBG(ctx, "handshake: WritePipe EP0x02 GLE=%lu -- stopping drain\n", err);
            break;
        }

        memset(in_buf, 0, sizeof(in_buf));
        xferred = 0;
        if (!WinUsb_ReadPipe(ctx->usb_handle, ARC_EP_RX_IN,
                              in_buf, sizeof(in_buf), &xferred, NULL)) {
            err = GetLastError();
            if (err == ERROR_SEM_TIMEOUT) {
                LDBG(ctx, "handshake: EP0x86 timeout -- channel empty\n");
                break;
            }
            if (is_gone_error(err)) {
                ctx->device_gone = true;
                LERR(ctx, "handshake: device gone on ReadPipe GLE=%lu\n", err);
                return ARC_ERR_DEVICE_GONE;
            }
            LDBG(ctx, "handshake: ReadPipe EP0x86 GLE=%lu -- stopping drain\n", err);
            break;
        }

        /* src=0 dst=0: device has no queued ARCNET packet, channel is clear. */
        if (xferred == 0 || (in_buf[0] == 0 && in_buf[1] == 0)) {
            LDBG(ctx, "handshake: EP0x86 empty response -- channel clear\n");
            break;
        }

        drained++;
        arc_log_hex(ctx, ARC_LOG_DEBUG,
                    "handshake: discarded stale packet", in_buf, xferred);
    }

    if (drained > 0)
        LINFO(ctx, "handshake: drained %d stale packet(s) from previous session\n", drained);
    else
        LDBG(ctx, "handshake: EP0x86 was clean\n");

    return ARC_OK;
}

/* =======================================================================
 * arc_init
 * ===================================================================== */
arc_result_t arc_init(arc_ctx_t *ctx, uint8_t nodeID, uint8_t timeout,
                      uint8_t clockPrescaler, bool recvBroadcasts)
{
    arc_result_t r;
    BYTE         cmd[12];
    BYTE         resp[ARC_EP_EVT_MAXPACKET];
    ULONG        xferred;
    ULONG        rx_timeout = ARC_RECEIVE_TIMEOUT_MS;

    if (!ctx) return ARC_ERR_OPEN;
    EnterCriticalSection(&ctx->lock);

    if (ctx->device_gone) { r = ARC_ERR_DEVICE_GONE; goto done; }

    LDBG(ctx, "arc_init: flushing data pipes\n");
    pipe_flush(ctx, ARC_EP_TX_OUT);
    pipe_flush(ctx, ARC_EP_RX_IN);
    if (ctx->device_gone) { r = ARC_ERR_DEVICE_GONE; goto done; }

    r = cmd04_internal(ctx);
    if (r != ARC_OK) goto done;

    r = handshake_internal(ctx);
    if (r != ARC_OK) goto done;

    cmd[0]  = ARC_OPCODE_INIT; cmd[1]  = 0x00;
    cmd[2]  = 0x00;            cmd[3]  = 0x00;
    cmd[4]  = 0x00;            cmd[5]  = timeout;
    cmd[6]  = nodeID;          cmd[7]  = 0x01;
    cmd[8]  = 0x00;            cmd[9]  = clockPrescaler;
    cmd[10] = (clockPrescaler > 5u) ? 0x01u : 0x00u;
    cmd[11] = recvBroadcasts ? 0x01u : 0x00u;

    arc_log_hex(ctx, ARC_LOG_DEBUG, "arc_init: command", cmd, (ULONG)sizeof(cmd));

    r = write_cmd(ctx, cmd, sizeof(cmd));
    if (r != ARC_OK) goto done;

    memset(resp, 0, sizeof(resp));
    r = read_response(ctx, ARC_OPCODE_INIT, resp, sizeof(resp),
                      ARC_BUDGET_INIT_MS, &xferred);
    if (r != ARC_OK) goto done;

    arc_log_hex(ctx, ARC_LOG_DEBUG, "arc_init: response", resp, xferred);
    if (xferred >= 6)
        LINFO(ctx, "arc_init: status=0x%02X\n", resp[4]);

    if (!WinUsb_SetPipePolicy(ctx->usb_handle, ARC_EP_RX_IN,
                               PIPE_TRANSFER_TIMEOUT,
                               sizeof(rx_timeout), &rx_timeout))
        LERR(ctx, "arc_init: SetPipePolicy EP0x86 warning GLE=%lu\n", GetLastError());

    LINFO(ctx, "arc_init: OK (nodeID=0x%02X)\n", nodeID);
    r = ARC_OK;

done:
    LeaveCriticalSection(&ctx->lock);
    return r;
}

/* =======================================================================
 * arc_reopen
 * ===================================================================== */
arc_result_t arc_reopen(arc_ctx_t *ctx)
{
    arc_result_t r;
    ULONG        timeout_ms = ARC_READ_TIMEOUT_MS;
    DWORD        err;

    if (!ctx) return ARC_ERR_OPEN;
    EnterCriticalSection(&ctx->lock);

    LDBG(ctx, "arc_reopen: closing dead handles\n");

    if (ctx->usb_handle) { WinUsb_Free(ctx->usb_handle);  ctx->usb_handle = NULL; }
    if (ctx->dev_handle && ctx->dev_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(ctx->dev_handle); ctx->dev_handle = NULL;
    }
    ctx->device_gone = false;

    if (ctx->device_path[0] == '\0') {
        char found[1][256];
        if (enum_device_paths(ARC_VID, ARC_PID, found, 1) == 0) {
            LINFO(ctx, "arc_reopen: no device found (still absent)\n");
            ctx->device_gone = true;
            r = ARC_ERR_DEVICE_GONE;
            goto done;
        }
        strncpy(ctx->device_path, found[0], sizeof(ctx->device_path) - 1);
        ctx->device_path[sizeof(ctx->device_path) - 1] = '\0';
    }

    LDBG(ctx, "arc_reopen: trying %s\n", ctx->device_path);

    ctx->dev_handle = CreateFileA(ctx->device_path,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);

    if (ctx->dev_handle == INVALID_HANDLE_VALUE) {
        err = GetLastError();
        LINFO(ctx, "arc_reopen: CreateFile failed GLE=%lu (device still absent)\n", err);
        ctx->dev_handle  = NULL;
        ctx->device_gone = true;
        r = ARC_ERR_DEVICE_GONE;
        goto done;
    }

    if (!WinUsb_Initialize(ctx->dev_handle, &ctx->usb_handle)) {
        err = GetLastError();
        LERR(ctx, "arc_reopen: WinUsb_Initialize failed GLE=%lu\n", err);
        CloseHandle(ctx->dev_handle);
        ctx->dev_handle  = NULL;
        ctx->usb_handle  = NULL;
        ctx->device_gone = true;
        r = ARC_ERR_DEVICE_GONE;
        goto done;
    }

    if (!WinUsb_SetPipePolicy(ctx->usb_handle, ARC_EP_EVT_IN,
                               PIPE_TRANSFER_TIMEOUT,
                               sizeof(timeout_ms), &timeout_ms))
        LERR(ctx, "arc_reopen: SetPipePolicy EP0x81 warning GLE=%lu\n", GetLastError());

    LINFO(ctx, "arc_reopen: OK -- call arc_init() to reinitialize\n");
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
        LERR(ctx, "arc_register: response too short: %lu bytes\n", xferred);
        r = ARC_ERR_IO;
        goto done;
    }
    if (resp[4] != cmd[2] || resp[5] != reg) {
        LERR(ctx, "arc_register: echo mismatch "
             "(bWrite_echo=0x%02X reg_echo=0x%02X, sent 0x%02X 0x%02X)\n",
             resp[4], resp[5], cmd[2], reg);
        r = ARC_ERR_ECHO;
        goto done;
    }
    if (!bWrite) *value = resp[6];
    r = ARC_OK;

done:
    LeaveCriticalSection(&ctx->lock);
    return r;
}

/* =======================================================================
 * arc_transmit
 *
 * The ACK poll calls arc_register() which re-enters ctx->lock.
 * CRITICAL_SECTION is recursive — same thread, no deadlock.
 * ===================================================================== */
arc_result_t arc_transmit(arc_ctx_t *ctx, uint8_t destNode,
                          const uint8_t *data, int len, bool waitAck)
{
    arc_result_t r;
    BYTE         buf[511];   /* max: 3-byte long-frame header + 508 bytes payload */
    ULONG        xferred;
    ULONG        total_len;
    ULONG        tx_timeout = ARC_TRANSMIT_TIMEOUT_MS;
    DWORD        err;
    ULONGLONG    ack_start;

    if (!ctx) return ARC_ERR_OPEN;
    if (!data || len < 1 || len > 508) {
        LERR(ctx, "arc_transmit: invalid argument (data=%p len=%d)\n",
             (void *)data, len);
        return ARC_ERR_PARAM;
    }
    EnterCriticalSection(&ctx->lock);

    if (ctx->device_gone) { r = ARC_ERR_DEVICE_GONE; goto done; }

    LDBG(ctx, "arc_transmit: flushing EP0x02 before transmit\n");
    pipe_flush(ctx, ARC_EP_TX_OUT);
    if (ctx->device_gone) { r = ARC_ERR_DEVICE_GONE; goto done; }

    if (!WinUsb_SetPipePolicy(ctx->usb_handle, ARC_EP_TX_OUT,
                               PIPE_TRANSFER_TIMEOUT,
                               sizeof(tx_timeout), &tx_timeout))
        LERR(ctx, "arc_transmit: SetPipePolicy TX_OUT GLE=%lu\n", GetLastError());

    if (len <= 253) {
        /* Short frame (UPDATE 2): [dst][256-L][data] */
        buf[0] = destNode;
        buf[1] = (BYTE)((256 - len) & 0xFF);
        memcpy(buf + 2, data, (size_t)len);
        total_len = (ULONG)(len + 2);
        LDBG(ctx, "arc_transmit: short frame dest=0x%02X len=%d header: %02X %02X\n",
             destNode, len, buf[0], buf[1]);
    } else {
        /* Long frame (UPDATE 9): [dst][0x00][512-L][data] */
        buf[0] = destNode;
        buf[1] = 0x00;
        buf[2] = (BYTE)((512 - len) & 0xFF);
        memcpy(buf + 3, data, (size_t)len);
        total_len = (ULONG)(len + 3);
        LDBG(ctx, "arc_transmit: long frame dest=0x%02X len=%d header: %02X %02X %02X\n",
             destNode, len, buf[0], buf[1], buf[2]);
    }

    /* Log payload (first bytes; truncated in log for very long payloads) */
    {
        char  hbuf[512]; int hpos; int k;
        hpos = snprintf(hbuf, sizeof(hbuf),
                        "[arcnet] arc_transmit: dest=0x%02X len=%d payload:", destNode, len);
        for (k = 0; k < len && hpos < (int)sizeof(hbuf) - 4; k++)
            hpos += snprintf(hbuf + hpos, sizeof(hbuf) - hpos, " %02X", data[k]);
        if (hpos < (int)sizeof(hbuf) - 1) { hbuf[hpos++] = '\n'; hbuf[hpos] = '\0'; }
        arc_log(ctx, ARC_LOG_DEBUG, "%s", hbuf);
    }

    xferred = 0;
    if (!WinUsb_WritePipe(ctx->usb_handle, ARC_EP_TX_OUT,
                           buf, total_len, &xferred, NULL)) {
        err = GetLastError();
        if (is_gone_error(err)) {
            ctx->device_gone = true;
            LERR(ctx, "arc_transmit: device gone GLE=%lu\n", err);
            r = ARC_ERR_DEVICE_GONE; goto done;
        }
        if (err == ERROR_SEM_TIMEOUT) {
            LINFO(ctx, "arc_transmit: WritePipe EP0x02 timeout (GLE=121) -> NET_BUSY\n");
            pipe_flush(ctx, ARC_EP_TX_OUT);
            r = ARC_ERR_NET_BUSY; goto done;
        }
        LERR(ctx, "arc_transmit: WritePipe EP0x02 failed GLE=%lu\n", err);
        pipe_flush(ctx, ARC_EP_TX_OUT);
        r = ARC_ERR_IO; goto done;
    }
    if (xferred != total_len) {
        LERR(ctx, "arc_transmit: short write %lu/%lu\n", xferred, total_len);
        r = ARC_ERR_IO; goto done;
    }
    LDBG(ctx, "arc_transmit: sent %lu bytes to EP0x02\n", xferred);

    if (!waitAck) {
        LDBG(ctx, "arc_transmit: waitAck=false -> ARC_OK\n");
        r = ARC_OK; goto done;
    }

    /* Event-driven ACK detection (UPDATE 10, refined UPDATE 11).
     * After the frame is written to EP0x02, the device firmware pushes a
     * 0x20 TX-complete event on EP0x81.  b4 bit structure (verified,
     * 3-scenario test_b4survey):
     *   bit0 (0x01): TX-complete flag -- 1=this event is for our TX,
     *                0=saf RECON/other (skip, keep waiting)
     *   bit1 (0x02): ACK -- 1=ACK received, 0=no ACK
     *   bit2 (0x04): RECON side-info -- does NOT affect ACK decision
     * Examples: 0x03=TX+ACK, 0x07=TX+ACK+RECON, 0x01=TX+noACK, 0x04=RECON(skip) */
    {
        BYTE      ev_buf[ARC_EP_EVT_MAXPACKET];
        ULONG     ev_xferred;
        ULONGLONG ack_deadline;
        ULONG     per_ms;
        BOOL      ev_ok;
        DWORD     ev_err;

        ack_start    = GetTickCount64();
        ack_deadline = ack_start + ARC_ACK_EVENT_TIMEOUT_MS;

        for (;;) {
            ULONGLONG now = GetTickCount64();
            if (now >= ack_deadline) {
                LINFO(ctx, "arc_transmit: no TX-event in %u ms -> ARC_NOT_ACKED\n",
                      ARC_ACK_EVENT_TIMEOUT_MS);
                r = ARC_NOT_ACKED; goto done;
            }
            per_ms = (ULONG)(ack_deadline - now);
            if (!WinUsb_SetPipePolicy(ctx->usb_handle, ARC_EP_EVT_IN,
                                       PIPE_TRANSFER_TIMEOUT,
                                       sizeof(per_ms), &per_ms))
                LERR(ctx, "arc_transmit: SetPipePolicy EP0x81 GLE=%lu\n",
                     GetLastError());

            memset(ev_buf, 0, sizeof(ev_buf));
            ev_xferred = 0;
            ev_ok = WinUsb_ReadPipe(ctx->usb_handle, ARC_EP_EVT_IN,
                                     ev_buf, sizeof(ev_buf), &ev_xferred, NULL);
            if (!ev_ok) {
                ev_err = GetLastError();
                if (ev_err == ERROR_SEM_TIMEOUT) {
                    LINFO(ctx, "arc_transmit: EP0x81 timeout -> ARC_NOT_ACKED\n");
                    r = ARC_NOT_ACKED; goto done;
                }
                if (is_gone_error(ev_err)) {
                    ctx->device_gone = true;
                    LERR(ctx, "arc_transmit: device gone on EP0x81 GLE=%lu\n", ev_err);
                    r = ARC_ERR_DEVICE_GONE; goto done;
                }
                LERR(ctx, "arc_transmit: ReadPipe EP0x81 GLE=%lu\n", ev_err);
                r = ARC_ERR_IO; goto done;
            }

            /* Skip non-0x20 packets (e.g. stale register responses). */
            if (ev_xferred < 6 || ev_buf[0] != ARC_OPCODE_EVENT) {
                LDBG(ctx, "arc_transmit: EP0x81 non-event skip "
                     "(opcode=0x%02X len=%lu)\n",
                     ev_xferred > 0 ? ev_buf[0] : 0xFFu, ev_xferred);
                continue;
            }

            {
                uint8_t b4      = ev_buf[4];
                ULONG   elapsed = (ULONG)(GetTickCount64() - ack_start);
                LDBG(ctx, "arc_transmit: 0x20 event b4=0x%02X (+%lu ms)\n",
                     b4, elapsed);

                /* bit0=0: saf RECON / other -- not our TX-complete, skip */
                if (!(b4 & 0x01)) {
                    LDBG(ctx, "arc_transmit: b4=0x%02X bit0=0 (RECON/other) skip\n", b4);
                    continue;
                }

                /* bit0=1: TX-complete event for our frame */
                if (b4 & 0x02) {
                    LDBG(ctx, "arc_transmit: b4=0x%02X TX-complete ACK -> ARC_OK\n", b4);
                    r = ARC_OK;
                } else {
                    LINFO(ctx, "arc_transmit: b4=0x%02X TX-complete no-ACK -> ARC_NOT_ACKED\n", b4);
                    r = ARC_NOT_ACKED;
                }
                goto done;
            }
        }
    }

done:
    LeaveCriticalSection(&ctx->lock);
    return r;
}

/* =======================================================================
 * arc_read_event
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

    if (timeout_ms > 0 &&
        !WinUsb_SetPipePolicy(ctx->usb_handle, ARC_EP_EVT_IN,
                               PIPE_TRANSFER_TIMEOUT, sizeof(to), &to))
        LERR(ctx, "arc_read_event: SetPipePolicy GLE=%lu\n", GetLastError());

    if (!WinUsb_ReadPipe(ctx->usb_handle, ARC_EP_EVT_IN,
                          (PUCHAR)buf, (ULONG)bufsize, &xferred, NULL)) {
        err = GetLastError();
        if (err == ERROR_SEM_TIMEOUT) { r = ARC_NO_PACKET; goto done; }
        if (is_gone_error(err)) {
            ctx->device_gone = true;
            LERR(ctx, "arc_read_event: device gone GLE=%lu\n", err);
            r = ARC_ERR_DEVICE_GONE; goto done;
        }
        LERR(ctx, "arc_read_event: ReadPipe EP0x81 GLE=%lu\n", err);
        r = ARC_ERR_IO; goto done;
    }
    *out_len = (int)xferred;
    r = (xferred > 0) ? ARC_OK : ARC_NO_PACKET;

done:
    LeaveCriticalSection(&ctx->lock);
    return r;
}

/* =======================================================================
 * arc_receive_locked  --  internal; caller must hold ctx->lock.
 * timeout_ms overrides the EP pipe timeout for this one call.
 * ===================================================================== */
static arc_result_t arc_receive_locked(arc_ctx_t *ctx,
                                        uint8_t *src, uint8_t *dst,
                                        uint8_t *data, int *len,
                                        ULONG timeout_ms)
{
    arc_result_t r;
    BYTE         poll[10];
    BYTE         buf[RX_BUF_SIZE];
    ULONG        xferred;
    DWORD        err;
    BOOL         ok;
    int          L;

    if (ctx->device_gone) return ARC_ERR_DEVICE_GONE;

    if (!WinUsb_SetPipePolicy(ctx->usb_handle, ARC_EP_TX_OUT,
                               PIPE_TRANSFER_TIMEOUT, sizeof(timeout_ms), &timeout_ms))
        LERR(ctx, "arc_receive: SetPipePolicy TX_OUT GLE=%lu\n", GetLastError());
    if (!WinUsb_SetPipePolicy(ctx->usb_handle, ARC_EP_RX_IN,
                               PIPE_TRANSFER_TIMEOUT, sizeof(timeout_ms), &timeout_ms))
        LERR(ctx, "arc_receive: SetPipePolicy RX_IN GLE=%lu\n", GetLastError());

    memset(poll, 0x00, sizeof(poll));
    xferred = 0;
    ok  = WinUsb_WritePipe(ctx->usb_handle, ARC_EP_TX_OUT,
                            poll, sizeof(poll), &xferred, NULL);
    err = GetLastError();
    if (!ok) {
        if (is_gone_error(err)) {
            ctx->device_gone = true;
            LERR(ctx, "arc_receive: device gone on WritePipe EP0x02 GLE=%lu\n", err);
            return ARC_ERR_DEVICE_GONE;
        }
        LDBG(ctx, "arc_receive: WritePipe EP0x02 GLE=%lu -> flush+NO_PACKET\n", err);
        pipe_flush(ctx, ARC_EP_TX_OUT);
        return ARC_NO_PACKET;
    }
    LDBG(ctx, "arc_receive: poll wrote %lu bytes to EP0x02\n", xferred);

    memset(buf, 0, sizeof(buf));
    xferred = 0;
    ok  = WinUsb_ReadPipe(ctx->usb_handle, ARC_EP_RX_IN,
                           buf, sizeof(buf), &xferred, NULL);
    err = GetLastError();
    if (!ok) {
        if (err == ERROR_SEM_TIMEOUT) {
            LDBG(ctx, "arc_receive: EP0x86 timeout (no packet) -> flush\n");
            pipe_flush(ctx, ARC_EP_RX_IN);
            return ARC_NO_PACKET;
        }
        if (is_gone_error(err)) {
            ctx->device_gone = true;
            LERR(ctx, "arc_receive: device gone on ReadPipe EP0x86 GLE=%lu\n", err);
            return ARC_ERR_DEVICE_GONE;
        }
        LERR(ctx, "arc_receive: ReadPipe EP0x86 GLE=%lu -> flush+IO\n", err);
        pipe_flush(ctx, ARC_EP_RX_IN);
        return ARC_ERR_IO;
    }
    LDBG(ctx, "arc_receive: read %lu bytes from EP0x86\n", xferred);

    if (xferred < 3 || (buf[0] == 0 && buf[1] == 0)) return ARC_NO_PACKET;

    *src = buf[0];
    *dst = buf[1];

    if (buf[2] != 0x00) {
        /* Short frame (UPDATE 3): [src][dst][256-L][data] */
        L = (256 - (int)buf[2]) & 0xFF;
        if (L == 0 || (int)xferred < L + 3) return ARC_NO_PACKET;
        *len = L;
        memcpy(data, buf + 3, (size_t)L);
        LDBG(ctx, "arc_receive: short frame src=0x%02X dst=0x%02X len=%d\n",
             *src, *dst, L);
    } else {
        /* Long frame (UPDATE 9): [src][dst][0x00][512-L][data] */
        if (xferred < 4) return ARC_NO_PACKET;
        L = 512 - (int)buf[3];
        if (L < 254 || L > 508 || (int)xferred < L + 4) {
            LDBG(ctx, "arc_receive: long frame invalid (L=%d xferred=%lu), dropping\n",
                 L, xferred);
            return ARC_NO_PACKET;
        }
        *len = L;
        memcpy(data, buf + 4, (size_t)L);
        LDBG(ctx, "arc_receive: long frame src=0x%02X dst=0x%02X len=%d nextbyte=0x%02X\n",
             *src, *dst, L, buf[3]);
    }

    r = ARC_OK;
    return r;
}

/* =======================================================================
 * arc_receive  --  public; acquires ctx->lock for the duration of the poll.
 * ===================================================================== */
arc_result_t arc_receive(arc_ctx_t *ctx, uint8_t *src, uint8_t *dst,
                         uint8_t *data, int *len)
{
    arc_result_t r;
    if (!ctx)                          return ARC_ERR_OPEN;
    if (!src || !dst || !data || !len) return ARC_ERR_PARAM;
    EnterCriticalSection(&ctx->lock);
    r = arc_receive_locked(ctx, src, dst, data, len, ARC_RECEIVE_TIMEOUT_MS);
    LeaveCriticalSection(&ctx->lock);
    return r;
}

/* =======================================================================
 * listen_worker  --  background thread body for arc_listen_start.
 *
 * Each iteration:
 *   1. Acquire ctx->lock, call arc_receive_locked (short timeout), release.
 *   2. On ARC_OK: enqueue under queue_lock; fire callback outside all locks.
 *   3. On ARC_NO_PACKET: brief Sleep before next poll.
 *   4. On error: longer back-off Sleep.
 * ===================================================================== */
static DWORD WINAPI listen_worker(LPVOID arg)
{
    arc_ctx_t   *ctx = (arc_ctx_t *)arg;

    while (!InterlockedCompareExchange(&ctx->listen_stop, 0, 0)) {
        arc_packet_t pkt;
        arc_result_t r;
        arc_recv_fn  fn;
        void        *fn_user;

        EnterCriticalSection(&ctx->lock);
        if (ctx->device_gone) {
            LeaveCriticalSection(&ctx->lock);
            Sleep(50);
            continue;
        }
        r = arc_receive_locked(ctx, &pkt.src, &pkt.dst, pkt.data, &pkt.len,
                               ARC_LISTEN_POLL_MS);
        LeaveCriticalSection(&ctx->lock);

        if (r == ARC_OK) {
            EnterCriticalSection(&ctx->queue_lock);
            if (ctx->queue.count < (int)ARC_QUEUE_CAP) {
                ctx->queue.pkts[ctx->queue.head] = pkt;
                ctx->queue.head = (ctx->queue.head + 1) % (int)ARC_QUEUE_CAP;
                ctx->queue.count++;
            } else {
                /* Drop oldest to make room for newest */
                ctx->queue.tail = (ctx->queue.tail + 1) % (int)ARC_QUEUE_CAP;
                ctx->queue.pkts[ctx->queue.head] = pkt;
                ctx->queue.head = (ctx->queue.head + 1) % (int)ARC_QUEUE_CAP;
                ctx->queue.dropped++;
            }
            fn      = ctx->recv_fn;
            fn_user = ctx->recv_user;
            LeaveCriticalSection(&ctx->queue_lock);

            if (fn) fn(&pkt, fn_user);  /* outside all locks */

        } else if (r == ARC_NO_PACKET) {
            Sleep(ARC_LISTEN_SLEEP_MS);
        } else {
            Sleep(50);  /* IO error or device gone — back off */
        }
    }
    return 0;
}

/* =======================================================================
 * arc_listen_start
 * ===================================================================== */
arc_result_t arc_listen_start(arc_ctx_t *ctx)
{
    arc_result_t r = ARC_OK;
    if (!ctx) return ARC_ERR_OPEN;
    EnterCriticalSection(&ctx->lock);
    if (ctx->listen_thread) { /* already running */ goto done; }
    InterlockedExchange(&ctx->listen_stop, 0);
    ctx->listen_thread = CreateThread(NULL, 0, listen_worker, ctx, 0, NULL);
    if (!ctx->listen_thread) {
        LERR(ctx, "arc_listen_start: CreateThread failed GLE=%lu\n", GetLastError());
        r = ARC_ERR_IO;
    } else {
        LINFO(ctx, "arc_listen_start: listener thread started\n");
    }
done:
    LeaveCriticalSection(&ctx->lock);
    return r;
}

/* =======================================================================
 * arc_listen_stop
 * ===================================================================== */
arc_result_t arc_listen_stop(arc_ctx_t *ctx)
{
    HANDLE th;
    if (!ctx) return ARC_ERR_OPEN;

    InterlockedExchange(&ctx->listen_stop, 1);

    EnterCriticalSection(&ctx->lock);
    th = ctx->listen_thread;
    if (th) ctx->listen_thread = NULL;
    LeaveCriticalSection(&ctx->lock);

    if (!th) return ARC_OK;  /* was not running */

    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    LINFO(ctx, "arc_listen_stop: listener thread joined\n");
    return ARC_OK;
}

/* =======================================================================
 * arc_poll_packet
 * ===================================================================== */
arc_result_t arc_poll_packet(arc_ctx_t *ctx, arc_packet_t *out)
{
    arc_result_t r;
    if (!ctx || !out) return ARC_ERR_PARAM;
    EnterCriticalSection(&ctx->queue_lock);
    if (ctx->queue.count == 0) {
        r = ARC_NO_PACKET;
    } else {
        *out = ctx->queue.pkts[ctx->queue.tail];
        ctx->queue.tail = (ctx->queue.tail + 1) % (int)ARC_QUEUE_CAP;
        ctx->queue.count--;
        r = ARC_OK;
    }
    LeaveCriticalSection(&ctx->queue_lock);
    return r;
}

/* =======================================================================
 * arc_pending_count
 * ===================================================================== */
int arc_pending_count(arc_ctx_t *ctx)
{
    int n;
    if (!ctx) return 0;
    EnterCriticalSection(&ctx->queue_lock);
    n = ctx->queue.count;
    LeaveCriticalSection(&ctx->queue_lock);
    return n;
}

/* =======================================================================
 * arc_set_recv_callback
 * ===================================================================== */
arc_result_t arc_set_recv_callback(arc_ctx_t *ctx, arc_recv_fn fn, void *user)
{
    if (!ctx) return ARC_ERR_OPEN;
    EnterCriticalSection(&ctx->queue_lock);
    ctx->recv_fn   = fn;
    ctx->recv_user = user;
    LeaveCriticalSection(&ctx->queue_lock);
    return ARC_OK;
}
