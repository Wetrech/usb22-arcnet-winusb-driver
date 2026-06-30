/*
 * test_multi.c  --  Simultaneous two-device open/init/transmit/receive diagnostic.
 *
 * Diagnostic questions:
 *   1. Can two USB22-485 devices be opened and initialised concurrently?
 *   2. Can device 0 (node 1) transmit to device 1 (node 2) and vice-versa,
 *      while both contexts remain open at the same time?
 *   3. Does arc_receive return promptly when no packet arrives (non-blocking)?
 *
 * Test sequence:
 *   [1] arc_list_devices     enumerate all paths
 *   [2] arc_open x2          open both; verify both handles are independent
 *   [3] arc_init x2          init node 1 and node 2 simultaneously
 *   [4] TX[0]->RX[1]         node 1 transmits "MULTI001"; node 2 polls ~3 s
 *   [5] TX[1]->RX[0]         node 2 transmits "MULTI002"; node 1 polls ~3 s
 *   [6] Summary + close
 */

#include "arcnet.h"
#include "test_common.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>

#define MAX_DEVS      8
#define RECV_BUDGET_S 3     /* seconds to poll for a packet */

/* -----------------------------------------------------------------------
 * recv_loop
 *   Polls arc_receive for up to budget_sec seconds.
 *   Returns  1 = packet received,  0 = timeout,  -1 = hardware error.
 * --------------------------------------------------------------------- */
static int recv_loop(arc_ctx_t *ctx, int node_id, int budget_sec)
{
    ULONGLONG    start = GetTickCount64();
    uint8_t      src, dst;
    uint8_t      data[256];
    int          data_len, i;
    arc_result_t r;
    ULONGLONG    elapsed;

    printf("[recv node%d] Listening for %d s (non-blocking)...\n",
           node_id, budget_sec);

    while (1) {
        elapsed = GetTickCount64() - start;
        if (elapsed >= (ULONGLONG)(budget_sec * 1000))
            break;

        data_len = 0;
        r = arc_receive(ctx, &src, &dst, data, &data_len);

        if (r == ARC_OK) {
            printf("[recv node%d] === PACKET RECEIVED ===\n", node_id);
            printf("[recv node%d]   src=%u  dst=%u  len=%d\n",
                   node_id, src, dst, data_len);
            printf("[recv node%d]   ASCII: \"", node_id);
            for (i = 0; i < data_len; i++)
                putchar((data[i] >= 0x20 && data[i] < 0x7F) ? data[i] : '.');
            printf("\"\n");
            printf("[recv node%d]   Hex  :", node_id);
            for (i = 0; i < data_len; i++) printf(" %02X", data[i]);
            printf("\n");
            printf("[recv node%d]   Time : %.2f s\n\n",
                   node_id, (double)elapsed / 1000.0);
            return 1;
        } else if (r == ARC_ERR_IO) {
            printf("[recv node%d] Hardware error during receive.\n\n", node_id);
            return -1;
        }
        /* ARC_NO_PACKET: arc_receive returned within ~200 ms; loop again */
    }

    printf("[recv node%d] No packet in %d s (receive loop exited cleanly).\n\n",
           node_id, budget_sec);
    return 0;
}

