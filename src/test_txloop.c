/*
 * test_txloop.c  --  Rapid-transmit ACK regression diagnosis.
 *
 * Observed pattern (GUI): first 1-2 sends return ARC_OK (TMA set), then the
 * rest return ARC_NOT_ACKED, and the channel never recovers.  This test
 * reproduces the same scenario directly against the DLL with full DEBUG
 * logging, so we can see every pipe_flush, every reg0 poll value, and
 * timing — without any GUI interference.
 *
 * Run pass 1 (rapid, no delay) then pass 2 (200 ms between sends).
 *
 * Extra diagnostics beyond what arc_transmit already logs:
 *   - reg0 read BEFORE each transmit (pre-TX state of COM20022)
 *   - reg0 read AFTER each transmit result (post-TX state)
 *   - wall-clock duration of each arc_transmit call
 *
 * Usage: test_txloop.exe [--verbose] [--count N]
 *   --verbose : ARC_LOG_DEBUG (shows pipe_flush, reg0 poll iterations)
 *   --count N : number of sends per pass (default 20)
 *
 * HARDWARE: two USB22-485 adapters on WinUSB.
 * SKIPPED (exit 0) when fewer than 2 devices are found.
 */

#include "arcnet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define NODE1   0x01u
#define NODE2   0x02u
#define PAYLOAD_LEN  14

/* -----------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------- */

static const char *result_tag(arc_result_t r)
{
    switch (r) {
    case ARC_OK:           return "ARC_OK          ";
    case ARC_NOT_ACKED:    return "ARC_NOT_ACKED   ";
    case ARC_NO_PACKET:    return "ARC_NO_PACKET   ";
    case ARC_ERR_IO:       return "ARC_ERR_IO      ";
    case ARC_ERR_NET_BUSY: return "ARC_ERR_NET_BUSY";
    case ARC_ERR_DEVICE_GONE: return "DEVICE_GONE     ";
    default:               return arc_result_str(r);
    }
}

static void read_reg0(arc_ctx_t *ctx, const char *tag)
{
    uint8_t reg0 = 0;
    arc_result_t r = arc_register(ctx, false, 0, &reg0);
    if (r == ARC_OK)
        printf("    [reg0 %s] 0x%02X  TMA=%d  TA=%d  RECON=%d  POR=%d\n",
               tag, reg0,
               (reg0 >> 1) & 1,   /* TMA: Transmit Message Acknowledged */
               (reg0 >> 0) & 1,   /* TA:  Transmitter Active            */
               (reg0 >> 2) & 1,   /* RECON: Reconfiguration             */
               (reg0 >> 7) & 1);  /* POR: Power-On Reset                */
    else
        printf("    [reg0 %s] FAIL (%s)\n", tag, arc_result_str(r));
}

/* -----------------------------------------------------------------------
 * One pass: N transmits with inter-send delay
 * --------------------------------------------------------------------- */
static void run_pass(arc_ctx_t *ctx1, arc_ctx_t *ctx2,
                     const char *label,
                     int count, int delay_ms,
                     const uint8_t *payload)
{
    int ok = 0, not_acked = 0, other = 0;

    printf("\n");
    printf("══════════════════════════════════════════════════════════\n");
    printf("  PASS: %s  (count=%d delay=%dms waitAck=true)\n",
           label, count, delay_ms);
    printf("══════════════════════════════════════════════════════════\n");

    /* Drain any leftover packets on ctx2 from a previous pass. */
    {
        uint8_t s, d, buf[508]; int l;
        ULONGLONG t0 = GetTickCount64();
        while ((GetTickCount64() - t0) < 200) {
            if (arc_receive(ctx2, &s, &d, buf, &l) != ARC_OK) break;
        }
    }

    for (int i = 0; i < count; i++) {
        printf("\n  ── TX #%d ──────────────────────────────────────────\n",
               i + 1);

        read_reg0(ctx1, "pre-TX ");

        ULONGLONG t0 = GetTickCount64();
        arc_result_t r = arc_transmit(ctx1, NODE2, payload, PAYLOAD_LEN, true);
        ULONG elapsed = (ULONG)(GetTickCount64() - t0);

        read_reg0(ctx1, "post-TX");

        printf("  TX #%-2d  %s  %lu ms  %s\n",
               i + 1, result_tag(r), elapsed,
               r == ARC_OK        ? "<-- ACK received" :
               r == ARC_NOT_ACKED ? "<-- TMA never set" : "");

        if      (r == ARC_OK)       ok++;
        else if (r == ARC_NOT_ACKED) not_acked++;
        else                         other++;

        /* After NOT_ACKED: extra reg0 reads to see if TMA shows up late */
        if (r == ARC_NOT_ACKED) {
            printf("    [post NOT_ACKED extra reads]\n");
            for (int k = 0; k < 3; k++) {
                Sleep(20);
                read_reg0(ctx1, "late   ");
            }
        }

        if (delay_ms > 0) Sleep((DWORD)delay_ms);
    }

    printf("\n──────────────────────────────────────────────────────────\n");
    printf("  SUMMARY %s: OK=%d  NOT_ACKED=%d  other=%d / %d\n",
           label, ok, not_acked, other, count);
    printf("──────────────────────────────────────────────────────────\n");
}

