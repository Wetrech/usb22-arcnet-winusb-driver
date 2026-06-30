/*
 * test_ack.c  --  ARCNET hardware ACK detection diagnostic.
 *
 * Goal: find which bit (register or EP0x81 event) changes between
 *       "receiver alive" and "receiver dead" after a transmit.
 *
 * Usage:
 *   test_ack.exe --receiver-alive 1     (node 2 open + init'd -- ACK expected)
 *   test_ack.exe --receiver-alive 0     (node 2 absent      -- no ACK expected)
 *
 * Sequence:
 *   [1] arc_list_devices
 *   [2] Open node 1 (always) + optionally node 2
 *   [3] arc_init node 1      + optionally node 2
 *   [4] arc_transmit node1 -> node2  "ACKTEST1"
 *   [5] Read COM20022 registers 0-7 on node 1 (printed with bit masks)
 *   [6] Drain EP0x81 on node 1 for EVENT_DRAIN_MS ms (all packets, hex + decode)
 *   [7] Summary
 */

#include "arcnet.h"
#include "test_common.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <windows.h>

#define MAX_DEVS        8
#define EVENT_DRAIN_MS  500
#define EVENT_TO_MS     50    /* per-read timeout inside the drain loop */

/* COM20022 Status Register 0 bit names (UPDATE 8: bit 1 = TMA confirmed) */
static const char *reg0_bits[8] = {
    "b0:TA",    /* Transmitter Available (TX done)          */
    "b1:TMA",   /* Transmit Message Acknowledged  <-- ACK! */
    "b2:?",
    "b3:?",
    "b4:?",
    "b5:?",
    "b6:RI",    /* Receive IRQ (data ready)                 */
    "b7:RST"    /* Reset flag                               */
};

/* -----------------------------------------------------------------------
 * print_reg  --  print a register value in hex, decimal, and binary
 * --------------------------------------------------------------------- */
static void print_reg(uint8_t reg_idx, uint8_t val, const char *bit_names[8])
{
    int b;
    printf("  Reg[%u] = 0x%02X  (%3u)  ", reg_idx, val, val);
    for (b = 7; b >= 0; b--)
        putchar((val >> b) & 1 ? '1' : '0');
    printf("b");
    if (bit_names && reg_idx == 0) {
        printf("  [");
        for (b = 7; b >= 0; b--) {
            if ((val >> b) & 1)
                printf(" %s", bit_names[b]);
        }
        printf(" ]");
    }
    printf("\n");
}

/* -----------------------------------------------------------------------
 * drain_events  --  read all EP0x81 packets for budget_ms
 * --------------------------------------------------------------------- */
static void drain_events(arc_ctx_t *ctx, int budget_ms)
{
    uint8_t   buf[64];
    int       out_len;
    int       count     = 0;
    ULONGLONG start     = GetTickCount64();
    ULONGLONG elapsed;
    arc_result_t r;
    int       i;

    printf("\n[EP0x81 DRAIN: %d ms budget, %d ms per-read timeout]\n",
           budget_ms, EVENT_TO_MS);

    while (1) {
        elapsed = GetTickCount64() - start;
        if ((int)elapsed >= budget_ms) break;

        memset(buf, 0, sizeof(buf));
        out_len = 0;
        r = arc_read_event(ctx, buf, sizeof(buf), EVENT_TO_MS, &out_len);

        if (r == ARC_OK && out_len > 0) {
            count++;
            printf("  [+%.0f ms] pkt#%d  opcode=0x%02X  len=%d  hex:",
                   (double)elapsed, count, buf[0], out_len);
            for (i = 0; i < out_len; i++) printf(" %02X", buf[i]);
            printf("\n");

            /* Decode known opcodes */
            if (buf[0] == 0x00 && out_len >= 6) {
                printf("           --> INIT response  status=0x%02X nodeID=0x%02X\n",
                       buf[4], buf[6 < out_len ? 6 : 5]);
            } else if (buf[0] == 0x01 && out_len >= 7) {
                printf("           --> REGISTER response  reg=0x%02X val=0x%02X\n",
                       buf[5], buf[6]);
            } else if (buf[0] == 0x20) {
                printf("           --> EVENT/RECON/STATUS  payload:");
                for (i = 1; i < out_len; i++) printf(" %02X", buf[i]);
                printf("\n");
            } else {
                printf("           --> (unknown opcode)\n");
            }
        } else if (r == ARC_ERR_IO) {
            printf("  [+%.0f ms] EP0x81 read error\n", (double)elapsed);
            break;
        }
        /* ARC_NO_PACKET: timeout, loop again */
    }

    if (count == 0)
        printf("  (no packets received in %d ms)\n", budget_ms);
    printf("[END DRAIN: %d packet(s) in %.0f ms]\n\n",
           count, (double)(GetTickCount64() - start));
}

