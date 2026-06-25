/*
 * test_lifecycle.c  --  Handle / resource hygiene stress test.
 *
 * Verifies that repeated arc_open / arc_close cycles do not leak OS handles
 * or heap memory.  The test itself measures and prints handle count and
 * working-set size at regular intervals so you can watch for growth without
 * opening Task Manager.
 *
 * Sections
 * --------
 *   A. FAST loop (N_FAST iters): arc_open -> arc_close, NO arc_init.
 *      Tests CreateFile + WinUsb_Initialize + WinUsb_Free + CloseHandle hygiene.
 *      ~1-2 ms per iteration; total runtime < 1 s.
 *
 *   B. FULL loop (N_FULL iters): arc_open -> arc_init -> register(x3) +
 *      transmit(x2) -> arc_close.
 *      Tests the complete protocol path.  arc_init takes ~2.5 s, so
 *      N_FULL=20 => ~50-60 s total.
 *
 *   C. Edge cases:
 *      C1. open -> close without init, then open again (handles released?).
 *      C2. arc_close(NULL) — must be a silent no-op (no crash).
 *
 * Expected outcome: handle count and working set stable across all sections;
 * no crashes; 0 unexpected errors.
 */

#include "arcnet.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <psapi.h>

#define N_FAST        300     /* Section A: open/close only        */
#define N_FULL         20     /* Section B: full init/use/close    */
#define N_EDGE_C1       5     /* Section C1: no-init reopen check  */

#define REPORT_EVERY_A  100   /* Section A: print every N iters    */
#define REPORT_EVERY_B    5   /* Section B: print every N iters    */

#define MAX_DEVS          8

/* -----------------------------------------------------------------------
 * print_resources  --  OS handle count + working-set via Win32 APIs
 * --------------------------------------------------------------------- */
static void print_resources(int iter, const char *label)
{
    DWORD                    handle_count = 0;
    PROCESS_MEMORY_COUNTERS  pmc;

    GetProcessHandleCount(GetCurrentProcess(), &handle_count);
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        printf("  [res] %-6s iter=%4d  handles=%4lu  WS=%5.0f KB  peakWS=%5.0f KB\n",
               label, iter,
               (ULONG)handle_count,
               (double)pmc.WorkingSetSize     / 1024.0,
               (double)pmc.PeakWorkingSetSize / 1024.0);
    } else {
        printf("  [res] %-6s iter=%4d  handles=%4lu  (memory info N/A GLE=%lu)\n",
               label, iter, (ULONG)handle_count, GetLastError());
    }
}

