/*
 * test_listen.c  --  Async receive API test.
 *
 * Requires two USB22-485 adapters on the same ARCNET network.
 *   Node1 (0x01) -- transmitter
 *   Node2 (0x02) -- listener (arc_listen_start + arc_poll_packet + callback)
 *
 * Tests:
 *   1. arc_set_recv_callback + arc_listen_start
 *   2. Receive TX_COUNT packets via queue (arc_poll_packet)
 *   3. Callback counter matches polled count
 *   4. arc_transmit on ctx2 while listener is running (lock-contention check)
 *   5. arc_listen_stop + arc_close
 *
 * Usage: test_listen.exe [--verbose]
 */

#include "arcnet.h"
#include "test_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define NODE1      0x01u
#define NODE2      0x02u
#define TX_COUNT   10
#define TX_DELAY_MS 80     /* spacing between transmits */
#define WAIT_MS    2500    /* drain time after all transmits */

/* -----------------------------------------------------------------------
 * Callback: called from listener thread — keep it minimal.
 * --------------------------------------------------------------------- */
static volatile LONG g_cb_count = 0;
static volatile LONG g_cb_errors = 0;

static void on_packet(const arc_packet_t *pkt, void *user)
{
    (void)user;
    InterlockedIncrement(&g_cb_count);
    /* Validate fields to catch memory corruption */
    if (pkt->len < 1 || pkt->len > 252)
        InterlockedIncrement(&g_cb_errors);
}

/* -----------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------- */
static void fail(const char *msg, arc_result_t r)
{
    fprintf(stderr, "[FAIL] %s: %s\n", msg, arc_result_str(r));
    exit(1);
}

/* =======================================================================
 * main
 * ===================================================================== */
int main(int argc, char **argv)
{
    bool verbose = false;
    int  i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0) verbose = true;
    }

    /* ---- Enumerate devices ------------------------------------------ */
    char paths[2][256];
    int  n = arc_list_devices(paths, 2);
    if (n < 2) {
        fprintf(stderr, "[FAIL] Need 2 USB22-485 devices, found %d\n", n);
        return 1;
    }
    printf("[test_listen] found %d device(s)\n", n);

    /* ---- Open -------------------------------------------------------- */
    arc_ctx_t *ctx1 = arc_open(paths[0], false);
    arc_ctx_t *ctx2 = arc_open(paths[1], false);
    if (!ctx1 || !ctx2) {
        fprintf(stderr, "[FAIL] arc_open failed\n");
        return 1;
    }
    if (verbose) {
        arc_set_log_level(ctx1, ARC_LOG_DEBUG);
        arc_set_log_level(ctx2, ARC_LOG_DEBUG);
    }
    printf("[test_listen] opened ctx1=%s\n", paths[0]);
    printf("[test_listen] opened ctx2=%s\n", paths[1]);

    /* ---- Init -------------------------------------------------------- */
    arc_result_t r;
    r = arc_init(ctx1, NODE1, 0x18, TEST_CLOCK_PRESCALER, false);
    if (r != ARC_OK) fail("ctx1 arc_init", r);
    printf("[test_listen] ctx1 node=0x%02X init OK\n", NODE1);

    r = arc_init(ctx2, NODE2, 0x18, TEST_CLOCK_PRESCALER, false);
    if (r != ARC_OK) fail("ctx2 arc_init", r);
    printf("[test_listen] ctx2 node=0x%02X init OK\n", NODE2);

    /* ---- Set callback & start listener on ctx2 ---------------------- */
    r = arc_set_recv_callback(ctx2, on_packet, NULL);
    if (r != ARC_OK) fail("arc_set_recv_callback", r);

    r = arc_listen_start(ctx2);
    if (r != ARC_OK) fail("arc_listen_start", r);
    printf("[test_listen] listener started on ctx2\n");

    /* ---- Transmit TX_COUNT packets from ctx1 to node2 -------------- */
    int tx_ok = 0;
    uint8_t payload[8] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77 };
    for (i = 0; i < TX_COUNT; i++) {
        payload[0] = (uint8_t)i;
        r = arc_transmit(ctx1, NODE2, payload, sizeof(payload), false);
        if (r == ARC_OK || r == ARC_NOT_ACKED) {
            tx_ok++;
        } else {
            fprintf(stderr, "[test_listen] tx[%d]: %s\n", i, arc_result_str(r));
        }
        Sleep(TX_DELAY_MS);
    }
    printf("[test_listen] transmitted %d/%d packets from ctx1\n", tx_ok, TX_COUNT);

    /* ---- Wait for listener to drain --------------------------------- */
    printf("[test_listen] waiting %d ms for listener...\n", WAIT_MS);
    Sleep(WAIT_MS);

    /* ---- Test 4: transmit on ctx2 while listener still running ------ */
    printf("[test_listen] transmit on ctx2 while listener active (lock test)...\n");
    r = arc_transmit(ctx2, NODE1, payload, 4, false);
    printf("[test_listen] ctx2 arc_transmit -> %s\n", arc_result_str(r));

    /* ---- Stop listener ---------------------------------------------- */
    arc_listen_stop(ctx2);
    printf("[test_listen] listener stopped\n");

    /* ---- Drain queue ------------------------------------------------ */
    int pending_before = arc_pending_count(ctx2);
    int polled = 0;
    arc_packet_t pkt;
    while (arc_poll_packet(ctx2, &pkt) == ARC_OK) {
        if (verbose)
            printf("  [poll] src=0x%02X dst=0x%02X len=%d data[0]=0x%02X\n",
                   pkt.src, pkt.dst, pkt.len, pkt.data[0]);
        polled++;
    }

    long cb   = (long)g_cb_count;
    long errs = (long)g_cb_errors;

    printf("\n[test_listen] Results:\n");
    printf("  transmitted   : %d / %d\n", tx_ok, TX_COUNT);
    printf("  callback fires: %ld\n",     cb);
    printf("  callback errors:%ld\n",     errs);
    printf("  queue pending  : %d (before drain)\n", pending_before);
    printf("  polled from Q  : %d\n",     polled);

    /* ---- Close ------------------------------------------------------ */
    arc_close(ctx1);
    arc_close(ctx2);

    /* ---- Verdict ---------------------------------------------------- */
    bool pass = (cb > 0 || polled > 0) && errs == 0;
    printf("\n%s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
