/*
 * test_arcnet.c  --  End-to-end test for the arcnet library.
 *
 * Test sequence:
 *   1. arc_open(NULL)        auto-find CC ARCNET device
 *   2. arc_init(...)         cmd04 + handshake + COM20020 config
 *   3. arc_register(read)    read COM20020 registers 0-7
 *   4. arc_transmit(...)     send test packet to TEST_DEST_NODE
 *   5. receive loop          listen for recv_timeout_sec seconds (skipped with --no-receive)
 */

#include "arcnet.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>

/*
 * resolve_device_path -- maps a WMI DeviceID to a WinUSB interface path.
 *
 * wmi_id example : "USB\VID_0D0B&PID_1002\5&1234ABCD&0&1"
 * WinUSB path    : "\\?\usb#vid_0d0b&pid_1002#5&1234abcd&0&1#{guid}"
 *
 * Matches the instance-ID segment (after the last '\') case-insensitively
 * against all known device paths; returns the first match. NULL = not found.
 */
static const char *resolve_device_path(const char *wmi_id)
{
    static char buf[256];
    char        paths[8][256];
    const char *p;
    const char *inst;
    size_t      il, pl, k;
    int         n, j;

    if (!wmi_id) return NULL;

    p    = strrchr(wmi_id, '\\');
    inst = p ? p + 1 : wmi_id;
    il   = strlen(inst);

    n = arc_list_devices(paths, 8);
    for (j = 0; j < n; j++) {
        pl = strlen(paths[j]);
        for (k = 0; k + il <= pl; k++) {
            if (_strnicmp(paths[j] + k, inst, il) == 0) {
                memcpy(buf, paths[j], pl + 1);
                return buf;
            }
        }
    }
    return NULL;
}

/* ---- Test parameters (adjust as needed) ---- */
#define TEST_NODE_ID         1
#define TEST_TIMEOUT         0x18
#define TEST_CLOCK_PRESCALER 0x00
#define TEST_RECV_BCAST      true
#define TEST_DEST_NODE       2
#define TEST_PAYLOAD         "HELLO-ARCNET"
#define RECEIVE_LOOP_SEC     30

int main(int argc, char *argv[])
{
    arc_ctx_t   *ctx = NULL;
    arc_result_t r;
    uint8_t      val;
    uint8_t      reg;
    uint8_t      src, dst;
    uint8_t      data[256];
    int          data_len;
    int          i;
    int          pkt_count = 0;
    int          hw_err    = 0;
    int          recv_timeout_sec = RECEIVE_LOOP_SEC;
    int          no_receive = 0;
    const char  *device_path_arg = NULL;   /* --device-path <WMI DeviceID> */
    const char  *dev_to_open;
    ULONGLONG    loop_start;

    /* Parse --recv-timeout N | --no-receive | --quick | --device-path <id> */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--recv-timeout") == 0 && i + 1 < argc)
            recv_timeout_sec = atoi(argv[++i]);
        else if (strcmp(argv[i], "--no-receive") == 0 || strcmp(argv[i], "--quick") == 0)
            no_receive = 1;
        else if (strcmp(argv[i], "--device-path") == 0 && i + 1 < argc)
            device_path_arg = argv[++i];
    }

    printf("==============================================\n");
    printf("  USB22-485 ARCNET Library Test\n");
    printf("  VID=0x%04X  PID=0x%04X\n", ARC_VID, ARC_PID);
    printf("==============================================\n\n");

    /* ------------------------------------------------------------------ */
    /* [1] Open device                                                     */
    /* ------------------------------------------------------------------ */
    printf("--- arc_open ---\n");
    dev_to_open = resolve_device_path(device_path_arg);
    if (device_path_arg && !dev_to_open) {
        printf("arc_open: device not found for '%s'\n\n", device_path_arg);
        return 1;
    }
    if (dev_to_open)
        printf("  path: %s\n", dev_to_open);

    ctx = arc_open(dev_to_open, /*verbose=*/true);
    printf("arc_open: %s\n\n", ctx ? "ARC_OK" : "ARC_ERR_OPEN");
    if (!ctx) return 1;

    /* ------------------------------------------------------------------ */
    /* [2] Initialize  (cmd04 -> handshake -> COM20020 config)            */
    /* ------------------------------------------------------------------ */
    printf("--- arc_init (nodeID=%d timeout=0x%02X prescaler=0x%02X bcast=%d) ---\n",
           TEST_NODE_ID, TEST_TIMEOUT, TEST_CLOCK_PRESCALER, (int)TEST_RECV_BCAST);
    r = arc_init(ctx, TEST_NODE_ID, TEST_TIMEOUT, TEST_CLOCK_PRESCALER, TEST_RECV_BCAST);
    printf("arc_init: %s\n\n", arc_result_str(r));
    if (r != ARC_OK) { arc_close(ctx); return 1; }

    /* ------------------------------------------------------------------ */
    /* [3] Read COM20020 registers 0-7                                    */
    /* ------------------------------------------------------------------ */
    printf("--- arc_register (read regs 0..7) ---\n");
    for (reg = 0; reg < 8; reg++) {
        val = 0;
        r = arc_register(ctx, false, reg, &val);
        if (r == ARC_OK)
            printf("  Reg[%u] = 0x%02X\n", reg, val);
        else
            printf("  Reg[%u] ERROR: %s\n", reg, arc_result_str(r));
    }
    printf("\n");

    /* ------------------------------------------------------------------ */
    /* [4] Transmit a test packet                                         */
    /* ------------------------------------------------------------------ */
    printf("--- arc_transmit -> node %u ---\n", TEST_DEST_NODE);
    r = arc_transmit(ctx, TEST_DEST_NODE,
                     (const uint8_t *)TEST_PAYLOAD,
                     (int)strlen(TEST_PAYLOAD), /*waitAck=*/true);
    printf("arc_transmit: %s\n\n", arc_result_str(r));

    /* ------------------------------------------------------------------ */
    /* [5] Receive loop — skipped when --no-receive / --quick is given   */
    /* ------------------------------------------------------------------ */
    if (!no_receive) {
        printf("--- Receive loop (%d s) ---\n", recv_timeout_sec);
        printf("Waiting for packets from other nodes...\n\n");

        loop_start = GetTickCount64();
        for (;;) {
            memset(data, 0, sizeof(data));
            data_len = 0;
            r = arc_receive(ctx, &src, &dst, data, &data_len);

            if (r == ARC_OK) {
                pkt_count++;
                printf("=== Packet #%d received ===\n", pkt_count);
                printf("  Source : %u (0x%02X)\n", src, src);
                printf("  Dest   : %u (0x%02X)\n", dst, dst);
                printf("  Length : %d bytes\n",     data_len);
                printf("  Hex    :");
                for (i = 0; i < data_len; i++) printf(" %02X", data[i]);
                printf("\n");
                printf("  ASCII  : \"");
                for (i = 0; i < data_len; i++)
                    putchar((data[i] >= 0x20 && data[i] < 0x7F) ? data[i] : '.');
                printf("\"\n\n");
                break;
            } else if (r == ARC_ERR_IO) {
                printf("arc_receive: hardware error, stopping.\n");
                hw_err = 1;
                goto done;
            }
            if ((GetTickCount64() - loop_start) >= (ULONGLONG)(recv_timeout_sec * 1000))
                break;
        }

        if (pkt_count == 0)
            printf("receive: paket alınmadı (süre doldu, %d sn).\n", recv_timeout_sec);
    }

done:
    printf("\n");
    arc_close(ctx);
    return hw_err ? 1 : 0;
}
