/*
 * test_bigframe.c  --  Long-frame boundary test (UPDATE 9).
 *
 * Protocol:
 *   TX short (len<=253): EP0x02 OUT  [dst][256-L][data]        (header 2 B)
 *   TX long  (len>=254): EP0x02 OUT  [dst][0x00][(512-L)&0xFF][data]  (header 3 B)
 *   RX short (len<=253): EP0x86 IN   [src][dst][256-L][data]   (header 3 B)
 *   RX long  (len>=254): EP0x86 IN   [src][dst][0x00][512-L][data]    (header 4 B)
 *
 * Test sizes:
 *   253  max short frame
 *   254  DEAD ZONE: (512-254)&0xFF = 0x02  -> RX decodes L=510 > 508 -> dropped
 *   255  DEAD ZONE: (512-255)&0xFF = 0x01  -> RX decodes L=511 > 508 -> dropped
 *   257  first clean long frame: 512-257 = 255 = 0xFF, fits in byte
 *   300  long frame: 512-300 = 212 = 0xD4
 *   400  long frame: 512-400 = 112 = 0x70
 *   508  max long frame: 512-508 = 4 = 0x04
 *
 * HARDWARE: two USB22-485 adapters on WinUSB.
 * SKIPPED (exit 0) when no WinUSB device is found.
 *
 * Usage: test_bigframe.exe [--verbose]
 *   --verbose : ARC_LOG_DEBUG on both contexts (shows internal header bytes)
 */

#include "arcnet.h"
#include "test_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define NODE1          0x01u
#define NODE2          0x02u
#define RX_TIMEOUT_MS  3000u

/* -----------------------------------------------------------------------
 * Test-case table
 * dead_zone=true: TX succeeds but our RX decoder cannot round-trip;
 * these cases do NOT count against the pass/fail verdict.
 * --------------------------------------------------------------------- */
static const struct {
    int  len;
    bool dead_zone;
    const char *note;
} CASES[] = {
    { 253, false, "max short frame" },
    { 254, true,  "DEAD ZONE: (512-254)&0xFF=0x02 -> RX decodes L=510>508 -> drop" },
    { 255, true,  "DEAD ZONE: (512-255)&0xFF=0x01 -> RX decodes L=511>508 -> drop" },
    { 257, false, "first clean long: 512-257=255=0xFF" },
    { 300, false, "long: 512-300=212=0xD4" },
    { 400, false, "long: 512-400=112=0x70" },
    { 508, false, "max long: 512-508=4=0x04" },
};
#define N_CASES  (int)(sizeof(CASES)/sizeof(CASES[0]))

/* -----------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------- */
static void fill_payload(uint8_t *buf, int len, uint8_t seed)
{
    int i;
    for (i = 0; i < len; i++)
        buf[i] = (uint8_t)((i + seed) & 0xFF);
}

/* Print the TX header bytes the library will compute for this len/dest. */
static void print_tx_header(int len, uint8_t dest)
{
    if (len <= 253) {
        uint8_t count = (uint8_t)((256 - len) & 0xFF);
        printf("  TX header : %02X %02X"
               "           [dst=0x%02X, count=256-%d=0x%02X, short]\n",
               dest, count, dest, len, count);
    } else {
        int    raw   = 512 - len;        /* may exceed 255 for len=254..256 */
        uint8_t nb   = (uint8_t)(raw & 0xFF);
        bool   ovf   = (raw > 255);
        printf("  TX header : %02X 00 %02X"
               "        [dst=0x%02X, 0x00=long, (512-%d)%s=0x%02X%s]\n",
               dest, nb, dest, len,
               ovf ? "&0xFF" : "    ", nb,
               ovf ? " OVERFLOW!" : "");
    }
}

/* Poll arc_receive until a packet arrives or timeout expires. */
static arc_result_t recv_blocking(arc_ctx_t *ctx,
                                   uint8_t *src, uint8_t *dst,
                                   uint8_t *data, int *len,
                                   DWORD timeout_ms)
{
    ULONGLONG t0 = GetTickCount64();
    arc_result_t r;
    while ((GetTickCount64() - t0) < (ULONGLONG)timeout_ms) {
        r = arc_receive(ctx, src, dst, data, len);
        if (r == ARC_OK)         return ARC_OK;
        if (r != ARC_NO_PACKET)  return r;
        Sleep(10);
    }
    return ARC_NO_PACKET;
}