/* =======================================================================
 * main
 * ===================================================================== */
int main(int argc, char **argv)
{
    bool verbose = false;
    int  count   = 20;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
            count = atoi(argv[++i]);
            if (count < 1 || count > 200) {
                fprintf(stderr, "count must be 1..200\n");
                return 1;
            }
        } else {
            fprintf(stderr, "Unknown arg: %s\n", argv[i]);
            return 1;
        }
    }

    /* ---- Enumerate -------------------------------------------------- */
    char paths[2][256];
    int  n = arc_list_devices(paths, 2);
    if (n < 2) {
        printf("[test_txloop] Need 2 WinUSB devices, found %d -- SKIPPED\n", n);
        return 0;
    }
    printf("[test_txloop] %d device(s) found\n", n);

    /* ---- Open -------------------------------------------------------- */
    arc_ctx_t *ctx1 = arc_open(paths[0], false);
    arc_ctx_t *ctx2 = arc_open(paths[1], false);
    if (!ctx1 || !ctx2) {
        fprintf(stderr, "[test_txloop] arc_open failed\n");
        if (ctx1) arc_close(ctx1);
        if (ctx2) arc_close(ctx2);
        return 1;
    }

    /* Log level: DEBUG shows pipe_flush + per-poll reg0 inside arc_transmit */
    arc_log_level_t lvl = verbose ? ARC_LOG_DEBUG : ARC_LOG_INFO;
    arc_set_log_level(ctx1, lvl);
    arc_set_log_level(ctx2, lvl);

    /* ---- Init -------------------------------------------------------- */
    arc_result_t r;
    r = arc_init(ctx1, NODE1, 0x18, 0x00, false);
    if (r != ARC_OK) {
        fprintf(stderr, "[test_txloop] ctx1 init: %s\n", arc_result_str(r));
        arc_close(ctx1); arc_close(ctx2); return 1;
    }
    printf("[test_txloop] ctx1 node=0x%02X OK\n", NODE1);

    r = arc_init(ctx2, NODE2, 0x18, 0x00, false);
    if (r != ARC_OK) {
        fprintf(stderr, "[test_txloop] ctx2 init: %s\n", arc_result_str(r));
        arc_close(ctx1); arc_close(ctx2); return 1;
    }
    printf("[test_txloop] ctx2 node=0x%02X OK\n", NODE2);

    /* Let the network settle after init. */
    printf("[test_txloop] waiting 500 ms for network to settle...\n");
    Sleep(500);

    /* Initial reg0 snapshot on ctx1 */
    printf("\n[test_txloop] Initial reg0 on ctx1 (before any TX):\n");
    arc_set_log_level(ctx1, ARC_LOG_INFO);   /* suppress debug for this read */
    read_reg0(ctx1, "initial");
    arc_set_log_level(ctx1, lvl);

    /* ---- Fixed payload ----------------------------------------------- */
    uint8_t payload[PAYLOAD_LEN];
    for (int i = 0; i < PAYLOAD_LEN; i++)
        payload[i] = (uint8_t)(0xA0 + i);

    printf("\n[test_txloop] Payload (%d bytes): ", PAYLOAD_LEN);
    for (int i = 0; i < PAYLOAD_LEN; i++) printf("%02X ", payload[i]);
    printf("\n");

    /* ---- Pass 1: rapid (no delay) ------------------------------------ */
    run_pass(ctx1, ctx2, "RAPID (0 ms)", count, 0, payload);

    /* Pause between passes to let the network recover. */
    printf("\n[test_txloop] Pausing 2 s between passes...\n");
    Sleep(2000);
    printf("[test_txloop] reg0 after pause:\n");
    arc_set_log_level(ctx1, ARC_LOG_INFO);
    read_reg0(ctx1, "pause  ");
    arc_set_log_level(ctx1, lvl);

    /* ---- Pass 2: slow (200 ms delay) --------------------------------- */
    run_pass(ctx1, ctx2, "SLOW (200 ms)", count, 200, payload);

    /* ---- Close ------------------------------------------------------- */
    arc_close(ctx1);
    arc_close(ctx2);
    printf("\n[test_txloop] done.\n");
    return 0;
}
