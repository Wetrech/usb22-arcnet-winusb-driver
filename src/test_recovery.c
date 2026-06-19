/*
 * test_recovery.c  --  Device unplug / reconnect diagnostic.
 *
 * Sequence:
 *   1. Open + init node 1.
 *   2. Poll loop (~500 ms): arc_register(reg0) + arc_transmit(waitAck=false).
 *      Log every return code and GetLastError.
 *   3. On ARC_ERR_DEVICE_GONE: stop polling, enter reconnect loop.
 *      Every ~2 s: arc_reopen() + arc_init().  On success resume step 2.
 *
 * [TEST] prefix = this test's own output.
 * [arcnet] prefix = library verbose output.
 *
 * Stop with Ctrl+C.
 */

#include "arcnet.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>

#define POLL_INTERVAL_MS    500
#define RECONNECT_DELAY_MS 2000
#define MAX_DEVS            8

/* -----------------------------------------------------------------------
 * poll_loop  --  returns when device_gone is detected
 * --------------------------------------------------------------------- */
static void poll_loop(arc_ctx_t *ctx)
{
    arc_result_t r;
    uint8_t      reg0;
    DWORD        gle;
    unsigned long iter = 0;

    printf("[TEST] --- Entering poll loop. Unplug device whenever ready. ---\n\n");

    while (1) {
        iter++;
        printf("[TEST] ---- iter=%lu  t=%.1f s ----\n",
               iter, (double)GetTickCount64() / 1000.0);

        /* (a) Register 0 read */
        reg0 = 0;
        SetLastError(0);
        r   = arc_register(ctx, /*bWrite=*/false, /*reg=*/0, &reg0);
        gle = GetLastError();
        if (r == ARC_ERR_DEVICE_GONE) {
            printf("[TEST] arc_register(reg0): ARC_ERR_DEVICE_GONE  GLE=%lu  "
                   "--> device removed!\n\n", gle);
            return;
        }
        printf("[TEST] arc_register(reg0): %-14s  val=0x%02X  bits=",
               arc_result_str(r), reg0);
        for (int b = 7; b >= 0; b--) putchar((reg0 >> b) & 1 ? '1' : '0');
        printf("b  GLE=%lu\n", gle);

        /* (b) Transmit to node 2 (waitAck=false) */
        SetLastError(0);
        r   = arc_transmit(ctx, 2, (const uint8_t *)"ALIVE", 5, /*waitAck=*/false);
        gle = GetLastError();
        if (r == ARC_ERR_DEVICE_GONE) {
            printf("[TEST] arc_transmit:       ARC_ERR_DEVICE_GONE  GLE=%lu  "
                   "--> device removed!\n\n", gle);
            return;
        }
        printf("[TEST] arc_transmit:       %-14s  GLE=%lu\n\n",
               arc_result_str(r), gle);

        Sleep(POLL_INTERVAL_MS);
    }
}

/* -----------------------------------------------------------------------
 * reconnect_loop  --  blocks until reopen+init succeeds, then returns
 * --------------------------------------------------------------------- */
static void reconnect_loop(arc_ctx_t *ctx)
{
    arc_result_t r;
    unsigned long attempt = 0;

    printf("[TEST] === DEVICE GONE — waiting for reconnect "
           "(retry every %d ms) ===\n\n", RECONNECT_DELAY_MS);

    while (1) {
        attempt++;
        printf("[TEST] [reconnect attempt %lu  t=%.1f s]\n",
               attempt, (double)GetTickCount64() / 1000.0);

        r = arc_reopen(ctx);
        printf("[TEST] arc_reopen: %s\n", arc_result_str(r));

        if (r == ARC_OK) {
            printf("[TEST] arc_reopen OK — calling arc_init ...\n");
            r = arc_init(ctx, 1, 0x18, 0x00, true);
            printf("[TEST] arc_init: %s\n", arc_result_str(r));
            if (r == ARC_OK) {
                printf("[TEST] === RECONNECTED AND INITIALIZED ===\n\n");
                return;
            }
            printf("[TEST] arc_init failed after reopen — will retry\n\n");
        } else {
            printf("[TEST] Device still absent.\n\n");
        }

        Sleep(RECONNECT_DELAY_MS);
    }
}

/* -----------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */
int main(void)
{
    char       paths[MAX_DEVS][256];
    int        n;
    arc_ctx_t *ctx = NULL;
    arc_result_t r;
    ULONGLONG  t0, t1;

    printf("[TEST] =====================================================\n");
    printf("[TEST]  USB22-485 Unplug Recovery Diagnostic\n");
    printf("[TEST]  Poll: %d ms   Reconnect retry: %d ms\n",
           POLL_INTERVAL_MS, RECONNECT_DELAY_MS);
    printf("[TEST] =====================================================\n\n");

    /* Enumerate */
    n = arc_list_devices(paths, MAX_DEVS);
    printf("[TEST] arc_list_devices: %d device(s)\n", n);
    if (n < 1) {
        fprintf(stderr, "[TEST] No devices found. Exiting.\n");
        return 1;
    }
    printf("[TEST] Using: %s\n\n", paths[0]);

    /* Open */
    ctx = arc_open(paths[0], /*verbose=*/true);
    if (!ctx) {
        fprintf(stderr, "[TEST] arc_open FAILED. Exiting.\n");
        return 1;
    }
    printf("[TEST] arc_open: OK\n\n");

    /* Init */
    printf("[TEST] arc_init (nodeID=1) ...\n");
    t0 = GetTickCount64();
    r  = arc_init(ctx, 1, 0x18, 0x00, true);
    t1 = GetTickCount64();
    printf("[TEST] arc_init: %s  (%.1f s)\n\n",
           arc_result_str(r), (double)(t1 - t0) / 1000.0);

    if (r != ARC_OK) {
        fprintf(stderr, "[TEST] Init failed. Exiting.\n");
        arc_close(ctx);
        return 1;
    }

    /* Main loop: poll -> detect gone -> reconnect -> poll -> ... */
    while (1) {
        poll_loop(ctx);
        reconnect_loop(ctx);
    }

    arc_close(ctx);
    return 0;
}
