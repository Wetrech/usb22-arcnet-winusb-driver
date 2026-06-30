/*
 * test_chipreset.c  --  arc_shutdown() integration test.
 *
 * Verifies that arc_shutdown() drops ctx1 off the ARCNET bus (light goes
 * off), that ctx2 sees a RECON event, and that ctx1 can be re-opened and
 * re-initialised afterwards.
 *
 * Sequence:
 *   1. Open + init both devices (node1, node2). Both lights on.
 *   2. Baseline reg0 reads.
 *   3. 3 s pause — user confirms both lights are on.
 *   4. arc_shutdown(ctx1)  — RST + close.  ctx1 pointer is now INVALID.
 *   5. Drain EP0x81 on ctx2 for 2 s — expect RECON event (b4=0x04?).
 *   6. 3 s pause — user confirms ctx1 light is OFF, ctx2 still on.
 *   7. Recovery: arc_open + arc_init ctx1 (fresh open, not arc_reopen).
 *   8. 3 s pause — user confirms ctx1 light is back ON.
 *   9. Baseline reg0 after recovery.
 *  10. arc_close both (normal close — light stays on).
 *
 * Usage: test_chipreset.exe [--verbose]
 * HARDWARE: two USB22-485 adapters on WinUSB.
 * SKIPPED (exit 0) when fewer than 2 devices are found.
 */

#include "arcnet.h"
#include "test_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define NODE1  0x01u
#define NODE2  0x02u

static void read_reg0(arc_ctx_t *ctx, const char *tag)
{
    uint8_t v = 0;
    arc_result_t r = arc_register(ctx, false, 0, &v);
    if (r == ARC_OK)
        printf("  reg0 %-18s: 0x%02X  RST=%d TA=%d TMA=%d\n",
               tag, v, (v >> 7) & 1, (v >> 0) & 1, (v >> 1) & 1);
    else
        printf("  reg0 %-18s: FAIL (%s)\n", tag, arc_result_str(r));
}

static void drain_events(arc_ctx_t *ctx, const char *label, ULONG drain_ms)
{
    uint8_t  buf[64];
    int      n = 0, count = 0;
    ULONGLONG t0 = GetTickCount64();

    printf("  [EP0x81 drain %s %lu ms]\n", label, drain_ms);
    while ((GetTickCount64() - t0) < (ULONGLONG)drain_ms) {
        if (arc_read_event(ctx, buf, (int)sizeof(buf), 30, &n) == ARC_OK && n > 0) {
            ULONG ms = (ULONG)(GetTickCount64() - t0);
            printf("    [+%4lu ms] %d B:", ms, n);
            for (int i = 0; i < n && i < 12; i++) printf(" %02X", buf[i]);
            printf("\n");
            if (n >= 6 && buf[0] == 0x20)
                printf("             opcode=0x20  b4=0x%02X  (%s)\n",
                       buf[4],
                       buf[4] == 0x04 ? "RECON" :
                       buf[4] == 0x03 ? "TX-OK" :
                       buf[4] == 0x01 ? "no-ACK" : "?");
            count++;
        }
    }
    if (count == 0) printf("    (no events)\n");
}

static void pause_observe(const char *msg, DWORD ms)
{
    printf("\n  *** %s ***\n  (waiting %lu ms)\n\n", msg, (ULONG)ms);
    Sleep(ms);
}

