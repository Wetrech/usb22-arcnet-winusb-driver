/*
 * arcnet.c  --  USB22-485 WinUSB user-mode library implementation.
 *
 * Protocol reference: reference/usb22-protocol-notes.md (UPDATE 1-6)
 *
 * Startup sequence (mirrors original CC driver, UPDATE 5 + 6):
 *   arc_open()   ->  CreateFile + WinUsb_Initialize
 *   arc_init()   ->  cmd04 (session start)
 *              ->  handshake (data-channel probe, mandatory before init)
 *              ->  COM20020 config command (device responds after ~2.5 s)
 *
 * Module state is a single static instance; only one device can be open.
 * All output is gated by arc_set_verbose(); callers use result codes.
 */

#include "arcnet.h"
#include <windows.h>
#include <setupapi.h>
#include <winusb.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

/* Internal buffer size for EP 0x86 (MaxPacketSize=512, per winusb_probe). */
#define RX_BUF_SIZE     512u

/* -----------------------------------------------------------------------
 * Module-level state
 * --------------------------------------------------------------------- */
typedef struct {
    HANDLE                  dev_handle;
    WINUSB_INTERFACE_HANDLE usb_handle;
} ARC_DEVICE;

static ARC_DEVICE g_dev;               /* zero-initialised by C runtime   */
static bool       g_verbose = false;
static bool       g_is_open = false;

/* -----------------------------------------------------------------------
 * Logging helpers -- all output is gated by g_verbose.
 * VLOG     : informational, to stdout
 * VLOG_ERR : error detail, to stderr
 * --------------------------------------------------------------------- */