/* -----------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */
int main(void)
{
    char         paths[MAX_DEVS][256];
    int          n, i;
    arc_ctx_t   *ctx[2]    = { NULL, NULL };
    arc_result_t r;
    int          open_ok[2]  = { 0, 0 };
    int          init_ok[2]  = { 0, 0 };
    int          tx01_ok     = 0;   /* node1 -> node2 transmit */
    int          rx01_result = 0;   /* node2 receive */
    int          tx10_ok     = 0;   /* node2 -> node1 transmit */
    int          rx10_result = 0;   /* node1 receive */
    ULONGLONG    t0, t1;

    printf("=====================================================\n");
    printf("  USB22-485 Multi-Device Transmit/Receive Test\n");
    printf("=====================================================\n\n");

    /* ------------------------------------------------------------------ */
    /* [1] Enumerate devices                                               */
    /* ------------------------------------------------------------------ */
    n = arc_list_devices(paths, MAX_DEVS);
    printf("[enum] arc_list_devices: %d device(s) found\n", n);
    for (i = 0; i < n; i++)
        printf("  [%d] %s\n", i, paths[i]);
    printf("\n");

    if (n < 2) {
        printf("  --> Need at least 2 devices; only %d present. Exiting.\n\n", n);
        return 1;
    }

    /* ------------------------------------------------------------------ */
    /* [2] Open both devices                                               */
    /* ------------------------------------------------------------------ */
    printf("[open 0] %s\n", paths[0]);
    t0 = GetTickCount64();
    ctx[0] = arc_open(paths[0], /*verbose=*/true);
    t1 = GetTickCount64();
    open_ok[0] = (ctx[0] != NULL);
    printf("[open 0] %s  (%.0f ms)\n\n", open_ok[0] ? "OK" : "FAILED", (double)(t1 - t0));

    printf("[open 1] %s\n", paths[1]);
    t0 = GetTickCount64();
    ctx[1] = arc_open(paths[1], /*verbose=*/true);
    t1 = GetTickCount64();
    open_ok[1] = (ctx[1] != NULL);
    printf("[open 1] %s  (%.0f ms)\n\n", open_ok[1] ? "OK" : "FAILED", (double)(t1 - t0));

    if (!open_ok[0] || !open_ok[1]) {
        printf("  --> Open failed; cannot continue.\n\n");
        goto summary;
    }

    /* ------------------------------------------------------------------ */
    /* [3] Init device 0 as node 1, device 1 as node 2                   */
    /* ------------------------------------------------------------------ */
    printf("[init 0] nodeID=1 ...\n");
    t0 = GetTickCount64();
    r  = arc_init(ctx[0], 1, 0x18, TEST_CLOCK_PRESCALER, true);
    t1 = GetTickCount64();
    init_ok[0] = (r == ARC_OK);
    printf("[init 0] %s  (%.1f s)\n\n", arc_result_str(r), (double)(t1 - t0) / 1000.0);

    printf("[init 1] nodeID=2 ...\n");
    t0 = GetTickCount64();
    r  = arc_init(ctx[1], 2, 0x18, TEST_CLOCK_PRESCALER, true);
    t1 = GetTickCount64();
    init_ok[1] = (r == ARC_OK);
    printf("[init 1] %s  (%.1f s)\n\n", arc_result_str(r), (double)(t1 - t0) / 1000.0);

    if (!init_ok[0] || !init_ok[1]) {
        printf("  --> Init failed; skipping transmit/receive.\n\n");
        goto summary;
    }

    /* ------------------------------------------------------------------ */
    /* [4] Direction A: node 1 -> node 2                                  */
    /* ------------------------------------------------------------------ */
    printf("-----------------------------------------------------\n");
    printf("[tx  0->1] node1 transmitting \"MULTI001\" to node2...\n");
    r = arc_transmit(ctx[0], 2, (const uint8_t *)"MULTI001", 8, /*waitAck=*/true);
    tx01_ok = (r == ARC_OK);
    printf("[tx  0->1] arc_transmit: %s\n\n", arc_result_str(r));

    rx01_result = recv_loop(ctx[1], /*node_id=*/2, RECV_BUDGET_S);

    /* ------------------------------------------------------------------ */
    /* [5] Direction B: node 2 -> node 1                                  */
    /* ------------------------------------------------------------------ */
    printf("-----------------------------------------------------\n");
    printf("[tx  1->0] node2 transmitting \"MULTI002\" to node1...\n");
    r = arc_transmit(ctx[1], 1, (const uint8_t *)"MULTI002", 8, /*waitAck=*/true);
    tx10_ok = (r == ARC_OK);
    printf("[tx  1->0] arc_transmit: %s\n\n", arc_result_str(r));

    rx10_result = recv_loop(ctx[0], /*node_id=*/1, RECV_BUDGET_S);

summary:
    /* ------------------------------------------------------------------ */
    /* [6] Summary                                                         */
    /* ------------------------------------------------------------------ */
    printf("=====================================================\n");
    printf("  SUMMARY\n");
    printf("  open : [0]=%s  [1]=%s\n",
           open_ok[0] ? "OK" : "FAIL", open_ok[1] ? "OK" : "FAIL");
    printf("  init : [0]=%s  [1]=%s\n",
           init_ok[0] ? "OK" : "FAIL", init_ok[1] ? "OK" : "FAIL");

    if (init_ok[0] && init_ok[1]) {
        printf("  TX 0->1: %s  |  RX node2: %s\n",
               tx01_ok ? "OK" : "FAIL",
               rx01_result > 0 ? "PACKET RECEIVED" :
               rx01_result == 0 ? "no packet" : "HW ERROR");
        printf("  TX 1->0: %s  |  RX node1: %s\n",
               tx10_ok ? "OK" : "FAIL",
               rx10_result > 0 ? "PACKET RECEIVED" :
               rx10_result == 0 ? "no packet" : "HW ERROR");

        if (rx01_result > 0 && rx10_result > 0)
            printf("\n  --> FULL PASS: bidirectional transmit/receive OK.\n");
        else if (rx01_result == 0 || rx10_result == 0)
            printf("\n  --> PARTIAL: TX succeeded but packet(s) not received.\n"
                   "      Check ARCNET bus wiring and Wireshark capture.\n");
        else
            printf("\n  --> FAIL: hardware errors during receive.\n");
    }
    printf("=====================================================\n\n");

    /* ------------------------------------------------------------------ */
    /* Close both (both contexts remain open throughout the test)         */
    /* ------------------------------------------------------------------ */
    for (i = 0; i < 2; i++) {
        if (ctx[i]) {
            printf("[close %d]\n", i);
            arc_close(ctx[i]);
        }
    }

    return (open_ok[0] && open_ok[1] &&
            init_ok[0] && init_ok[1] &&
            rx01_result > 0 && rx10_result > 0) ? 0 : 1;
}