/* -----------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */
int main(void)
{
    char          paths[MAX_DEVS][256];
    int           n;
    arc_ctx_t    *ctx;
    arc_result_t  r;
    ULONGLONG     t0, t1;
    int           i, j;

    /* Section counters */
    int  a_open_ok = 0, a_open_fail = 0;
    int  b_full_ok = 0, b_init_fail = 0, b_open_fail = 0, b_other = 0;
    int  unexpected = 0;

    printf("============================================================\n");
    printf("  Handle / Resource Hygiene Stress Test\n");
    printf("  Section A: %d x (open -> close)                 [fast]\n", N_FAST);
    printf("  Section B: %d x (open -> init -> use -> close)  [~%.0f s]\n",
           N_FULL, N_FULL * 3.0);
    printf("  Section C: edge cases\n");
    printf("============================================================\n\n");

    /* Enumerate once */
    n = arc_list_devices(paths, MAX_DEVS);
    printf("[enum] %d device(s) found\n", n);
    if (n < 1) {
        fprintf(stderr, "No devices found. Exiting.\n");
        return 1;
    }
    printf("[path] %s\n\n", paths[0]);

    /* Baseline */
    print_resources(0, "base");
    printf("\n");

    /* ==================================================================
     * Section A: FAST open/close loop (no arc_init)
     * ================================================================== */
    printf("--- Section A: FAST loop (%d iters, no init) ---\n", N_FAST);
    t0 = GetTickCount64();

    for (i = 1; i <= N_FAST; i++) {
        ctx = arc_open(paths[0], /*verbose=*/false);
        if (ctx) {
            a_open_ok++;
            arc_close(ctx);
            ctx = NULL;
        } else {
            a_open_fail++;
        }

        if (i % REPORT_EVERY_A == 0 || i == N_FAST)
            print_resources(i, "A");
    }

    t1 = GetTickCount64();
    printf("  A done: open_ok=%d  open_fail=%d  (%.2f s)\n\n",
           a_open_ok, a_open_fail, (double)(t1 - t0) / 1000.0);

    /* ==================================================================
     * Section B: FULL loop (open -> init -> register/transmit -> close)
     * ================================================================== */
    printf("--- Section B: FULL loop (%d iters, with init ~2.5 s each) ---\n", N_FULL);
    t0 = GetTickCount64();

    for (i = 1; i <= N_FULL; i++) {
        uint8_t  src, dst, rxbuf[253];
        int      rxlen;
        uint8_t  reg0;

        ctx = arc_open(paths[0], /*verbose=*/false);
        if (!ctx) {
            b_open_fail++;
            printf("  [B %2d] arc_open FAIL\n", i);
            if (i % REPORT_EVERY_B == 0 || i == N_FULL)
                print_resources(i, "B");
            continue;
        }

        r = arc_init(ctx, 1, 0x18, 0x00, /*recvBroadcasts=*/true);
        if (r != ARC_OK) {
            b_init_fail++;
            printf("  [B %2d] arc_init FAIL: %s -- closing cleanly\n",
                   i, arc_result_str(r));
            arc_close(ctx); ctx = NULL;
            if (i % REPORT_EVERY_B == 0 || i == N_FULL)
                print_resources(i, "B");
            continue;
        }

        /* 3x register reads */
        for (j = 0; j < 3; j++) {
            reg0 = 0;
            r = arc_register(ctx, false, 0, &reg0);
            if (r != ARC_OK && r != ARC_ERR_NET_BUSY && r != ARC_NOT_ACKED) {
                if (r == ARC_ERR_DEVICE_GONE) goto b_close;
                b_other++;
            }
        }

        /* 2x transmit (no ACK wait, destination node 2) */
        for (j = 0; j < 2; j++) {
            r = arc_transmit(ctx, 2,
                             (const uint8_t *)"LIFECYCLE", 9,
                             /*waitAck=*/false);
            if (r != ARC_OK && r != ARC_ERR_NET_BUSY && r != ARC_NOT_ACKED) {
                if (r == ARC_ERR_DEVICE_GONE) goto b_close;
                b_other++;
            }
        }

        /* 1x receive poll (expect ARC_NO_PACKET — OK) */
        r = arc_receive(ctx, &src, &dst, rxbuf, &rxlen);
        if (r != ARC_OK && r != ARC_NO_PACKET) {
            if (r == ARC_ERR_DEVICE_GONE) goto b_close;
            b_other++;
        }

        b_full_ok++;

b_close:
        arc_close(ctx); ctx = NULL;

        if (i % REPORT_EVERY_B == 0 || i == N_FULL)
            print_resources(i, "B");
    }

    t1 = GetTickCount64();
    printf("  B done: full_ok=%d  init_fail=%d  open_fail=%d  other=%d  (%.1f s)\n\n",
           b_full_ok, b_init_fail, b_open_fail, b_other, (double)(t1 - t0) / 1000.0);

    /* ==================================================================
     * Section C: Edge cases
     * ================================================================== */
    printf("--- Section C: Edge cases ---\n");

    /* C1: open -> close (no init) -> open again -- handles must be released */
    printf("  [C1] open-without-init-close, then reopen (%d rounds)\n", N_EDGE_C1);
    {
        int c1_ok = 0, c1_fail = 0;
        for (i = 0; i < N_EDGE_C1; i++) {
            ctx = arc_open(paths[0], false);
            if (!ctx) { c1_fail++; continue; }
            arc_close(ctx); ctx = NULL;          /* close without init   */

            ctx = arc_open(paths[0], false);     /* reopen immediately   */
            if (ctx) {
                c1_ok++;
                arc_close(ctx); ctx = NULL;
            } else {
                c1_fail++;
                printf("    round %d: reopen after no-init-close FAIL\n", i);
            }
        }
        printf("    reopen-ok=%d  reopen-fail=%d  %s\n",
               c1_ok, c1_fail, c1_fail == 0 ? "PASS" : "FAIL");
        if (c1_fail) unexpected++;
    }
    print_resources(0, "C1");

    /* C2: arc_close(NULL) -- must be a silent no-op */
    printf("  [C2] arc_close(NULL) x3 -- must not crash\n");
    arc_close(NULL);
    arc_close(NULL);
    arc_close(NULL);
    printf("    PASS (no crash)\n");
    print_resources(0, "C2");

    /* ==================================================================
     * Summary
     * ================================================================== */
    printf("\n============================================================\n");
    printf("  SUMMARY\n");
    printf("  Section A  open/close only : ok=%d  fail=%d\n",
           a_open_ok, a_open_fail);
    printf("  Section B  full cycle      : ok=%d  init_fail=%d"
           "  open_fail=%d  proto_other=%d\n",
           b_full_ok, b_init_fail, b_open_fail, b_other);
    printf("  Section C  edge cases      : unexpected=%d\n", unexpected);
    printf("\n");

    /* Final resource snapshot */
    print_resources(-1, "final");

    int pass = (unexpected == 0 && a_open_fail == 0 && b_open_fail == 0);
    printf("\n  --> %s\n", pass ? "PASS" : "FAIL (see above)");
    printf("============================================================\n");

    return pass ? 0 : 1;
}
