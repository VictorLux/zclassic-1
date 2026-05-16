/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#define _GNU_SOURCE  /* pthread_timedjoin_np */

#include "services/gap_fill_service.h"

#include "validation/main_state.h"
#include "validation/chainstate.h"
#include "chain/chain.h"
#include "net/download.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "util/thread_registry.h"
#include "event/event.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>

struct gap_fill_state {
    pthread_t              thread;
    pthread_mutex_t        mu;
    pthread_cond_t         cv;
    _Atomic bool           running;
    _Atomic bool           stop_requested;
    bool                   thread_started;

    struct main_state      *ms;
    struct download_manager *dm;

    struct gap_fill_stats  stats;
};

static struct gap_fill_state g_gf = {
    .mu = PTHREAD_MUTEX_INITIALIZER,
    .cv = PTHREAD_COND_INITIALIZER,
};

/* Walk pprev from `start` collecting block_index pointers until we
 * reach a node whose height == stop_height_exclusive (i.e. stop_h+1
 * is the lowest included height). Returns count collected. Returns
 * -1 if a corrupt walk is detected (non-monotonic height or step
 * cap hit) — caller logs and skips this pass. */
static int collect_pprev_window(struct block_index *start,
                                int stop_height_exclusive,
                                struct block_index **out,
                                int out_cap)
{
    if (!start || out_cap <= 0) return 0;
    int count = 0;
    struct block_index *cur = start;
    int last_h = cur->nHeight + 1; /* sentinel: any valid prev_h < this */
    int steps = 0;
    while (cur && count < out_cap &&
           cur->nHeight > stop_height_exclusive) {
        if (steps++ > GAPFILL_WALK_CAP) return -1; // raw-return-ok:sentinel
        if (cur->nHeight >= last_h) return -1; // raw-return-ok:sentinel
        last_h = cur->nHeight;
        out[count++] = cur;
        cur = cur->pprev;
    }
    return count;
}

/* One pass: scan [tip+1, best_header] for missing data, queue
 * downloads. Returns number of blocks enqueued (0 = idle, -1 =
 * corrupt walk detected). */
static int gap_fill_pass(void)
{
    struct main_state *ms = g_gf.ms;
    struct download_manager *dm = g_gf.dm;
    if (!ms || !dm) return 0;

    /* Snapshot tip and best_header under cs_main. We hold the lock
     * only for the pointer reads + pprev walk; the dl_queue_blocks
     * call is done outside the lock. */
    zcl_mutex_lock(&ms->cs_main);
    int tip_h = active_chain_height(&ms->chain_active);
    struct block_index *best = ms->pindex_best_header;
    int best_h = best ? best->nHeight : 0;

    if (!best || best_h <= tip_h) {
        zcl_mutex_unlock(&ms->cs_main);
        g_gf.stats.last_tip_h  = tip_h;
        g_gf.stats.last_best_h = best_h;
        return 0;
    }

    /* Window: collect at most GAPFILL_WINDOW indices from pprev,
     * stopping at tip_h (exclusive). */
    int window = GAPFILL_WINDOW;
    if (best_h - tip_h < window) window = best_h - tip_h;
    struct block_index **bis = zcl_malloc((size_t)window * sizeof(*bis),
                                          "gap_fill_window");
    if (!bis) {
        zcl_mutex_unlock(&ms->cs_main);
        return 0;
    }
    int collected = collect_pprev_window(best, tip_h, bis, window);
    if (collected < 0) {
        zcl_mutex_unlock(&ms->cs_main);
        free(bis);
        return -1; // raw-return-ok:sentinel
    }

    /* Filter: needs data AND not in-flight. Build parallel arrays for
     * dl_queue_blocks. We allocate up to `collected` slots. */
    struct uint256 *hashes = zcl_malloc((size_t)collected * sizeof(*hashes),
                                        "gap_fill_hashes");
    int32_t *heights = zcl_malloc((size_t)collected * sizeof(*heights),
                                  "gap_fill_heights");
    if (!hashes || !heights) {
        zcl_mutex_unlock(&ms->cs_main);
        free(bis); free(hashes); free(heights);
        return 0;
    }

    int n_need = 0;
    int lo = bis[collected - 1] ? bis[collected - 1]->nHeight : tip_h + 1;
    int hi = bis[0] ? bis[0]->nHeight : tip_h;
    for (int i = 0; i < collected; i++) {
        struct block_index *bi = bis[i];
        if (!bi || !bi->phashBlock) continue;
        if (bi->nStatus & BLOCK_HAVE_DATA) continue;
        if (dl_is_in_flight(dm, bi->phashBlock)) continue;
        hashes[n_need]  = *bi->phashBlock; /* value copy */
        heights[n_need] = bi->nHeight;
        n_need++;
    }
    zcl_mutex_unlock(&ms->cs_main);
    free(bis);

    int enqueued = 0;
    if (n_need > 0) {
        size_t added = dl_queue_blocks(dm, hashes, heights,
                                       (size_t)n_need);
        enqueued = (int)added;
        if (added > 0) {
            printf("[gap-fill] queued %zu blocks (window [%d..%d] "
                   "tip=%d best=%d)\n",
                   added, lo, hi, tip_h, best_h);
            event_emitf(EV_BLOCK_REQUESTED, 0,
                        "gap_fill queued=%zu lo=%d hi=%d tip=%d best=%d",
                        added, lo, hi, tip_h, best_h);
        }
    }

    free(hashes);
    free(heights);

    g_gf.stats.last_tip_h     = tip_h;
    g_gf.stats.last_best_h    = best_h;
    g_gf.stats.last_window_lo = lo;
    g_gf.stats.last_window_hi = hi;
    return enqueued;
}

