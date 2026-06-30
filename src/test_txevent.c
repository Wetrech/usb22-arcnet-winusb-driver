/*
 * test_txevent.c  --  EP0x81 TX-complete event discovery.
 *
 * Goal: find what EP0x81 pushes after a transmit with and without an ACK,
 * so we can replace the racy reg0/TMA polling with event-driven ACK detection.
 *
 * Method:
 *   arc_transmit(waitAck=false)  -- writes EP0x02, returns immediately, no
 *                                    reg0 polling, EP0x81 untouched by us.
 *   drain_ep81()                 -- reads EP0x81 for DRAIN_MS, prints every
 *                                    packet raw.  Opcode 0x20 events are NOT
 *                                    skipped — we need to see them.
 *
 * SCENARIO A: node1 -> node2 (alive, init'd).  ACK expected.
 *             Expected on EP0x81: some TX-complete event (byte4=?).
 *
 * SCENARIO B: node1 -> node5 (absent, never init'd).  No ACK.
 *             Expected on EP0x81: RECON event (byte4=0x04 as per UPDATE 8).
 *
 * Run both while Wireshark/USBPcap is capturing — see WIRESHARK NOTE below.
 *
 * HARDWARE: two USB22-485 adapters on WinUSB.
 * SKIPPED (exit 0) when fewer than 2 devices are found.
 *
 * Usage: test_txevent.exe [--verbose]
 *
 * -----------------------------------------------------------------------
 * WIRESHARK NOTE (for simultaneous capture):
 *   1. Open Wireshark, select USBPcap interface (usually "USBPcap1" etc.).
 *   2. Start capture, then immediately run this test.
 *   3. Display filter to isolate EP0x81 IN traffic:
 *        usb.endpoint_address == 0x81
 *      or (more specific, once you know bus+device numbers):
 *        usb.bus_id == <N> && usb.device_address == <M> && usb.endpoint_address == 0x81
 *   4. After capture: look for URB_BULK IN packets on EP0x81 that appear
 *      within ~200ms of each EP0x02 OUT (TX write).  Those are the events.
 *      Compare bytes between Scenario A (ACK) and B (no-ACK).
 * -----------------------------------------------------------------------
 */

#include "arcnet.h"
#include "test_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define NODE1        0x01u
#define NODE2        0x02u   /* scenario A: alive (second adapter, init'd) */
#define NODE_DEAD    0x05u   /* scenario B: dead  (no such device)          */
#define PAYLOAD_LEN  14
#define TX_COUNT     5

/* How long to drain EP0x81 after each TX.  Scenario B needs longer because
 * ARCNET reconfiguration can take ~100-250 ms. */
#define DRAIN_MS_A   300u
#define DRAIN_MS_B   600u

/* Per-read timeout inside the drain loop (short so we spin fast). */
#define PER_READ_MS  25u

/* -----------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------- */

static void print_raw(const uint8_t *buf, int len, ULONG elapsed_ms)
{
    int i;
    printf("    [+%4lu ms] EP0x81 (%2d B):", elapsed_ms, len);
    for (i = 0; i < len && i < 32; i++) printf(" %02X", buf[i]);
    if (len > 32) printf(" ...");
    printf("\n");

    if (len >= 6) {
        printf("              opcode=%02X  b1=%02X b2=%02X b3=%02X  "
               "b4=%02X  b5=%02X",
               buf[0], buf[1], buf[2], buf[3], buf[4], buf[5]);
        if (buf[0] == 0x20)
            printf("  <-- opcode 0x20 EVENT  b4=0x%02X", buf[4]);
        printf("\n");
    }
}

/* Read EP0x81 for drain_ms, printing every packet including 0x20 events. */
static int drain_ep81(arc_ctx_t *ctx, ULONG drain_ms)
{
    uint8_t  buf[64];
    int      out_len = 0;
    int      count   = 0;
    ULONGLONG t0     = GetTickCount64();

    while ((GetTickCount64() - t0) < (ULONGLONG)drain_ms) {
        arc_result_t r = arc_read_event(ctx, buf, (int)sizeof(buf),
                                        PER_READ_MS, &out_len);
        ULONG elapsed = (ULONG)(GetTickCount64() - t0);
        if (r == ARC_OK && out_len > 0) {
            print_raw(buf, out_len, elapsed);
            count++;
        }
        /* ARC_NO_PACKET = timeout, keep looping */
    }
    return count;
}

/* Quick one-shot reg0 read (no lock complexity: arc_register handles it). */
static void print_reg0(arc_ctx_t *ctx, const char *tag)
{
    uint8_t reg0 = 0;
    if (arc_register(ctx, false, 0, &reg0) == ARC_OK)
        printf("    reg0 %-8s: 0x%02X  TA=%d TMA=%d\n",
               tag, reg0, reg0 & 0x01, (reg0 >> 1) & 0x01);
}

/* -----------------------------------------------------------------------
 * One scenario: TX_COUNT sends to dest_node, drain EP0x81 after each.
 * --------------------------------------------------------------------- */
