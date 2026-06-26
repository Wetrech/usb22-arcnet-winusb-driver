/*
 * test_closequiet.c  --  Functional silence: arc_close vs arc_shutdown.
 *
 * Question: after closing node2, is it functionally DEAD?
 *   - Does node1->node2 TX return NOT_ACKED? (good: no more ACKs)
 *   - Does node1 receive any data from node2? (should be nothing)
 *
 * Both variants run in one pass:
 *   VARIANT A: arc_close(ctx2)    -- USB released, chip stays on bus (light on)
 *   VARIANT B: arc_shutdown(ctx2) -- RST + USB released
 *
 * Between variants, ctx2 is re-opened and re-initialised so the baseline
 * (working ACK) is confirmed fresh before each close.
 *
 * Usage: test_closequiet.exe [--verbose]
 * HARDWARE: two USB22-485 adapters on WinUSB.
 */

#include "arcnet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define NODE1        0x01u
#define NODE2        0x02u
#define TX_REPEATS   5          /* transmits after close */
#define LISTEN_MS    3000       /* receive window after close */

static arc_log_level_t g_lvl;

/* ── helpers ──────────────────────────────────────────────────────────── */

static arc_ctx_t *open_init(const char *path, uint8_t node, const char *tag)
{
    arc_ctx_t *ctx = arc_open(path, false);
    if (!ctx) { fprintf(stderr, "[%s] arc_open failed\n", tag); return NULL; }
    arc_set_log_level(ctx, g_lvl);
    arc_result_t r = arc_init(ctx, node, 0x18, 0x00, false);
    if (r != ARC_OK) {
        fprintf(stderr, "[%s] arc_init: %s\n", tag, arc_result_str(r));
        arc_close(ctx);
        return NULL;
    }
    printf("  [%s] node=0x%02X init OK\n", tag, node);
    return ctx;
}

/* Send TX_REPEATS frames node1->node2 with waitAck, print outcome summary. */
static void tx_probe(arc_ctx_t *ctx1, const char *phase)
{
    static const uint8_t payload[] = { 'P','R','O','B','E' };
    int ok = 0, not_acked = 0, err = 0;

    printf("\n  [TX probe %s] node1->node2 x%d (waitAck=true)\n", phase, TX_REPEATS);
    for (int i = 0; i < TX_REPEATS; i++) {
        arc_result_t r = arc_transmit(ctx1, NODE2, payload, (int)sizeof(payload), true);
        printf("    TX #%d: %s\n", i + 1, arc_result_str(r));
        if      (r == ARC_OK)        ok++;
        else if (r == ARC_NOT_ACKED) not_acked++;
        else                         err++;
        Sleep(50);
    }
    printf("  SUMMARY %s: ARC_OK=%d  ARC_NOT_ACKED=%d  errors=%d\n",
           phase, ok, not_acked, err);
    if (not_acked == TX_REPEATS)
        printf("  --> node2 FUNCTIONALLY SILENT (no ACK) [GOOD]\n");
    else if (ok > 0)
        printf("  --> node2 STILL RESPONDING with ACK (ARC_OK x%d) [UNEXPECTED]\n", ok);
}

/* Listen on ctx1 for LISTEN_MS ms, report any packets received from node2. */
static void rx_listen(arc_ctx_t *ctx1, const char *phase)
{
    uint8_t src, dst, data[508];
    int len, total = 0;
    ULONGLONG t0 = GetTickCount64();

    printf("\n  [RX listen %s] %d ms window on node1 (expect nothing from node2)\n",
           phase, LISTEN_MS);
    while ((GetTickCount64() - t0) < (ULONGLONG)LISTEN_MS) {
        arc_result_t r = arc_receive(ctx1, &src, &dst, data, &len);
        if (r == ARC_OK) {
            ULONG ms = (ULONG)(GetTickCount64() - t0);
            printf("    [+%4lu ms] pkt src=0x%02X dst=0x%02X len=%d\n",
                   ms, src, dst, len);
            total++;
        }
    }
    if (total == 0)
        printf("  --> no data received [GOOD]\n");
    else
        printf("  --> %d packet(s) received [UNEXPECTED]\n", total);
}

/* Re-open + re-init ctx2 so the next variant starts from a clean state. */
static arc_ctx_t *reopen_ctx2(const char *path)
{
    printf("\n  [re-init] reopening node2 for next variant...\n");
    arc_ctx_t *ctx = open_init(path, NODE2, "ctx2-reopen");
    if (!ctx) return NULL;
    Sleep(800);  /* let network settle before testing */
    return ctx;
}

/* ── variant runner ───────────────────────────────────────────────────── */

typedef enum { VARIANT_CLOSE, VARIANT_SHUTDOWN } variant_t;

static void run_variant(arc_ctx_t *ctx1, arc_ctx_t *ctx2, variant_t v,
                        const char *label)
{
    printf("\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  VARIANT %s\n", label);
    printf("═══════════════════════════════════════════════════════════════\n");

    /* Pre-close: confirm ACK is working */
    printf("\n  [pre-close] confirming node2 is alive and ACKing...\n");
    {
        static const uint8_t p[] = { 'O','K' };
        for (int i = 0; i < 2; i++) {
            arc_result_t r = arc_transmit(ctx1, NODE2, p, (int)sizeof(p), true);
            printf("    pre-TX #%d: %s\n", i + 1, arc_result_str(r));
            Sleep(100);
        }
    }

    /* Close node2 */
    printf("\n  [close] ");
    if (v == VARIANT_CLOSE) {
        printf("arc_close(ctx2) -- USB released, RST NOT written\n");
        arc_close(ctx2);
    } else {
        printf("arc_shutdown(ctx2) -- RST written then arc_close\n");
        arc_result_t r = arc_shutdown(ctx2);
        printf("  arc_shutdown: %s\n", arc_result_str(r));
    }
    Sleep(100);  /* brief settle */

    /* Post-close probes */
    tx_probe(ctx1, label);
    rx_listen(ctx1, label);
}

/* ── main ─────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    g_lvl = ARC_LOG_INFO;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0) g_lvl = ARC_LOG_DEBUG;
        else { fprintf(stderr, "Unknown arg: %s\n", argv[i]); return 1; }
    }

    char paths[2][256];
    int n = arc_list_devices(paths, 2);
    if (n < 2) {
        printf("[test_closequiet] Need 2 WinUSB devices, found %d -- SKIPPED\n", n);
        return 0;
    }
    printf("[test_closequiet] %d device(s) found\n\n", n);

    /* Initial open + init */
    arc_ctx_t *ctx1 = open_init(paths[0], NODE1, "ctx1");
    arc_ctx_t *ctx2 = open_init(paths[1], NODE2, "ctx2");
    if (!ctx1 || !ctx2) {
        if (ctx1) arc_close(ctx1);
        if (ctx2) arc_close(ctx2);
        return 1;
    }
    Sleep(500);

    /* VARIANT A: arc_close */
    run_variant(ctx1, ctx2, VARIANT_CLOSE, "A: arc_close");

    /* Re-open ctx2 for variant B */
    ctx2 = reopen_ctx2(paths[1]);
    if (!ctx2) { arc_close(ctx1); return 1; }

    /* VARIANT B: arc_shutdown */
    run_variant(ctx1, ctx2, VARIANT_SHUTDOWN, "B: arc_shutdown");

    /* Final cleanup */
    printf("\n[test_closequiet] closing ctx1 (ctx2 already closed by variant B)\n");
    arc_close(ctx1);
    printf("[test_closequiet] done.\n");
    return 0;
}
