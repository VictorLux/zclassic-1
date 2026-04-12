/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Background block hash verification — recomputes SHA256d of every block
 * header on disk and compares against the stored hash in the block index.
 * See bg_hash_verification_service.h for design overview. */

#include "services/bg_hash_verification_service.h"
#include "validation/main_state.h"
#include "validation/chainstate.h"
#include "storage/disk_block_io.h"
#include "primitives/block.h"
#include "models/database.h"
#include "event/event.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include "util/log_macros.h"

#define SAVE_INTERVAL 1000
#define LOG_INTERVAL  10000

struct bg_hash_verification_service *g_bg_hash_verify = NULL;

/* ── Progress persistence ─────────────────────────────────────── */

static int load_progress(struct node_db *ndb)
{
    if (!ndb || !ndb->open) return 0;
    int64_t val = 0;
    if (node_db_state_get_int(ndb, "bg_hash_verification_height", &val))
        return (int)val;
    return 0;
}

static void save_progress(struct node_db *ndb, int height)
{
    if (!ndb || !ndb->open) return;
    node_db_state_set_int(ndb, "bg_hash_verification_height", (int64_t)height);
}

/* ── State names ──────────────────────────────────────────────── */

const char *bg_hash_verify_state_name(enum bg_hash_verify_state state)
{
    switch (state) {
    case BG_HASH_VERIFY_IDLE:     return "idle";
    case BG_HASH_VERIFY_RUNNING:  return "running";
    case BG_HASH_VERIFY_COMPLETE: return "complete";
    case BG_HASH_VERIFY_FAILED:   return "failed";
    default:                      return "unknown";
    }
}

/* ── Main verification thread ─────────────────────────────────── */

static void *bg_hash_verify_thread(void *arg)
{
    struct bg_hash_verification_service *svc = arg;
    struct main_state *ms = svc->ms;

    int start_height = load_progress(svc->ndb);
    if (start_height < 1) start_height = 1; /* skip genesis (no block file) */

    int chain_height = active_chain_height(&ms->chain_active);
    atomic_store(&svc->progress.chain_height, chain_height);

    if (start_height > chain_height) {
        atomic_store(&svc->progress.state, BG_HASH_VERIFY_COMPLETE);
        printf("[bg-hash-verify] Already complete (verified to h=%d)\n",
               start_height);
        return NULL;
    }

    printf("[bg-hash-verify] Starting hash verification from h=%d to h=%d\n",
           start_height, chain_height);
    atomic_store(&svc->progress.state, BG_HASH_VERIFY_RUNNING);

    struct timespec ts_start;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    int verified = 0;
    int mismatches = 0;

    for (int h = start_height; h <= chain_height; h++) {
        if (atomic_load(&svc->stop_requested)) break;

        const struct block_index *pindex =
            active_chain_at(&ms->chain_active, h);
        if (!pindex || !pindex->phashBlock) continue;

        /* Skip blocks without data on disk */
        if (!(pindex->nStatus & BLOCK_HAVE_DATA)) continue;

        /* Read block from disk (pread — thread-safe, no FILE* cache) */
        struct block blk;
        block_init(&blk);
        if (!read_block_from_disk_index_pread(&blk, pindex, svc->datadir)) {
            block_free(&blk);
            continue; /* block file may be missing — not a hash error */
        }

        /* Recompute hash from the deserialized header */
        struct uint256 computed;
        block_header_get_hash(&blk.header, &computed);
        block_free(&blk);

        /* Compare against stored hash */
        if (uint256_cmp(&computed, pindex->phashBlock) != 0) {
            char exp[65], got[65];
            uint256_get_hex(pindex->phashBlock, exp);
            uint256_get_hex(&computed, got);
            fprintf(stderr, "[bg-hash-verify] MISMATCH at h=%d!\n"
                    "  stored:   %s\n  computed: %s\n", h, exp, got);
            mismatches++;
            atomic_store(&svc->progress.mismatches, mismatches);
        }

        verified++;
        atomic_store(&svc->progress.verified_height, h);

        /* Periodic save + log */
        if (h % SAVE_INTERVAL == 0)
            save_progress(svc->ndb, h);
        if (h % LOG_INTERVAL == 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double elapsed = (now.tv_sec - ts_start.tv_sec) +
                (now.tv_nsec - ts_start.tv_nsec) / 1e9;
            double bps = elapsed > 0 ? verified / elapsed : 0;
            printf("[bg-hash-verify] h=%d/%d (%.0f blocks/s, %d mismatches)\n",
                   h, chain_height, bps, mismatches);
        }

        /* Update chain height periodically (chain may grow) */
        if (h % SAVE_INTERVAL == 0) {
            int new_tip = active_chain_height(&ms->chain_active);
            if (new_tip > chain_height) chain_height = new_tip;
            atomic_store(&svc->progress.chain_height, chain_height);
        }
    }

    /* Final save */
    if (!atomic_load(&svc->stop_requested))
        save_progress(svc->ndb, chain_height);

    struct timespec ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    double total = (ts_end.tv_sec - ts_start.tv_sec) +
        (ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;

    if (mismatches > 0) {
        fprintf(stderr, "[bg-hash-verify] FAILED: %d mismatches in %d blocks "
                "(%.0fs)\n", mismatches, verified, total);
        atomic_store(&svc->progress.state, BG_HASH_VERIFY_FAILED);
    } else if (!atomic_load(&svc->stop_requested)) {
        printf("[bg-hash-verify] Complete: %d blocks verified, 0 mismatches "
               "(%.0fs)\n", verified, total);
        atomic_store(&svc->progress.state, BG_HASH_VERIFY_COMPLETE);
    }

    return NULL;
}

/* ── Public API ───────────────────────────────────────────────── */

void bg_hash_verify_init(struct bg_hash_verification_service *svc,
                         struct main_state *ms,
                         struct node_db *ndb,
                         const char *datadir,
                         const struct chain_params *params)
{
    memset(svc, 0, sizeof(*svc));
    svc->ms = ms;
    svc->ndb = ndb;
    svc->datadir = datadir;
    svc->params = params;
    atomic_store(&svc->stop_requested, false);
    atomic_store(&svc->progress.state, BG_HASH_VERIFY_IDLE);
}

bool bg_hash_verify_start(struct bg_hash_verification_service *svc)
{
    if (!svc || !svc->ms || svc->thread_started)
        LOG_FAIL("bg_hash", "bg_hash_verify_start: null svc=%d ms=%d or already started=%d",
                 !svc, svc ? !svc->ms : 1, svc ? svc->thread_started : 0);

    if (pthread_create(&svc->thread, NULL, bg_hash_verify_thread, svc) != 0) {
        fprintf(stderr, "[bg-hash-verify] Failed to create thread\n");
        return false;
    }

    svc->thread_started = true;
    g_bg_hash_verify = svc;
    return true;
}

void bg_hash_verify_stop(struct bg_hash_verification_service *svc)
{
    if (!svc || !svc->thread_started) return;
    atomic_store(&svc->stop_requested, true);
    pthread_join(svc->thread, NULL);
    svc->thread_started = false;
}

struct bg_hash_verify_progress bg_hash_verify_get_progress(
    const struct bg_hash_verification_service *svc)
{
    struct bg_hash_verify_progress p;
    p.verified_height = atomic_load(&svc->progress.verified_height);
    p.chain_height = atomic_load(&svc->progress.chain_height);
    p.mismatches = atomic_load(&svc->progress.mismatches);
    p.state = atomic_load(&svc->progress.state);
    return p;
}
