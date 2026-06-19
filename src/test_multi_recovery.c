/*
 * test_multi_recovery.c  --  Two-device isolation and recovery test.
 *
 * Goal: prove that arc_ctx_t contexts are fully independent.
 *
 *   - Both node1 and node2 are opened and polled every POLL_MS.
 *   - When ONE device is unplugged, ONLY its context enters reconnect
 *     mode; the OTHER keeps polling at the same rate without any pause.
 *   - When the removed device is re-inserted it auto-recovers via
 *     arc_reopen() + arc_init() and rejoins the poll loop.
 *
 * Reconnect timing: non-blocking — a "next_retry_at" timestamp is checked
 * each loop iteration so the sibling device never waits for a Sleep().
 *
 * Stop with Ctrl+C.
 */

#include "arcnet.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>

#define MAX_DEVS         8
#define POLL_MS          500
#define RECONNECT_MS    2000
#define TAG_LEN         20   /* max chars kept from instance-ID segment */

typedef enum { STATE_ACTIVE, STATE_GONE } slot_state_t;

typedef struct {
    arc_ctx_t    *ctx;
    int           node_id;
    slot_state_t  state;
    ULONGLONG     next_retry_at;    /* GetTickCount64() for next arc_reopen attempt */
    unsigned long gone_count;       /* total device_gone events */
    unsigned long reconnect_count;  /* total successful reconnects */
    char          path_tag[TAG_LEN];/* short suffix of device path for display */
    char          full_path[256];   /* full WinUSB path */
} slot_t;

/* -----------------------------------------------------------------------
 * make_path_tag
 *   Extracts the instance-ID segment between the 2nd and 3rd '#' in the
 *   WinUSB device path (e.g. "5&3a4b5c6d&0&2") and keeps at most
 *   TAG_LEN-1 chars from its end (e.g. "&0&2").
 * --------------------------------------------------------------------- */
static void make_path_tag(const char *path, char *tag)
{
    const char *p = path;
    const char *seg = NULL;
    int hashes = 0;
    size_t seg_len, keep, offset;

    for (; *p; p++) {
        if (*p == '#') {
            hashes++;
            if (hashes == 2) seg = p + 1; /* start of instance ID */
            if (hashes == 3) break;        /* end of instance ID   */
        }
    }
    if (!seg) { strncpy(tag, path + (strlen(path) > TAG_LEN-1 ? strlen(path)-(TAG_LEN-1) : 0), TAG_LEN-1); tag[TAG_LEN-1] = '\0'; return; }

    seg_len = (hashes >= 3) ? (size_t)(p - seg) : strlen(seg);
    keep    = seg_len < (size_t)(TAG_LEN - 1) ? seg_len : (size_t)(TAG_LEN - 1);
    offset  = seg_len - keep;
    memcpy(tag, seg + offset, keep);
    tag[keep] = '\0';
}

/* -----------------------------------------------------------------------
 * poll_slot  --  called every iteration for one device slot
 * --------------------------------------------------------------------- */
static void poll_slot(slot_t *s)
{
    arc_result_t r;
    uint8_t      reg0 = 0;
    ULONGLONG    now  = GetTickCount64();

    /* ---- GONE mode -------------------------------------------------- */
    if (s->state == STATE_GONE) {
        if (now < s->next_retry_at) {
            printf("  [node%d %s] GONE -- next retry in %.1f s\n",
                   s->node_id, s->path_tag,
                   (double)(s->next_retry_at - now) / 1000.0);
            return;
        }

        printf("  [node%d %s] RECONNECT attempt #%lu ...\n",
               s->node_id, s->path_tag, s->gone_count);

        r = arc_reopen(s->ctx);
        printf("  [node%d %s]   arc_reopen : %s\n",
               s->node_id, s->path_tag, arc_result_str(r));
        if (r != ARC_OK) {
            s->next_retry_at = now + RECONNECT_MS;
            return;
        }

        r = arc_init(s->ctx, (uint8_t)s->node_id, 0x18, 0x00, true);
        printf("  [node%d %s]   arc_init   : %s\n",
               s->node_id, s->path_tag, arc_result_str(r));
        if (r != ARC_OK) {
            s->next_retry_at = now + RECONNECT_MS;
            return;
        }

        s->state = STATE_ACTIVE;
        s->reconnect_count++;
        printf("  [node%d %s] *** RECOVERED (reconnect #%lu) ***\n",
               s->node_id, s->path_tag, s->reconnect_count);
        return;
    }

    /* ---- ACTIVE mode ------------------------------------------------- */

    /* (a) Register 0 read */
    r = arc_register(s->ctx, false, 0, &reg0);
    if (r == ARC_ERR_DEVICE_GONE) {
        s->state         = STATE_GONE;
        s->gone_count++;
        s->next_retry_at = now + RECONNECT_MS;
        printf("  [node%d %s] *** DEVICE GONE (event #%lu) -- "
               "entering reconnect mode ***\n",
               s->node_id, s->path_tag, s->gone_count);
        return;
    }
    printf("  [node%d %s] reg0=0x%02X %-14s",
           s->node_id, s->path_tag, reg0, arc_result_str(r));

    /* (b) Transmit (waitAck=false -- don't stall the loop) */
    r = arc_transmit(s->ctx,
                     (uint8_t)(s->node_id == 1 ? 2 : 1),
                     (const uint8_t *)"ALIVE", 5,
                     /*waitAck=*/false);
    if (r == ARC_ERR_DEVICE_GONE) {
        s->state         = STATE_GONE;
        s->gone_count++;
        s->next_retry_at = now + RECONNECT_MS;
        printf("\n  [node%d %s] *** DEVICE GONE (event #%lu) on TX -- "
               "entering reconnect mode ***\n",
               s->node_id, s->path_tag, s->gone_count);
        return;
    }
    if (r == ARC_ERR_NET_BUSY) {
        printf("tx=NET_BUSY (gecici: ARCNET RECON/token yok, siradaki turda yeniden denenecek)\n");
        return;
    }
    printf("tx=%s\n", arc_result_str(r));
}