#define VLOG(fmt, ...) \
    do { if (g_verbose) printf("[arcnet] " fmt, ##__VA_ARGS__); } while (0)

#define VLOG_ERR(fmt, ...) \
    do { if (g_verbose) fprintf(stderr, "[arcnet] " fmt, ##__VA_ARGS__); } while (0)

/* Project-specific DeviceInterfaceGUID — must match driver/usb22_winusb.inf [Dev_AddReg].
 * {E6B4B5C0-F74E-4A1D-9B8F-2C3D4E5F6A7B} */
static const GUID GUID_USB_DEVICE = {
    0xE6B4B5C0u, 0xF74Eu, 0x4A1Du,
    { 0x9Bu, 0x8Fu, 0x2Cu, 0x3Du, 0x4Eu, 0x5Fu, 0x6Au, 0x7Bu }
};

/* =======================================================================
 * arc_set_verbose
 * ===================================================================== */
void arc_set_verbose(bool enable)
{
    g_verbose = enable;
}

/* =======================================================================
 * arc_result_str
 * ===================================================================== */
const char *arc_result_str(arc_result_t r)
{
    switch (r) {
    case ARC_OK:          return "ARC_OK";
    case ARC_NO_PACKET:   return "ARC_NO_PACKET";
    case ARC_ERR_OPEN:    return "ARC_ERR_OPEN";
    case ARC_ERR_IO:      return "ARC_ERR_IO";
    case ARC_ERR_TIMEOUT: return "ARC_ERR_TIMEOUT";
    case ARC_ERR_PARAM:   return "ARC_ERR_PARAM";
    case ARC_ERR_ECHO:    return "ARC_ERR_ECHO";
    default:              return "(unknown)";
    }
}

/* =======================================================================
 * enum_device_paths  --  internal
 *   Fills paths[][] with WinUSB device interface paths matching vid/pid.
 *   Returns count found (up to maxDevices).
 * ===================================================================== */
static int enum_device_paths(unsigned int vid, unsigned int pid,
                              char paths[][256], int maxDevices)
{
    HDEVINFO                           devs;
    SP_DEVICE_INTERFACE_DATA           iface_data;
    PSP_DEVICE_INTERFACE_DETAIL_DATA_A detail;
    DWORD                              idx, needed;
    char                               token[32];   /* "VID_0D0B&PID_1002" */
    char                               upper[512];
    char                              *p;
    int                                count = 0;

    snprintf(token, sizeof(token), "VID_%04X&PID_%04X", vid, pid);

    devs = SetupDiGetClassDevsA(&GUID_USB_DEVICE, NULL, NULL,
                                 DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE) {
        VLOG_ERR("SetupDiGetClassDevs failed: %lu\n", GetLastError());
        return 0;
    }

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

        /* Case-insensitive match; Windows paths use uppercase VID/PID */
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
arc_result_t arc_open(const char *devicePath)
{
    char        auto_path[256];
    const char *path;
    ULONG       timeout_ms = ARC_READ_TIMEOUT_MS;

    if (g_is_open) {
        VLOG_ERR("arc_open: device already open; call arc_close() first\n");
        return ARC_ERR_OPEN;
    }

    if (devicePath) {
        path = devicePath;
    } else {
        /* Auto-find the first CC ARCNET device */
        char found[1][256];
        if (enum_device_paths(ARC_VID, ARC_PID, found, 1) == 0) {
            VLOG_ERR("arc_open: device not found (VID=%04X PID=%04X). "
                     "Is WinUSB bound via Zadig?\n", ARC_VID, ARC_PID);
            return ARC_ERR_OPEN;
        }
        strncpy(auto_path, found[0], sizeof(auto_path) - 1);
        auto_path[sizeof(auto_path) - 1] = '\0';
        path = auto_path;
    }

    VLOG("arc_open: %s\n", path);

    g_dev.dev_handle = CreateFileA(path,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);

    if (g_dev.dev_handle == INVALID_HANDLE_VALUE) {
        VLOG_ERR("arc_open: CreateFile failed: %lu\n", GetLastError());
        return ARC_ERR_OPEN;
    }

    if (!WinUsb_Initialize(g_dev.dev_handle, &g_dev.usb_handle)) {
        VLOG_ERR("arc_open: WinUsb_Initialize failed: %lu\n", GetLastError());
        CloseHandle(g_dev.dev_handle);
        g_dev.dev_handle = NULL;
        return ARC_ERR_OPEN;
    }

    /* Per-read timeout on the command response endpoint */
    if (!WinUsb_SetPipePolicy(g_dev.usb_handle, ARC_EP_EVT_IN,
                               PIPE_TRANSFER_TIMEOUT,
                               sizeof(timeout_ms), &timeout_ms))
        VLOG_ERR("arc_open: SetPipePolicy EP0x81 warning: %lu\n", GetLastError());

    g_is_open = true;
    VLOG("arc_open: OK\n");
    return ARC_OK;
}

/* =======================================================================
 * arc_close
 * ===================================================================== */
void arc_close(void)
{
    if (g_dev.usb_handle)   { WinUsb_Free(g_dev.usb_handle); g_dev.usb_handle = NULL; }
    if (g_dev.dev_handle)   { CloseHandle(g_dev.dev_handle);  g_dev.dev_handle = NULL; }
    g_is_open = false;
    VLOG("arc_close: done\n");
}

/* =======================================================================
 * write_cmd  --  internal: send bytes to EP_CMD_OUT (0x01)
 * ===================================================================== */
static arc_result_t write_cmd(const BYTE *cmd, ULONG len)
{
    ULONG xferred = 0;
    if (!WinUsb_WritePipe(g_dev.usb_handle, ARC_EP_CMD_OUT,
                           (PUCHAR)cmd, len, &xferred, NULL)) {
        VLOG_ERR("write_cmd: WritePipe EP0x01 failed: %lu\n", GetLastError());
        return ARC_ERR_IO;
    }
    if (xferred != len) {
        VLOG_ERR("write_cmd: short write %lu/%lu\n", xferred, len);
        return ARC_ERR_IO;
    }
    return ARC_OK;
}

/* =======================================================================
 * read_response  --  internal
 *   Reads from EP_EVT_IN (0x81) until a packet with byte[0]==expected arrives,
 *   or the time budget expires.
 *
 *   0x20 (EVENT) and other unexpected opcodes are silently skipped.
 *   Per-read timeout = ARC_READ_TIMEOUT_MS (1 s); the budget allows multiple
 *   attempts -- necessary for init which takes ~2.5 s (UPDATE 6).
 *
 *   Returns ARC_OK on success (*out_len = byte count received).
 *   Returns ARC_ERR_TIMEOUT if budget expires without the expected opcode.
 *   Returns ARC_ERR_IO on USB failure.
 * ===================================================================== */
static arc_result_t read_response(BYTE expected_opcode,
                                   BYTE *buf, ULONG buf_size,
                                   DWORD budget_ms, ULONG *out_len)
{
    ULONGLONG start = GetTickCount64();
    ULONG     xferred;
    DWORD     err;

    while ((GetTickCount64() - start) < (ULONGLONG)budget_ms) {
        xferred = 0;
        if (!WinUsb_ReadPipe(g_dev.usb_handle, ARC_EP_EVT_IN,
                              buf, buf_size, &xferred, NULL)) {
            err = GetLastError();
            if (err == ERROR_SEM_TIMEOUT)
                /* Per-read timeout expired; budget may still have room.
                 * Happens multiple times during init's ~2.5 s device wait. */
                continue;
            VLOG_ERR("read_response: ReadPipe EP0x81 failed: %lu\n", err);
            return ARC_ERR_IO;
        }

        if (xferred == 0)
            continue;   /* ZLP: device not ready yet, budget still running */

        if (buf[0] == expected_opcode) {
            *out_len = xferred;
            return ARC_OK;
        }

        /* Skip async events and stale responses with the wrong opcode */
        if (buf[0] == ARC_OPCODE_EVENT)
            VLOG("read_response: skipping 0x20 event at %.1f s\n",
                 (double)(GetTickCount64() - start) / 1000.0);
        else
            VLOG("read_response: skipping unexpected opcode 0x%02X at %.1f s\n",
                 buf[0], (double)(GetTickCount64() - start) / 1000.0);
    }

    VLOG_ERR("read_response: budget %lu ms exhausted, opcode 0x%02X not received\n",
             budget_ms, expected_opcode);
    return ARC_ERR_TIMEOUT;
}

/* =======================================================================
 * cmd04_internal  --  session-start command (UPDATE 4)
 *   OUT EP0x01 : 04 00          (2 bytes)
 *   IN  EP0x81 : 04 00 00 00   (4 bytes)
 * ===================================================================== */
static arc_result_t cmd04_internal(void)
{
    BYTE         cmd[2] = { ARC_OPCODE_CMD04, 0x00 };
    BYTE         resp[ARC_EP_EVT_MAXPACKET];
    ULONG        xferred;
    arc_result_t r;
    ULONG        i;

    VLOG("cmd04: sending 04 00\n");

    r = write_cmd(cmd, sizeof(cmd));
    if (r != ARC_OK) return r;

    memset(resp, 0, sizeof(resp));
    r = read_response(ARC_OPCODE_CMD04, resp, sizeof(resp),
                      ARC_BUDGET_SHORT_MS, &xferred);
    if (r != ARC_OK) return r;

    if (g_verbose) {
        printf("[arcnet] cmd04: response (%lu bytes):", xferred);
        for (i = 0; i < xferred; i++) printf(" %02X", resp[i]);
        printf("\n");
    }

    /* Expected: 04 00 00 00 -- warn on mismatch but continue */
    if (xferred < 4 || resp[1] || resp[2] || resp[3])
        VLOG("cmd04: response differs from 04 00 00 00 (continuing)\n");

    return ARC_OK;
}

/* =======================================================================
 * handshake_internal  --  data-channel probe before init (UPDATE 5)
 *   OUT EP0x02 : 10 zero bytes
 *   IN  EP0x86 : any response, or timeout on empty channel (both OK)
 *
 *   Without this step the init command receives no response.
 * ===================================================================== */
static arc_result_t handshake_internal(void)
{
    BYTE  out_buf[10];
    BYTE  in_buf[ARC_EP_EVT_MAXPACKET];
    ULONG xferred;
    ULONG hs_timeout = 500u;    /* short probe timeout for empty channel */
    DWORD err;
    ULONG i;

    /* Use a short timeout on the receive data endpoint for this probe only */
    if (!WinUsb_SetPipePolicy(g_dev.usb_handle, ARC_EP_RX_IN,
                               PIPE_TRANSFER_TIMEOUT,
                               sizeof(hs_timeout), &hs_timeout))
        VLOG_ERR("handshake: SetPipePolicy EP0x86 warning: %lu\n", GetLastError());

    memset(out_buf, 0x00, sizeof(out_buf));
    xferred = 0;
    if (!WinUsb_WritePipe(g_dev.usb_handle, ARC_EP_TX_OUT,
                           out_buf, sizeof(out_buf), &xferred, NULL)) {
        VLOG_ERR("handshake: WritePipe EP0x02 failed: %lu\n", GetLastError());
        return ARC_ERR_IO;
    }
    VLOG("handshake: sent %lu zero bytes to EP0x02\n", xferred);

    memset(in_buf, 0, sizeof(in_buf));
    xferred = 0;
    if (!WinUsb_ReadPipe(g_dev.usb_handle, ARC_EP_RX_IN,
                          in_buf, sizeof(in_buf), &xferred, NULL)) {
        err = GetLastError();
        if (err == ERROR_SEM_TIMEOUT) {
            /* Empty channel timeout is normal and expected */
            VLOG("handshake: EP0x86 timeout (empty channel, OK)\n");
            return ARC_OK;
        }
        VLOG_ERR("handshake: ReadPipe EP0x86 failed: %lu\n", err);
        return ARC_ERR_IO;
    }

    if (g_verbose) {
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
arc_result_t arc_init(uint8_t nodeID, uint8_t timeout,
                      uint8_t clockPrescaler, bool recvBroadcasts)
{
    BYTE         cmd[12];
    BYTE         resp[ARC_EP_EVT_MAXPACKET];
    ULONG        xferred;
    ULONG        rx_timeout = ARC_RECEIVE_TIMEOUT_MS;
    arc_result_t r;
    ULONG        i;

    if (!g_is_open) return ARC_ERR_OPEN;

    /* Step 1: session-start */
    r = cmd04_internal();
    if (r != ARC_OK) return r;

    /* Step 2: data-channel handshake (mandatory -- see UPDATE 5) */
    r = handshake_internal();
    if (r != ARC_OK) return r;

    /* Step 3: COM20020 configuration command (12 bytes, hand-packed).
     *
     * Byte layout (matches protdef.h COM20020_CONFIG, no padding):
     *   [0]  opcode = 0x00
     *   [1]  0x00
     *   [2]  BaseIOAddress lo  = 0x00
     *   [3]  BaseIOAddress hi  = 0x00
     *   [4]  InterruptLevel    = 0x00
     *   [5]  Timeout
     *   [6]  NodeID
     *   [7]  128NAKs           = 1
     *   [8]  ReceiveAll        = 0
     *   [9]  ClockPrescaler
     *  [10]  SlowArbitration   = 1 when prescaler > 5
     *  [11]  ReceiveBroadcasts
     */
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

    if (g_verbose) {
        printf("[arcnet] arc_init: command (%lu bytes):", (ULONG)sizeof(cmd));
        for (i = 0; i < sizeof(cmd); i++) printf(" %02X", cmd[i]);
        printf("\n");
    }

    r = write_cmd(cmd, sizeof(cmd));
    if (r != ARC_OK) return r;

    memset(resp, 0, sizeof(resp));
    /* Device configures COM20020 and waits for ARCNET token -- takes ~2.5 s */
    r = read_response(ARC_OPCODE_INIT, resp, sizeof(resp),
                      ARC_BUDGET_INIT_MS, &xferred);
    if (r != ARC_OK) return r;

    if (g_verbose) {
        printf("[arcnet] arc_init: response (%lu bytes):", xferred);
        for (i = 0; i < xferred; i++) printf(" %02X", resp[i]);
        printf("\n");
        /* resp[4] = status byte; 0x22 observed -- any value treated as OK */
        if (xferred >= 6)
            printf("[arcnet] arc_init: status byte = 0x%02X%s\n",
                   resp[4], resp[4] == 0x00 ? " (zero)" : " (non-zero, OK)");
    }

    /* Switch EP 0x86 to the normal per-poll timeout for arc_receive() */
    if (!WinUsb_SetPipePolicy(g_dev.usb_handle, ARC_EP_RX_IN,
                               PIPE_TRANSFER_TIMEOUT,
                               sizeof(rx_timeout), &rx_timeout))
        VLOG_ERR("arc_init: SetPipePolicy EP0x86 warning: %lu\n", GetLastError());

    VLOG("arc_init: OK (nodeID=0x%02X)\n", nodeID);
    return ARC_OK;
}

/* =======================================================================
 * arc_register
 *   OUT EP0x01 : 01 00 [bWrite] [reg] [val_or_00]   (5 bytes)
 *   IN  EP0x81 : 01 00 00 00 [bWrite] [reg] [val]   (7 bytes)
 * ===================================================================== */
arc_result_t arc_register(bool bWrite, uint8_t reg, uint8_t *value)
{
    BYTE         cmd[5];
    BYTE         resp[ARC_EP_EVT_MAXPACKET];
    ULONG        xferred;
    arc_result_t r;

    if (!g_is_open)  return ARC_ERR_OPEN;
    if (!value)      return ARC_ERR_PARAM;

    cmd[0] = ARC_OPCODE_REGISTER;
    cmd[1] = 0x00;
    cmd[2] = bWrite ? 0x01u : 0x00u;
    cmd[3] = reg;
    cmd[4] = bWrite ? *value : 0x00u;

    r = write_cmd(cmd, sizeof(cmd));
    if (r != ARC_OK) return r;

    memset(resp, 0, sizeof(resp));
    r = read_response(ARC_OPCODE_REGISTER, resp, sizeof(resp),
                      ARC_BUDGET_SHORT_MS, &xferred);
    if (r != ARC_OK) return r;

    if (xferred < 7) {
        VLOG_ERR("arc_register: response too short: %lu bytes\n", xferred);
        return ARC_ERR_IO;
    }

    /* resp[4]=bWrite echo, resp[5]=reg echo */
    if (resp[4] != cmd[2] || resp[5] != reg) {
        VLOG_ERR("arc_register: echo mismatch "
                 "(bWrite_echo=0x%02X reg_echo=0x%02X, sent 0x%02X 0x%02X)\n",
                 resp[4], resp[5], cmd[2], reg);
        return ARC_ERR_ECHO;
    }

    if (!bWrite)
        *value = resp[6];   /* byte[6] = register value */

    return ARC_OK;
}

/* =======================================================================
 * arc_transmit
 *   Wire format (UPDATE 2 + 3):
 *     byte[0] = destNode
 *     byte[1] = (256 - len) & 0xFF   (ARCNET count byte)
 *     byte[2..] = payload
 * ===================================================================== */
arc_result_t arc_transmit(uint8_t destNode, const uint8_t *data, int len)
{
    BYTE  buf[254];   /* 2 header + up to 252 data bytes */
    ULONG xferred;
    int   i;

    if (!g_is_open)                  return ARC_ERR_OPEN;
    if (!data || len < 1 || len > 252) {
        VLOG_ERR("arc_transmit: invalid argument (data=%p len=%d)\n",
                 (void *)data, len);
        return ARC_ERR_PARAM;
    }

    buf[0] = destNode;
    buf[1] = (BYTE)((256 - len) & 0xFF);
    memcpy(buf + 2, data, (size_t)len);

    if (g_verbose) {
        printf("[arcnet] arc_transmit: dest=0x%02X len=%d payload:", destNode, len);
        for (i = 0; i < len; i++) printf(" %02X", data[i]);
        printf("\n");
    }

    xferred = 0;
    if (!WinUsb_WritePipe(g_dev.usb_handle, ARC_EP_TX_OUT,
                           buf, (ULONG)(len + 2), &xferred, NULL)) {
        VLOG_ERR("arc_transmit: WritePipe EP0x02 failed: %lu\n", GetLastError());
        return ARC_ERR_IO;
    }
    if (xferred != (ULONG)(len + 2)) {
        VLOG_ERR("arc_transmit: short write %lu/%d\n", xferred, len + 2);
        return ARC_ERR_IO;
    }

    VLOG("arc_transmit: OK (%lu bytes to EP0x02)\n", xferred);
    return ARC_OK;
}

/* =======================================================================
 * arc_receive
 *   Protocol (UPDATE 3 + UPDATE 5):
 *     1. Poll: write 10 zero bytes to EP_TX_OUT (0x02)  ["any packets for me?"]
 *     2. Read: EP_RX_IN (0x86), timeout = ARC_RECEIVE_TIMEOUT_MS
 *     3. Parse: byte[0]=src, byte[1]=dst, byte[2]=count, byte[3..]=data
 *        payload length L = (256 - count) & 0xFF
 *   Empty response: xferred<3 OR src==0 && dst==0 -> ARC_NO_PACKET
 * ===================================================================== */
arc_result_t arc_receive(uint8_t *src, uint8_t *dst, uint8_t *data, int *len)
{
    BYTE  poll[10];
    BYTE  buf[RX_BUF_SIZE];
    ULONG xferred;
    DWORD err;
    int   L;

    if (!g_is_open)                      return ARC_ERR_OPEN;
    if (!src || !dst || !data || !len)   return ARC_ERR_PARAM;

    /* 1. Poll */
    memset(poll, 0x00, sizeof(poll));
    xferred = 0;
    if (!WinUsb_WritePipe(g_dev.usb_handle, ARC_EP_TX_OUT,
                           poll, sizeof(poll), &xferred, NULL)) {
        VLOG_ERR("arc_receive: poll WritePipe EP0x02 failed: %lu\n", GetLastError());
        return ARC_ERR_IO;
    }

    /* 2. Read */
    memset(buf, 0, sizeof(buf));
    xferred = 0;
    if (!WinUsb_ReadPipe(g_dev.usb_handle, ARC_EP_RX_IN,
                          buf, sizeof(buf), &xferred, NULL)) {
        err = GetLastError();
        if (err == ERROR_SEM_TIMEOUT)
            return ARC_NO_PACKET;   /* normal empty-channel result */
        VLOG_ERR("arc_receive: ReadPipe EP0x86 failed: %lu\n", err);
        return ARC_ERR_IO;
    }

    /* 3. Empty response check */
    if (xferred < 3 || (buf[0] == 0 && buf[1] == 0))
        return ARC_NO_PACKET;

    /* 4. Parse */
    *src = buf[0];
    *dst = buf[1];
    L    = (256 - (int)buf[2]) & 0xFF;

    if (L == 0 || (int)xferred < L + 3)
        return ARC_NO_PACKET;

    *len = L;
    memcpy(data, buf + 3, (size_t)L);
    return ARC_OK;
}
