/*
 * test_b4survey.c  --  Raw EP0x81 b4 field survey after TX (3 scenarios).
 *
 * Goal: determine the exact bit structure of b4 in the 0x20 TX-complete
 * event so arc_transmit can distinguish ACK from no-ACK in any network.
 *
 * Scenarios:
 *   A) ALIVE          : node1 -> node2 (node2 open+init, chip on bus)
 *   B) NO-RECEIVER    : node1 -> node5 (non-existent node -- nobody home)
 *   C) CHIP-ALIVE     : node1 -> node2 (node2 arc_close'd, chip still on bus,
 *                       hardware ACK reflex observed in test_closequiet)
 *
 * Method: TX with waitAck=false, then drain EP0x81 for DRAIN_MS and log
 * EVERY 0x20 event -- not just the first.  Cross-scenario b4 frequency
 * table printed at the end.
 *
 * Usage: test_b4survey.exe [--verbose]
 * HARDWARE: two USB22-485 adapters on WinUSB; node5 must NOT exist on bus.
 */

#include "arcnet.h"
#include "test_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define NODE1           0x01u
#define NODE2           0x02u
#define NODE_DEAD       0x05u   /* must not exist on the bus */
#define TX_COUNT        20
#define DRAIN_MS        300
#define TX_GAP_MS       50

static arc_log_level_t g_lvl;

/* ── b4 frequency table ───────────────────────────────────────────────── */
#define B4_MAX 256
typedef struct {
    int total_events;
    int count[B4_MAX];
    int tx_count;
} b4_table_t;

static void b4_record(b4_table_t *t, uint8_t b4) { t->total_events++; t->count[b4]++; }

static void b4_print(const b4_table_t *t, const char *label)
{
    printf("\n  b4 table [%s]  TX=%d  total-0x20-events=%d\n",
           label, t->tx_count, t->total_events);
    printf("  %-6s %-7s  [bit2] [bit1] [bit0]\n", "b4", "count");
    for (int i = 0; i < B4_MAX; i++) {
        if (!t->count[i]) continue;
        printf("  0x%02X   %-7d  %d      %d      %d\n",
               i, t->count[i], (i>>2)&1, (i>>1)&1, (i>>0)&1);
    }
    if (!t->total_events) printf("  (no events)\n");
}

/* ── drain EP0x81 for one TX ─────────────────────────────────────────── */
static void drain_after_tx(arc_ctx_t *ctx, b4_table_t *tbl, int tx_idx)
{
    uint8_t buf[64];
    int n;
    ULONGLONG t0 = GetTickCount64();
    int got_tx_event = 0;

    while ((GetTickCount64() - t0) < (ULONGLONG)DRAIN_MS) {
        if (arc_read_event(ctx, buf, (int)sizeof(buf), 30, &n) != ARC_OK || n < 1)
            continue;

        if (n >= 6 && buf[0] == 0x20) {
            uint8_t b4 = buf[4];
            b4_record(tbl, b4);
            printf("    TX#%-2d [+%3llu ms]  b4=0x%02X  bit[2:0]=%d%d%d  %s\n",
                   tx_idx,
                   (unsigned long long)(GetTickCount64() - t0),
                   b4,
                   (b4>>2)&1, (b4>>1)&1, (b4>>0)&1,
                   !(b4 & 0x01) ? "RECON/other (bit0=0)" :
                    (b4 & 0x02) ? "TX+ACK  (bit0=1 bit1=1)" :
                                  "TX+noACK(bit0=1 bit1=0)");
            if (b4 & 0x01) got_tx_event = 1;
        } else if (n >= 1 && buf[0] != 0x20) {
            printf("    TX#%-2d [+%3llu ms]  opcode=0x%02X (%d B, skip)\n",
                   tx_idx,
                   (unsigned long long)(GetTickCount64() - t0),
                   buf[0], n);
        }
    }
    if (!got_tx_event)
        printf("    TX#%-2d  no TX-complete event in %d ms\n", tx_idx, DRAIN_MS);
}

/* ── scenario runner ─────────────────────────────────────────────────── */
static void run_scenario(arc_ctx_t *ctx1, uint8_t dest, const char *label,
                         b4_table_t *tbl)
{
    static const uint8_t payload[] = { 'B','4','S','U','R','V','E','Y' };

    printf("\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  SCENARIO %s  (dest=0x%02X)\n", label, dest);
    printf("═══════════════════════════════════════════════════════════════\n");

    tbl->tx_count = TX_COUNT;
    memset(tbl->count, 0, sizeof(tbl->count));
    tbl->total_events = 0;

    for (int i = 1; i <= TX_COUNT; i++) {
        arc_result_t r = arc_transmit(ctx1, dest, payload,
                                      (int)sizeof(payload), false);
        if (r != ARC_OK)
            printf("    TX#%-2d  arc_transmit: %s\n", i, arc_result_str(r));
        else
            drain_after_tx(ctx1, tbl, i);
        Sleep(TX_GAP_MS);
    }

    b4_print(tbl, label);
}

