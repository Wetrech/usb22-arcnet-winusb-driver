/*
 * test_threads.c  --  Per-context thread-safety stress test.
 *
 * Two worker threads share the SAME arc_ctx_t concurrently for 20 s:
 *   Thread TX : arc_transmit() every TX_INTERVAL_MS
 *   Thread REG: arc_register(read reg0) every REG_INTERVAL_MS
 *
 * The CRITICAL_SECTION inside each context must prevent cmd/response
 * channel interleaving on EP0x01/EP0x81 and pipe state races on EP0x02.
 * Expected: no crash, no deadlock, clean exit after 20 s.
 *
 * Library verbose is OFF so only this test's own output appears.
 */

#include "arcnet.h"
#include "test_common.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>

#define TEST_DURATION_MS  20000
#define TX_INTERVAL_MS      200
#define REG_INTERVAL_MS     150
#define MAX_DEVS              8

typedef struct {
    arc_ctx_t    *ctx;
    volatile LONG *stop;
    const char    *name;
    LONG           ok;
    LONG           not_acked;
    LONG           net_busy;
    LONG           device_gone;
    LONG           other;        /* any result not in the list above */
} worker_t;

/* -----------------------------------------------------------------------
 * tx_worker  --  calls arc_transmit in a loop until stop flag is set
 * --------------------------------------------------------------------- */
static DWORD WINAPI tx_worker(LPVOID arg)
{
    worker_t      *w       = (worker_t *)arg;
    const uint8_t  payload[] = "THREAD_TX";
    arc_result_t   r;

    while (*w->stop == 0) {
        r = arc_transmit(w->ctx, 2,
                         payload, (int)(sizeof(payload) - 1),
                         /*waitAck=*/false);
        switch (r) {
        case ARC_OK:              InterlockedIncrement(&w->ok);          break;
        case ARC_NOT_ACKED:       InterlockedIncrement(&w->not_acked);   break;
        case ARC_ERR_NET_BUSY:    InterlockedIncrement(&w->net_busy);    break;
        case ARC_ERR_DEVICE_GONE: InterlockedIncrement(&w->device_gone); break;
        default:                  InterlockedIncrement(&w->other);       break;
        }
        Sleep(TX_INTERVAL_MS);
    }
    return 0;
}

/* -----------------------------------------------------------------------
 * reg_worker  --  calls arc_register(read reg0) in a loop
 * --------------------------------------------------------------------- */
static DWORD WINAPI reg_worker(LPVOID arg)
{
    worker_t     *w = (worker_t *)arg;
    arc_result_t  r;
    uint8_t       reg0;

    while (*w->stop == 0) {
        reg0 = 0;
        r = arc_register(w->ctx, /*bWrite=*/false, 0, &reg0);
        switch (r) {
        case ARC_OK:              InterlockedIncrement(&w->ok);          break;
        case ARC_NOT_ACKED:       InterlockedIncrement(&w->not_acked);   break;
        case ARC_ERR_NET_BUSY:    InterlockedIncrement(&w->net_busy);    break;
        case ARC_ERR_DEVICE_GONE: InterlockedIncrement(&w->device_gone); break;
        default:                  InterlockedIncrement(&w->other);       break;
        }
        Sleep(REG_INTERVAL_MS);
    }
    return 0;
}

/* -----------------------------------------------------------------------
 * print_worker
 * --------------------------------------------------------------------- */
static void print_worker(const worker_t *w)
{
    LONG total = w->ok + w->not_acked + w->net_busy + w->device_gone + w->other;
    printf("  %-4s  calls=%-4ld  OK=%-4ld  NOT_ACKED=%-3ld"
           "  NET_BUSY=%-3ld  DEVICE_GONE=%-3ld  other=%-3ld\n",
           w->name, total,
           w->ok, w->not_acked, w->net_busy, w->device_gone, w->other);
}

