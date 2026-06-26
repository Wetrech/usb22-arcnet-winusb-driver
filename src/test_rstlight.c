/*
 * test_rstlight.c  --  Isolated RST light-effect test.
 *
 * Goal: determine whether COM20022 reg0 bit7 (RST=0x80) permanently drops
 * the node off the ARCNET bus (light stays off) or only momentarily.
 *
 * Protocol:
 *   1. Open + init ONE device (node1 only). Second adapter may be plugged
 *      in but is NOT opened or initialised.
 *   2. 3 s pause  -- user observes BASELINE light state.
 *   3. Write reg0=0x80 (RST).
 *   4. 10 s DEAD SILENCE -- absolutely no USB operations. User watches light.
 *      Q: does it go off and STAY off, or briefly flicker and return?
 *   5. Read reg0 -- what does the chip report after 10 s?
 *   6. arc_close (normal).
 *
 * Usage: test_rstlight.exe
 * HARDWARE: one USB22-485 adapter open; a second may be on the bus.
 */

#include "arcnet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define NODE1  0x01u

int main(void)
{
    char paths[2][256];
    int n = arc_list_devices(paths, 2);
    if (n < 1) {
        printf("[test_rstlight] No WinUSB devices found -- ABORT\n");
        return 1;
    }
    printf("[test_rstlight] %d device(s) found, opening paths[0]\n\n", n);

    arc_ctx_t *ctx = arc_open(paths[0], false);
    if (!ctx) {
        fprintf(stderr, "[test_rstlight] arc_open failed\n");
        return 1;
    }
    arc_set_log_level(ctx, ARC_LOG_INFO);

    arc_result_t r = arc_init(ctx, NODE1, 0x18, 0x00, false);
    if (r != ARC_OK) {
        fprintf(stderr, "[test_rstlight] arc_init: %s\n", arc_result_str(r));
        arc_close(ctx);
        return 1;
    }
    printf("[test_rstlight] node=0x%02X init OK\n\n", NODE1);

    /* ── Step 2: baseline ───────────────────────────────────────────────── */
    printf("*** BASELINE -- izle: ışık yanık mı? (3 sn) ***\n\n");
    Sleep(3000);

    /* ── Step 3: RST ────────────────────────────────────────────────────── */
    uint8_t rst = 0x80u;
    r = arc_register(ctx, true, 0, &rst);
    printf("[test_rstlight] reg0=0x80 (RST) yazıldı: %s\n", arc_result_str(r));
    printf("\n");

    /* ── Step 4: 10 s dead silence ──────────────────────────────────────── */
    printf("*** RST SONRASI -- 10 sn HIÇBIR USB işlemi yok ***\n");
    printf("    Işığı izle: kalıcı söndü mü? Kısa sönüp geri mi geldi? Hiç sönmedi mi?\n\n");

    for (int i = 10; i >= 1; i--) {
        printf("  bekleniyor... %2d sn\n", i);
        fflush(stdout);
        Sleep(1000);
    }
    printf("\n");

    /* ── Step 5: reg0 okuma ──────────────────────────────────────────────── */
    uint8_t v = 0;
    r = arc_register(ctx, false, 0, &v);
    if (r == ARC_OK)
        printf("[test_rstlight] reg0 (10 sn sonra): 0x%02X  RST=%d TA=%d TMA=%d\n",
               v, (v >> 7) & 1, (v >> 0) & 1, (v >> 1) & 1);
    else
        printf("[test_rstlight] reg0 okuma: %s\n", arc_result_str(r));

    /* ── Step 6: close ──────────────────────────────────────────────────── */
    arc_close(ctx);
    printf("\n[test_rstlight] done.\n");
    return 0;
}