int main(int argc, char **argv)
{
    bool verbose = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0) verbose = true;
        else { fprintf(stderr, "Unknown arg: %s\n", argv[i]); return 1; }
    }

    char paths[2][256];
    int n = arc_list_devices(paths, 2);
    if (n < 2) {
        printf("[test_chipreset] Need 2 WinUSB devices, found %d -- SKIPPED\n", n);
        return 0;
    }
    printf("[test_chipreset] %d device(s) found\n", n);

    /* ── Step 1: open + init ───────────────────────────────────────────── */
    arc_ctx_t *ctx1 = arc_open(paths[0], false);
    arc_ctx_t *ctx2 = arc_open(paths[1], false);
    if (!ctx1 || !ctx2) {
        fprintf(stderr, "[test_chipreset] arc_open failed\n");
        if (ctx1) arc_close(ctx1);
        if (ctx2) arc_close(ctx2);
        return 1;
    }
    arc_log_level_t lvl = verbose ? ARC_LOG_DEBUG : ARC_LOG_INFO;
    arc_set_log_level(ctx1, lvl);
    arc_set_log_level(ctx2, lvl);

    arc_result_t r;
    r = arc_init(ctx1, NODE1, 0x18, TEST_CLOCK_PRESCALER, false);
    if (r != ARC_OK) {
        fprintf(stderr, "[test_chipreset] ctx1 init: %s\n", arc_result_str(r));
        arc_close(ctx1); arc_close(ctx2); return 1;
    }
    printf("[test_chipreset] ctx1 node=0x%02X OK\n", NODE1);

    r = arc_init(ctx2, NODE2, 0x18, TEST_CLOCK_PRESCALER, false);
    if (r != ARC_OK) {
        fprintf(stderr, "[test_chipreset] ctx2 init: %s\n", arc_result_str(r));
        arc_close(ctx1); arc_close(ctx2); return 1;
    }
    printf("[test_chipreset] ctx2 node=0x%02X OK\n", NODE2);
    Sleep(500);

    /* ── Step 2: baseline ──────────────────────────────────────────────── */
    printf("\n── BASELINE ────────────────────────────────────────────────\n");
    read_reg0(ctx1, "ctx1 before shutdown");
    read_reg0(ctx2, "ctx2 before shutdown");

    /* ── Step 3: confirm both lights ───────────────────────────────────── */
    pause_observe("BOTH LIGHTS ON? (3 s)", 3000);

    /* ── Step 4: arc_shutdown(ctx1) ────────────────────────────────────── */
    printf("── arc_shutdown(ctx1) ──────────────────────────────────────\n");
    printf("  (writes RST to reg0, waits 50 ms, then closes — ctx1 freed)\n");
    r = arc_shutdown(ctx1);
    ctx1 = NULL;  /* freed by arc_shutdown; must not dereference */
    printf("  arc_shutdown: %s\n", arc_result_str(r));

    /* ── Step 5: ctx2 event drain ──────────────────────────────────────── */
    printf("\n── ctx2 EP0x81 events after shutdown ───────────────────────\n");
    drain_events(ctx2, "ctx2", 2000);
    read_reg0(ctx2, "ctx2 post-shutdown");

    /* ── Step 6: confirm ctx1 light off ────────────────────────────────── */
    pause_observe("ctx1 LIGHT OFF? ctx2 still on? (3 s)", 3000);

    /* ── Step 7: recovery ──────────────────────────────────────────────── */
    printf("── RECOVERY: fresh arc_open + arc_init for ctx1 ───────────\n");
    ctx1 = arc_open(paths[0], false);
    if (!ctx1) {
        fprintf(stderr, "  arc_open failed — device may need USB re-plug\n");
        arc_close(ctx2);
        return 1;
    }
    arc_set_log_level(ctx1, lvl);
    r = arc_init(ctx1, NODE1, 0x18, TEST_CLOCK_PRESCALER, false);
    printf("  arc_open:  OK\n");
    printf("  arc_init:  %s\n", arc_result_str(r));

    if (r == ARC_OK) {
        /* ── Step 8: confirm ctx1 light back ──────────────────────────── */
        pause_observe("ctx1 LIGHT BACK ON? (3 s)", 3000);

        /* ── Step 9: final reg0 ─────────────────────────────────────────*/
        printf("── FINAL reg0 after recovery ───────────────────────────────\n");
        read_reg0(ctx1, "ctx1 recovered");
        read_reg0(ctx2, "ctx2 final");
    }

    /* ── Step 10: normal close (lights stay on) ─────────────────────────*/
    arc_close(ctx1);
    arc_close(ctx2);
    printf("\n[test_chipreset] done.\n");
    return 0;
}