/* -----------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */
int main(void)
{
    char          paths[MAX_DEVS][256];
    int           n;
    arc_ctx_t    *ctx = NULL;
    arc_result_t  r;
    ULONGLONG     t0, t1;
    volatile LONG stop     = 0;
    worker_t      tx_w     = {0};
    worker_t      reg_w    = {0};
    HANDLE        threads[2];
    DWORD         wait_res;
    LONG          total_other;

    printf("==========================================================\n");
    printf("  Thread-Safety Stress Test  (duration: %d s)\n",
           TEST_DURATION_MS / 1000);
    printf("  One context, two concurrent threads:\n");
    printf("    TX  thread: arc_transmit()        every %d ms\n", TX_INTERVAL_MS);
    printf("    REG thread: arc_register(reg0 rd) every %d ms\n", REG_INTERVAL_MS);
    printf("  Library verbose: OFF\n");
    printf("==========================================================\n\n");

    /* Enumerate */
    n = arc_list_devices(paths, MAX_DEVS);
    printf("[enum] %d device(s) found\n", n);
    if (n < 1) {
        fprintf(stderr, "No devices found. Exiting.\n");
        return 1;
    }
    printf("[open] %s\n\n", paths[0]);

    /* Open — verbose off from the start */
    ctx = arc_open(paths[0], /*verbose=*/false);
    if (!ctx) {
        fprintf(stderr, "arc_open failed.\n");
        return 1;
    }

    /* Init */
    printf("[init] arc_init (nodeID=1) ...\n");
    t0 = GetTickCount64();
    r  = arc_init(ctx, 1, 0x18, TEST_CLOCK_PRESCALER, /*recvBroadcasts=*/true);
    t1 = GetTickCount64();
    printf("[init] %s  (%.1f s)\n\n", arc_result_str(r), (double)(t1 - t0) / 1000.0);
    if (r != ARC_OK) {
        arc_close(ctx);
        return 1;
    }

    /* Confirm verbose is off (arc_open already set it, but be explicit) */
    arc_set_verbose(ctx, false);

    /* Wire up workers */
    tx_w.ctx  = ctx;  tx_w.stop  = &stop;  tx_w.name  = "TX";
    reg_w.ctx = ctx;  reg_w.stop = &stop;  reg_w.name = "REG";

    /* Launch */
    printf("[start] Launching TX and REG threads ...\n");
    threads[0] = CreateThread(NULL, 0, tx_worker,  &tx_w,  0, NULL);
    threads[1] = CreateThread(NULL, 0, reg_worker, &reg_w, 0, NULL);
    if (!threads[0] || !threads[1]) {
        fprintf(stderr, "CreateThread failed GLE=%lu\n", GetLastError());
        InterlockedExchange(&stop, 1);
        if (threads[0]) { WaitForSingleObject(threads[0], 3000); CloseHandle(threads[0]); }
        if (threads[1]) { WaitForSingleObject(threads[1], 3000); CloseHandle(threads[1]); }
        arc_close(ctx);
        return 1;
    }

    /* Run */
    printf("[run]   Running for %d s ...\n", TEST_DURATION_MS / 1000);
    t0 = GetTickCount64();
    Sleep(TEST_DURATION_MS);

    /* Stop */
    InterlockedExchange(&stop, 1);
    printf("[stop]  Stop signal sent, waiting for threads (timeout 5 s) ...\n");
    wait_res = WaitForMultipleObjects(2, threads, /*waitAll=*/TRUE, 5000);
    CloseHandle(threads[0]);
    CloseHandle(threads[1]);
    t1 = GetTickCount64();

    if (wait_res == WAIT_TIMEOUT) {
        printf("\n[WARN]  WaitForMultipleObjects timed out -- possible deadlock!\n");
    }

    /* Results */
    printf("\n[RESULTS]  wall time: %.1f s\n", (double)(t1 - t0) / 1000.0);
    print_worker(&tx_w);
    print_worker(&reg_w);

    total_other = tx_w.other + reg_w.other;
    printf("\n  Unexpected (other) errors : %ld\n", total_other);
    printf("  Deadlock detected         : %s\n",
           wait_res == WAIT_TIMEOUT ? "YES (FAIL)" : "no");
    printf("\n  --> %s\n",
           (total_other == 0 && wait_res != WAIT_TIMEOUT) ? "PASS" : "FAIL");

    arc_close(ctx);
    return (total_other == 0 && wait_res != WAIT_TIMEOUT) ? 0 : 1;
}