static void run_scenario(arc_ctx_t *ctx1,
                         const char *label, uint8_t dest_node,
                         const uint8_t *payload, ULONG drain_ms)
{
    int i;

    printf("\n");
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  SCENARIO %s  node1 (0x%02X) -> node 0x%02X               \n",
           label, NODE1, dest_node);
    printf("║  %s  (%d TX, drain=%lu ms each)          \n",
           (dest_node == NODE2) ? "Receiver ALIVE — ACK expected   "
                                : "Receiver DEAD  — no ACK expected",
           TX_COUNT, drain_ms);
    printf("╚══════════════════════════════════════════════════════════╝\n");

    /* Pre-scenario: flush any residual EP0x81 events. */
    {
        uint8_t tmp[64]; int n;
        printf("  [pre-scenario EP0x81 flush]\n");
        while (arc_read_event(ctx1, tmp, (int)sizeof(tmp), 20, &n) == ARC_OK)
            printf("    residual: %d bytes\n", n);
    }

    for (i = 0; i < TX_COUNT; i++) {
        arc_result_t r;

        printf("\n  ── TX #%d to 0x%02X ──────────────────────────────────\n",
               i + 1, dest_node);

        print_reg0(ctx1, "pre-TX");

        /* waitAck=FALSE: library writes EP0x02 and returns immediately.
         * EP0x81 is NOT read by arc_transmit in this mode. */
        r = arc_transmit(ctx1, dest_node, payload, PAYLOAD_LEN, /*waitAck=*/false);
        printf("  arc_transmit: %s\n", arc_result_str(r));

        if (r != ARC_OK) {
            printf("  TX write failed — skipping EP0x81 drain\n");
            Sleep(500);
            continue;
        }

        /* Immediately drain EP0x81.  The firmware should push a TX-complete
         * or RECON event here.  Print everything raw. */
        int events = drain_ep81(ctx1, drain_ms);

        print_reg0(ctx1, "post-drain");

        if (events == 0)
            printf("  --> NO EP0x81 events in %lu ms\n", drain_ms);
        else
            printf("  --> %d event(s) captured above\n", events);

        /* Brief settle between sends. */
        Sleep(200);
    }
}

/* =======================================================================
 * main
 * ===================================================================== */
int main(int argc, char **argv)
{
    bool verbose = false;
    int  i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0)
            verbose = true;
        else {
            fprintf(stderr, "Unknown arg: %s\n", argv[i]);
            return 1;
        }
    }

    /* ---- Enumerate -------------------------------------------------- */
    char paths[2][256];
    int  n = arc_list_devices(paths, 2);
    if (n < 2) {
        printf("[test_txevent] Need 2 WinUSB devices, found %d -- SKIPPED\n", n);
        return 0;
    }
    printf("[test_txevent] %d device(s) found\n", n);

    /* ---- Open -------------------------------------------------------- */
    arc_ctx_t *ctx1 = arc_open(paths[0], false);
    arc_ctx_t *ctx2 = arc_open(paths[1], false);
    if (!ctx1 || !ctx2) {
        fprintf(stderr, "[test_txevent] arc_open failed\n");
        if (ctx1) arc_close(ctx1);
        if (ctx2) arc_close(ctx2);
        return 1;
    }

    arc_log_level_t lvl = verbose ? ARC_LOG_DEBUG : ARC_LOG_INFO;
    arc_set_log_level(ctx1, lvl);
    arc_set_log_level(ctx2, lvl);

    /* ---- Init -------------------------------------------------------- */
    arc_result_t r;
    r = arc_init(ctx1, NODE1, 0x18, TEST_CLOCK_PRESCALER, false);
    if (r != ARC_OK) {
        fprintf(stderr, "[test_txevent] ctx1 init: %s\n", arc_result_str(r));
        arc_close(ctx1); arc_close(ctx2); return 1;
    }
    printf("[test_txevent] ctx1 node=0x%02X OK\n", NODE1);

    r = arc_init(ctx2, NODE2, 0x18, TEST_CLOCK_PRESCALER, false);
    if (r != ARC_OK) {
        fprintf(stderr, "[test_txevent] ctx2 init: %s\n", arc_result_str(r));
        arc_close(ctx1); arc_close(ctx2); return 1;
    }
    printf("[test_txevent] ctx2 node=0x%02X OK\n", NODE2);

    printf("[test_txevent] waiting 500 ms for network to settle...\n");
    Sleep(500);

    /* ---- Fixed payload ----------------------------------------------- */
    uint8_t payload[PAYLOAD_LEN];
    for (i = 0; i < PAYLOAD_LEN; i++)
        payload[i] = (uint8_t)(0xC0 + i);

    printf("[test_txevent] payload (%d B):", PAYLOAD_LEN);
    for (i = 0; i < PAYLOAD_LEN; i++) printf(" %02X", payload[i]);
    printf("\n");

    /* ---- Scenario A: receiver alive --------------------------------- */
    run_scenario(ctx1, "A", NODE2, payload, DRAIN_MS_A);

    printf("\n[test_txevent] pausing 2 s before scenario B...\n");
    Sleep(2000);

    /* ---- Scenario B: receiver dead ---------------------------------- */
    run_scenario(ctx1, "B", NODE_DEAD, payload, DRAIN_MS_B);

    /* ---- Close ------------------------------------------------------- */
    arc_close(ctx1);
    arc_close(ctx2);
    printf("\n[test_txevent] done.\n");
    return 0;
}
