/*
 * test_arcnet.c  --  End-to-end test for the arcnet library.
 *
 * Test sequence:
 *   1. arc_open(NULL)        auto-find CC ARCNET device
 *   2. arc_init(...)         cmd04 + handshake + COM20020 config
 *   3. arc_register(read)    read COM20020 registers 0-7
 *   4. arc_transmit(...)     send test packet to TEST_DEST_NODE
 *   5. receive loop          listen for RECEIVE_LOOP_SEC seconds, print packets
 *
 * To skip transmit: comment out section [4] below.
 * Expected behaviour is identical to the v0.1.0 arcnet_io.exe binary.
 */

#include "arcnet.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>    /* GetTickCount64 */

/* ---- Test parameters (adjust as needed) ---- */
#define TEST_NODE_ID         1
#define TEST_TIMEOUT         0x18
#define TEST_CLOCK_PRESCALER 0x00
#define TEST_RECV_BCAST      true
#define TEST_DEST_NODE       2
#define TEST_PAYLOAD         "HELLO-ARCNET"
#define RECEIVE_LOOP_SEC     30

int main(void)
{
    arc_result_t r;
    uint8_t      val;
    uint8_t      reg;
    uint8_t      src, dst;
    uint8_t      data[256];
    int          data_len;
    int          i;
    int          pkt_count = 0;
    ULONGLONG    loop_start;

    printf("==============================================\n");
    printf("  USB22-485 ARCNET Library Test\n");
    printf("  VID=0x%04X  PID=0x%04X\n", ARC_VID, ARC_PID);
    printf("==============================================\n\n");

    /* Enable verbose output so all protocol details are visible */
    arc_set_verbose(true);

    /* ------------------------------------------------------------------ */
    /* [1] Open device (auto-select first CC ARCNET device)               */
    /* ------------------------------------------------------------------ */
    printf("--- arc_open ---\n");
    r = arc_open(NULL);
    printf("arc_open: %s\n\n", arc_result_str(r));
    if (r != ARC_OK) return 1;

    /* ------------------------------------------------------------------ */
    /* [2] Initialize  (cmd04 -> handshake -> COM20020 config)            */
    /* ------------------------------------------------------------------ */
    printf("--- arc_init (nodeID=%d timeout=0x%02X prescaler=0x%02X bcast=%d) ---\n",
           TEST_NODE_ID, TEST_TIMEOUT, TEST_CLOCK_PRESCALER, (int)TEST_RECV_BCAST);
    r = arc_init(TEST_NODE_ID, TEST_TIMEOUT, TEST_CLOCK_PRESCALER, TEST_RECV_BCAST);
    printf("arc_init: %s\n\n", arc_result_str(r));
    if (r != ARC_OK) { arc_close(); return 1; }

    /* ------------------------------------------------------------------ */
    /* [3] Read COM20020 registers 0-7                                    */
    /* ------------------------------------------------------------------ */
    printf("--- arc_register (read regs 0..7) ---\n");
    for (reg = 0; reg < 8; reg++) {
        val = 0;
        r = arc_register(false, reg, &val);
        if (r == ARC_OK)
            printf("  Reg[%u] = 0x%02X\n", reg, val);
        else
            printf("  Reg[%u] ERROR: %s\n", reg, arc_result_str(r));
    }
    printf("\n");

    /* ------------------------------------------------------------------ */
    /* [4] Transmit a test packet                                         */
    /*     Comment this block out to run as a receive-only listener.      */
    /* ------------------------------------------------------------------ */
    printf("--- arc_transmit -> node %u ---\n", TEST_DEST_NODE);
    r = arc_transmit(TEST_DEST_NODE,
                     (const uint8_t *)TEST_PAYLOAD,
                     (int)strlen(TEST_PAYLOAD));
    printf("arc_transmit: %s\n\n", arc_result_str(r));
    /* Transmit failures are not fatal; continue to receive loop */

    /* ------------------------------------------------------------------ */
    /* [5] Receive loop -- listen for RECEIVE_LOOP_SEC seconds            */
    /* ------------------------------------------------------------------ */
    printf("--- Receive loop (%d s) ---\n", RECEIVE_LOOP_SEC);
    printf("Waiting for packets from other nodes... (Ctrl+C to abort)\n\n");

    loop_start = GetTickCount64();
    while ((GetTickCount64() - loop_start) < (ULONGLONG)(RECEIVE_LOOP_SEC * 1000)) {

        memset(data, 0, sizeof(data));
        data_len = 0;
        r = arc_receive(&src, &dst, data, &data_len);

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

            break;  /* stop after first packet */

        } else if (r == ARC_ERR_IO) {
            printf("arc_receive: hardware error, stopping.\n");
            goto done;
        }
        /* ARC_NO_PACKET: channel empty, continue silently */
    }

    if (pkt_count == 0)
        printf("Timeout: no packet received in %d seconds.\n", RECEIVE_LOOP_SEC);

done:
    printf("\n");
    arc_close();
    return (pkt_count > 0) ? 0 : 1;
}