/* Drain any leftover packets on ctx (brief window). */
static void drain(arc_ctx_t *ctx)
{
    uint8_t s, d, buf[508]; int l;
    ULONGLONG t0 = GetTickCount64();
    while ((GetTickCount64() - t0) < 300) {
        if (arc_receive(ctx, &s, &d, buf, &l) != ARC_OK) break;
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
        if (strcmp(argv[i], "--verbose") == 0) verbose = true;
        else { fprintf(stderr, "Unknown arg: %s\n", argv[i]); return 1; }
    }

    /* ---- Enumerate -------------------------------------------------- */
    char paths[2][256];
    int  n = arc_list_devices(paths, 2);
    if (n < 2) {
        printf("[test_bigframe] Need 2 WinUSB devices, found %d -- SKIPPED\n", n);
        printf("(Rebind adapters from CC driver to WinUSB first.)\n");
        return 0;
    }
    printf("[test_bigframe] %d device(s) found\n", n);

    /* ---- Open + init ------------------------------------------------ */
    arc_ctx_t *ctx1 = arc_open(paths[0], false);
    arc_ctx_t *ctx2 = arc_open(paths[1], false);
    if (!ctx1 || !ctx2) {
        fprintf(stderr, "[test_bigframe] arc_open failed\n");
        if (ctx1) arc_close(ctx1);
        if (ctx2) arc_close(ctx2);
        return 1;
    }
    if (verbose) {
        arc_set_log_level(ctx1, ARC_LOG_DEBUG);
        arc_set_log_level(ctx2, ARC_LOG_DEBUG);
    }

    arc_result_t r;
    r = arc_init(ctx1, NODE1, 0x18, TEST_CLOCK_PRESCALER, false);
    if (r != ARC_OK) {
        fprintf(stderr, "[test_bigframe] ctx1 init: %s\n", arc_result_str(r));
        arc_close(ctx1); arc_close(ctx2); return 1;
    }
    printf("[test_bigframe] ctx1 node=0x%02X OK\n", NODE1);

    r = arc_init(ctx2, NODE2, 0x18, TEST_CLOCK_PRESCALER, false);
    if (r != ARC_OK) {
        fprintf(stderr, "[test_bigframe] ctx2 init: %s\n", arc_result_str(r));
        arc_close(ctx1); arc_close(ctx2); return 1;
    }
    printf("[test_bigframe] ctx2 node=0x%02X OK\n", NODE2);

    /* ---- Run cases -------------------------------------------------- */
    int must_pass = 0, must_total = 0;   /* non-dead-zone cases */
    int dead_expected = 0, dead_surprise = 0; /* dead-zone outcomes */

    uint8_t tx_buf[508];
    uint8_t rx_data[508];
    uint8_t rx_src, rx_dst;
    int     rx_len;

    for (i = 0; i < N_CASES; i++) {
        int  len  = CASES[i].len;
        bool dead = CASES[i].dead_zone;

        printf("\n═══ case %d / %d  len=%-3d  %s%s ═══\n",
               i + 1, N_CASES, len,
               dead ? "[DEAD ZONE] " : "",
               CASES[i].note);

        fill_payload(tx_buf, len, (uint8_t)(i * 53 + 7));
        print_tx_header(len, NODE2);

        /* ---- TX ---------------------------------------------------- */
        r = arc_transmit(ctx1, NODE2, tx_buf, len, false);
        printf("  TX result : %s\n", arc_result_str(r));

        if (r != ARC_OK && r != ARC_NOT_ACKED) {
            printf("  OUTCOME   : FAIL (TX error)\n");
            if (!dead) { must_total++; }
            Sleep(200);
            continue;
        }

        /* ---- RX ---------------------------------------------------- */
        Sleep(30);
        r = recv_blocking(ctx2, &rx_src, &rx_dst, rx_data, &rx_len, RX_TIMEOUT_MS);

        if (r != ARC_OK) {
            if (dead) {
                printf("  RX result : %s  (expected — dead zone)\n", arc_result_str(r));
                printf("  OUTCOME   : EXPECTED FAIL\n");
                dead_expected++;
            } else {
                printf("  RX result : %s\n", arc_result_str(r));
                printf("  OUTCOME   : FAIL (RX timeout/error)\n");
                must_total++;
            }
            drain(ctx2);
            continue;
        }

        /* Decode what the RX header must have looked like */
        printf("  RX decoded: src=0x%02X dst=0x%02X len=%d  (%s frame)\n",
               rx_src, rx_dst, rx_len,
               rx_len <= 253 ? "short" : "long");

        if (rx_len > 253) {
            uint8_t nb = (uint8_t)((512 - rx_len) & 0xFF);
            printf("  RX header : src 00 %02X ... [0x00=long, 512-%d=0x%02X]\n",
                   nb, rx_len, nb);
        } else {
            uint8_t cnt = (uint8_t)((256 - rx_len) & 0xFF);
            printf("  RX header : src %02X ... [count=256-%d=0x%02X, short]\n",
                   cnt, rx_len, cnt);
        }

        /* Payload verification */
        bool size_ok    = (rx_len == len);
        bool routing_ok = (rx_src == NODE1) && (rx_dst == NODE2);
        int  mismatch   = -1;
        if (size_ok) {
            int j;
            for (j = 0; j < len; j++) {
                if (rx_data[j] != tx_buf[j]) { mismatch = j; break; }
            }
        }

        if (!routing_ok) {
            printf("  payload   : ROUTING ERROR src=0x%02X dst=0x%02X\n",
                   rx_src, rx_dst);
        } else if (!size_ok) {
            printf("  payload   : SIZE MISMATCH got=%d want=%d\n", rx_len, len);
        } else if (mismatch >= 0) {
            printf("  payload   : CONTENT MISMATCH at byte[%d]: got=0x%02X want=0x%02X\n",
                   mismatch, rx_data[mismatch], tx_buf[mismatch]);
        } else {
            printf("  payload   : OK  (%d bytes, exact match)\n", len);
        }

        bool ok = routing_ok && size_ok && (mismatch < 0);

        if (dead) {
            if (ok) {
                printf("  OUTCOME   : SURPRISE PASS (dead zone worked!)\n");
                dead_surprise++;
            } else {
                printf("  OUTCOME   : EXPECTED FAIL (dead zone, partial decode)\n");
                dead_expected++;
            }
        } else {
            printf("  OUTCOME   : %s\n", ok ? "PASS" : "FAIL");
            must_total++;
            if (ok) must_pass++;
        }

        drain(ctx2);
    }

    /* ---- Close + summary -------------------------------------------- */
    arc_close(ctx1);
    arc_close(ctx2);

    printf("\n══════════════════════════════════════════\n");
    printf("  Required cases : %d / %d passed\n",     must_pass,    must_total);
    printf("  Dead-zone cases: %d expected-fail, %d surprise-pass\n",
           dead_expected, dead_surprise);
    printf("══════════════════════════════════════════\n");
    printf("%s\n", (must_pass == must_total) ? "PASS" : "FAIL");
    return (must_pass == must_total) ? 0 : 1;
}