/* -----------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    char         paths[MAX_DEVS][256];
    int          n, i;
    arc_ctx_t   *ctx1      = NULL;
    arc_ctx_t   *ctx2      = NULL;
    arc_result_t r;
    arc_result_t tx_result = ARC_ERR_IO;  /* arc_transmit return, preserved for summary */
    int          receiver_alive = -1;
    uint8_t      val;
    ULONGLONG    t0, t1;

    /* Parse --receiver-alive 1|0 */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--receiver-alive") == 0 && i + 1 < argc)
            receiver_alive = atoi(argv[++i]);
    }

    if (receiver_alive < 0) {
        fprintf(stderr,
                "Usage: test_ack.exe --receiver-alive 1|0\n"
                "  1 = node 2 open+init (receiver ALIVE, ACK expected)\n"
                "  0 = node 2 absent    (receiver DEAD,  no ACK)\n");
        return 1;
    }

    printf("=====================================================\n");
    printf("  ARCNET ACK Detection Diagnostic\n");
    printf("  Scenario: receiver %s\n",
           receiver_alive ? "ALIVE (node2 open+init)" : "DEAD (node2 absent)");
    printf("=====================================================\n\n");

    /* ------------------------------------------------------------------ */
    /* [1] Enumerate                                                        */
    /* ------------------------------------------------------------------ */
    n = arc_list_devices(paths, MAX_DEVS);
    printf("[1] arc_list_devices: %d device(s)\n", n);
    for (i = 0; i < n; i++)
        printf("    [%d] %s\n", i, paths[i]);
    printf("\n");

    if (n < 1) {
        fprintf(stderr, "    --> No devices found. Exiting.\n");
        return 1;
    }
    if (receiver_alive && n < 2) {
        fprintf(stderr,
                "    --> --receiver-alive 1 requires 2 devices; only %d found.\n", n);
        return 1;
    }

    /* ------------------------------------------------------------------ */
    /* [2] Open                                                            */
    /* ------------------------------------------------------------------ */
    printf("[2] Opening node 1 ...\n");
    ctx1 = arc_open(paths[0], /*verbose=*/true);
    printf("    arc_open node1: %s\n\n", ctx1 ? "OK" : "FAILED");
    if (!ctx1) return 1;

    if (receiver_alive) {
        printf("[2] Opening node 2 ...\n");
        ctx2 = arc_open(paths[1], /*verbose=*/true);
        printf("    arc_open node2: %s\n\n", ctx2 ? "OK" : "FAILED");
        if (!ctx2) { arc_close(ctx1); return 1; }
    } else {
        printf("[2] node 2 intentionally NOT opened (receiver DEAD scenario).\n\n");
    }

    /* ------------------------------------------------------------------ */
    /* [3] Init                                                            */
    /* ------------------------------------------------------------------ */
    printf("[3] Initializing node 1 (nodeID=1) ...\n");
    t0 = GetTickCount64();
    r  = arc_init(ctx1, 1, 0x18, TEST_CLOCK_PRESCALER, true);
    t1 = GetTickCount64();
    printf("    arc_init node1: %s  (%.1f s)\n\n", arc_result_str(r),
           (double)(t1 - t0) / 1000.0);
    if (r != ARC_OK) goto done;

    if (receiver_alive) {
        printf("[3] Initializing node 2 (nodeID=2) ...\n");
        t0 = GetTickCount64();
        r  = arc_init(ctx2, 2, 0x18, TEST_CLOCK_PRESCALER, true);
        t1 = GetTickCount64();
        printf("    arc_init node2: %s  (%.1f s)\n\n", arc_result_str(r),
               (double)(t1 - t0) / 1000.0);
        if (r != ARC_OK) goto done;
    }

    /* ------------------------------------------------------------------ */
    /* [4] Transmit + ACK check (waitAck=true)                            */
    /* ------------------------------------------------------------------ */
    printf("[4] arc_transmit: node1 -> node2  \"ACKTEST1\"  (waitAck=true)\n");
    tx_result = arc_transmit(ctx1, 2, (const uint8_t *)"ACKTEST1", 8, /*waitAck=*/true);
    printf("    arc_transmit: %s", arc_result_str(tx_result));
    if      (tx_result == ARC_OK)        printf("  --> packet SENT and ACKNOWLEDGED\n\n");
    else if (tx_result == ARC_NOT_ACKED) printf("  --> packet sent but NO ACK (node2 absent?)\n\n");
    else                                 printf("  --> USB error\n\n");
    if (tx_result == ARC_ERR_IO) goto done;

    /* Brief pause so hardware has time to process the ACK/NAK */
    Sleep(20);

    /* ------------------------------------------------------------------ */
    /* [5] Read registers 0-7 on node 1 immediately after transmit        */
    /* ------------------------------------------------------------------ */
    printf("[5] POST-TRANSMIT: COM20022 registers 0-7 on node 1\n");
    printf("    (Reg[0] = status; watch for ACK/NAK/TA bits)\n");
    for (uint8_t reg = 0; reg < 8; reg++) {
        val = 0;
        r = arc_register(ctx1, /*bWrite=*/false, reg, &val);
        if (r == ARC_OK)
            print_reg(reg, val, (reg == 0) ? reg0_bits : NULL);
        else
            printf("  Reg[%u] ERROR: %s\n", reg, arc_result_str(r));
    }

    /* ------------------------------------------------------------------ */
    /* [6] Drain EP0x81 on node 1                                         */
    /* ------------------------------------------------------------------ */
    printf("\n[6] POST-TRANSMIT: draining EP0x81 on node 1 (%d ms)\n",
           EVENT_DRAIN_MS);
    drain_events(ctx1, EVENT_DRAIN_MS);

done:
    /* ------------------------------------------------------------------ */
    /* [7] Summary                                                         */
    /* ------------------------------------------------------------------ */
    printf("=====================================================\n");
    printf("  SCENARIO  : receiver %s\n",
           receiver_alive ? "ALIVE" : "DEAD");
    printf("  TX result : %s\n", arc_result_str(tx_result));
    printf("  Expected  : %s\n",
           receiver_alive ? "ARC_OK (TMA bit set)" : "ARC_NOT_ACKED (TMA never set)");
    printf("=====================================================\n\n");

    if (ctx2) { printf("[close node2]\n"); arc_close(ctx2); }
    if (ctx1) { printf("[close node1]\n"); arc_close(ctx1); }
    return 0;
}