static void *gap_fill_thread_main(void *arg)
{
    (void)arg;
    printf("[gap-fill] service started\n");
    while (!atomic_load(&g_gf.stop_requested)) {
        int n = gap_fill_pass();
        g_gf.stats.passes++;
        if (n > 0) {
            g_gf.stats.blocks_enqueued += (uint64_t)n;
        } else if (n == 0) {
            g_gf.stats.passes_idle++;
        } else {
            g_gf.stats.passes_corrupt_walk++;
            fprintf(stderr, // obs-ok:pre-existing-diagnostic
                "[gap-fill] %s:%d %s(): corrupt pprev walk detected, "
                "skipping pass\n", __FILE__, __LINE__, __func__);
        }

        /* Sleep until kicked or GAPFILL_TICK_SECS elapsed. */
        pthread_mutex_lock(&g_gf.mu);
        if (!atomic_load(&g_gf.stop_requested)) {
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_sec += GAPFILL_TICK_SECS;
            pthread_cond_timedwait(&g_gf.cv, &g_gf.mu, &until);
        }
        pthread_mutex_unlock(&g_gf.mu);
    }
    printf("[gap-fill] service stopped (passes=%llu enqueued=%llu "
           "idle=%llu corrupt=%llu)\n",
           (unsigned long long)g_gf.stats.passes,
           (unsigned long long)g_gf.stats.blocks_enqueued,
           (unsigned long long)g_gf.stats.passes_idle,
           (unsigned long long)g_gf.stats.passes_corrupt_walk);
    return NULL;
}

bool gap_fill_start(struct main_state *ms, struct download_manager *dm)
{
    if (!ms || !dm) {
        LOG_FAIL("gap-fill", "start: null ms or dm");
    }
    if (atomic_load(&g_gf.running)) return true;
    g_gf.ms = ms;
    g_gf.dm = dm;
    memset(&g_gf.stats, 0, sizeof(g_gf.stats));
    atomic_store(&g_gf.stop_requested, false);
    if (thread_registry_spawn_ex("zcl_gap_fill", gap_fill_thread_main, NULL,
                                  &g_gf.thread) != 0) {
        LOG_FAIL("gap-fill", "thread_registry_spawn_ex failed: errno=%d", errno);
    }
    g_gf.thread_started = true;
    atomic_store(&g_gf.running, true);
    return true;
}

void gap_fill_stop(void)
{
    if (!atomic_load(&g_gf.running)) return;
    atomic_store(&g_gf.stop_requested, true);
    pthread_mutex_lock(&g_gf.mu);
    pthread_cond_broadcast(&g_gf.cv);
    pthread_mutex_unlock(&g_gf.mu);
    if (g_gf.thread_started) {
        /* Round 6 Part 3: cap join at 5 s. If the worker is stuck
         * (eg holding cs_main on a long pprev walk), detach rather
         * than block systemd shutdown past TimeoutStopSec. */
        struct timespec ts;
        if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
            ts.tv_sec += 5;
            int rc = pthread_timedjoin_np(g_gf.thread, NULL, &ts);
            if (rc != 0) {
                fprintf(stderr, // obs-ok:pre-existing-diagnostic
                        "gap_fill_stop: thread join timed out (rc=%d) — "
                        "detaching\n", rc);
                pthread_detach(g_gf.thread);
            }
        } else {
            pthread_join(g_gf.thread, NULL);
        }
        g_gf.thread_started = false;
    }
    atomic_store(&g_gf.running, false);
}

void gap_fill_kick(void)
{
    if (!atomic_load(&g_gf.running)) return;
    pthread_mutex_lock(&g_gf.mu);
    pthread_cond_broadcast(&g_gf.cv);
    pthread_mutex_unlock(&g_gf.mu);
}

void gap_fill_get_stats(struct gap_fill_stats *out)
{
    if (!out) return;
    if (!atomic_load(&g_gf.running)) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = g_gf.stats;
}