/* -----------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */
int main(void)
{
    char         paths[MAX_DEVS][256];
    int          n;
    slot_t       slots[2];
    int          i;
    arc_result_t r;
    ULONGLONG    t0, t1;
    unsigned long iter = 0;

    memset(slots, 0, sizeof(slots));
    slots[0].node_id = 1;
    slots[1].node_id = 2;

    printf("============================================================\n");
    printf("  Two-Device Isolation and Recovery Test\n");
    printf("  Poll: %d ms   Reconnect retry: %d ms   Stop: Ctrl+C\n",
           POLL_MS, RECONNECT_MS);
    printf("============================================================\n\n");

    /* ------------------------------------------------------------------ */
    /* Enumerate                                                            */
    /* ------------------------------------------------------------------ */
    n = arc_list_devices(paths, MAX_DEVS);
    printf("[enum] %d device(s) found\n\n", n);

    if (n < 2) {
        fprintf(stderr, "Need 2 devices; only %d found.\n", n);
        return 1;
    }

    /* Store paths and build tags before opening */
    for (i = 0; i < 2; i++) {
        strncpy(slots[i].full_path, paths[i], sizeof(slots[i].full_path) - 1);
        slots[i].full_path[sizeof(slots[i].full_path) - 1] = '\0';
        make_path_tag(paths[i], slots[i].path_tag);
    }

    /* ------------------------------------------------------------------ */
    /* Print device-to-node mapping clearly                                 */
    /* ------------------------------------------------------------------ */
    printf("[DEVICE MAP]\n");
    for (i = 0; i < 2; i++)
        printf("  node%d (nodeID=%d)  tag=%-18s  path=%s\n",
               slots[i].node_id, slots[i].node_id,
               slots[i].path_tag, slots[i].full_path);
    printf("\n");

    /* ------------------------------------------------------------------ */
    /* *** IMPORTANT NOTE ***                                               */
    /* ------------------------------------------------------------------ */
    printf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    printf("  TEST: cihazlardan BIRINI cekin; digerinin kesintisiz\n");
    printf("  calismayi surdurdugunü gozleyin.\n");
    printf("  Cektiginiiz cihaz 'DEVICE GONE' diyecek -- eslemeyi\n");
    printf("  boylece ogreneceksiniz (hangi path hangi fiziksel port).\n");
    printf("  Geri takinca otomatik toparlanma gozleyin.\n");
    printf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n\n");

    /* ------------------------------------------------------------------ */
    /* Open both (verbose=false keeps poll output clean)                    */
    /* ------------------------------------------------------------------ */
    for (i = 0; i < 2; i++) {
        slots[i].ctx = arc_open(slots[i].full_path, /*verbose=*/false);
        if (!slots[i].ctx) {
            fprintf(stderr, "arc_open FAILED for node%d (%s)\n",
                    slots[i].node_id, slots[i].path_tag);
            goto cleanup;
        }
        printf("[open]  node%d (%s): OK\n", slots[i].node_id, slots[i].path_tag);
    }
    printf("\n");

    /* ------------------------------------------------------------------ */
    /* Init both                                                            */
    /* ------------------------------------------------------------------ */
    for (i = 0; i < 2; i++) {
        printf("[init]  node%d (%s) nodeID=%d ...\n",
               slots[i].node_id, slots[i].path_tag, slots[i].node_id);
        t0 = GetTickCount64();
        r  = arc_init(slots[i].ctx, (uint8_t)slots[i].node_id, 0x18, 0x00, true);
        t1 = GetTickCount64();
        printf("        arc_init: %s  (%.1f s)\n",
               arc_result_str(r), (double)(t1 - t0) / 1000.0);
        if (r != ARC_OK) {
            fprintf(stderr, "Init failed for node%d. Exiting.\n", slots[i].node_id);
            goto cleanup;
        }
        slots[i].state = STATE_ACTIVE;
    }
    printf("\n");

    printf("[READY] Both devices active -- entering poll loop.\n\n");

    /* ------------------------------------------------------------------ */
    /* Main poll loop                                                        */
    /* ------------------------------------------------------------------ */
    while (1) {
        iter++;
        printf("[iter=%lu  t=%.1f s]\n",
               iter, (double)GetTickCount64() / 1000.0);

        for (i = 0; i < 2; i++)
            poll_slot(&slots[i]);

        printf("\n");
        Sleep(POLL_MS);
    }

cleanup:
    for (i = 0; i < 2; i++)
        if (slots[i].ctx) arc_close(slots[i].ctx);
    return 1;
}