/* ── helpers ──────────────────────────────────────────────────────────── */
static arc_ctx_t *open_init(const char *path, uint8_t node, const char *tag)
{
    arc_ctx_t *ctx = arc_open(path, false);
    if (!ctx) { fprintf(stderr, "[%s] arc_open failed\n", tag); return NULL; }
    arc_set_log_level(ctx, g_lvl);
    arc_result_t r = arc_init(ctx, node, 0x18, TEST_CLOCK_PRESCALER, false);
    if (r != ARC_OK) {
        fprintf(stderr, "[%s] arc_init: %s\n", tag, arc_result_str(r));
        arc_close(ctx); return NULL;
    }
    printf("  [%s] node=0x%02X init OK\n", tag, node);
    return ctx;
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
        printf("[test_b4survey] Need 2 WinUSB devices, found %d -- SKIPPED\n", n);
        return 0;
    }
    printf("[test_b4survey] %d device(s)  TX_COUNT=%d  DRAIN_MS=%d\n\n",
           n, TX_COUNT, DRAIN_MS);
    printf("  NODE_DEAD=0x%02X must NOT be on the bus!\n", NODE_DEAD);

    b4_table_t tbl_a, tbl_b, tbl_c;

    /* ── SCENARIO A: ALIVE receiver ──────────────────────────────────── */
    {
        arc_ctx_t *ctx1 = open_init(paths[0], NODE1, "A-ctx1");
        arc_ctx_t *ctx2 = open_init(paths[1], NODE2, "A-ctx2");
        if (!ctx1 || !ctx2) { if (ctx1) arc_close(ctx1); if (ctx2) arc_close(ctx2); return 1; }
        Sleep(500);
        run_scenario(ctx1, NODE2, "A: ALIVE (node2 open+init)", &tbl_a);
        arc_close(ctx1);
        arc_close(ctx2);
    }

    Sleep(800);

    /* ── SCENARIO B: NO-RECEIVER (non-existent node) ─────────────────── */
    {
        arc_ctx_t *ctx1 = open_init(paths[0], NODE1, "B-ctx1");
        if (!ctx1) return 1;
        /* only ctx1 open -- node5 does not exist */
        Sleep(500);
        run_scenario(ctx1, NODE_DEAD, "B: NO-RECEIVER (node5 non-existent)", &tbl_b);
        arc_close(ctx1);
    }

    Sleep(800);

    /* ── SCENARIO C: CHIP-ALIVE (node2 arc_close'd, chip on bus) ────── */
    {
        arc_ctx_t *ctx1 = open_init(paths[0], NODE1, "C-ctx1");
        arc_ctx_t *ctx2 = open_init(paths[1], NODE2, "C-ctx2");
        if (!ctx1 || !ctx2) { if (ctx1) arc_close(ctx1); if (ctx2) arc_close(ctx2); return 1; }
        Sleep(500);
        printf("\n  [C] arc_close(ctx2) -- chip stays on bus\n");
        arc_close(ctx2);
        Sleep(200);
        run_scenario(ctx1, NODE2, "C: CHIP-ALIVE (node2 arc_close'd)", &tbl_c);
        arc_close(ctx1);
    }

    /* ── CROSS-SCENARIO SUMMARY ─────────────────────────────────────── */
    printf("\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  CROSS-SCENARIO b4 SUMMARY\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  %-6s  %-9s  %-11s  %-11s  verdict\n",
           "b4", "A(ALIVE)", "B(NO-RECV)", "C(CHIP-ALIVE)");

    for (int i = 0; i < B4_MAX; i++) {
        int a = tbl_a.count[i], b = tbl_b.count[i], c = tbl_c.count[i];
        if (!a && !b && !c) continue;
        const char *v = "?";
        if (a > 0 && !b && !c) v = "ALIVE only";
        else if (!a && b > 0 && !c) v = "NO-RECV only";
        else if (!a && !b && c > 0) v = "CHIP-ALIVE only";
        else if (a > 0 && !b && c > 0) v = "ALIVE+CHIP (HW ACK reflex)";
        else if (!a && b > 0 && c > 0) v = "NO-RECV+CHIP";
        else if (a > 0 && b > 0) v = "ALIVE+NO-RECV (non-ACK status bit)";
        printf("  0x%02X   %-9d  %-11d  %-11d  %s\n", i, a, b, c, v);
    }

    printf("\n  BIT HYPOTHESIS (bit0=TX-complete, bit1=ACK):\n");
    int a_b0=0, b_b0=0, c_b0=0;
    int a_b1=0, b_b1=0, c_b1=0;
    for (int i = 0; i < B4_MAX; i++) {
        if (i & 0x01) { a_b0 += tbl_a.count[i]; b_b0 += tbl_b.count[i]; c_b0 += tbl_c.count[i]; }
        if (i & 0x02) { a_b1 += tbl_a.count[i]; b_b1 += tbl_b.count[i]; c_b1 += tbl_c.count[i]; }
    }
    printf("    bit0 set: A=%d/%d  B=%d/%d  C=%d/%d\n",
           a_b0, TX_COUNT, b_b0, TX_COUNT, c_b0, TX_COUNT);
    printf("    bit1 set: A=%d/%d  B=%d/%d  C=%d/%d\n",
           a_b1, TX_COUNT, b_b1, TX_COUNT, c_b1, TX_COUNT);
    printf("\n  Expect if hypothesis correct:\n");
    printf("    bit0: A=20 B=20 C=20  (every TX generates a TX-complete)\n");
    printf("    bit1: A=20 B=0  C=20  (ACK bit: ALIVE and CHIP-ALIVE both ACK)\n");

    printf("\n[test_b4survey] done.\n");
    return 0;
}
