/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Block Index Loader: projection-backed boot rebuild.
 *
 * load_block_index_from_projection() reconstructs the in-memory block
 * index map purely from the log-derived block_index_projection, then
 * seeds the active tip from the tip_finalize cursor in progress.kv. This
 * restores the active tip from the tip_finalize cursor in progress.kv.
 *
 * This file owns projection-backed rebuild. The shared height-sorted forward
 * pass (block_index_forward_pass) lives in block_index_loader.c and is
 * declared in services/block_index_loader.h. */

#include "platform/time_compat.h"
#include "services/block_index_loader.h"
#include "services/chain_tip.h"
#include "chain/chain.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include "storage/block_index_db.h"
#include "storage/block_index_projection.h"
#include "jobs/tip_finalize_stage.h"
#include "jobs/stage_helpers.h"
#include "core/uint256.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/log_macros.h"
#include "util/safe_alloc.h"

static int rebuild_cmp_height(const void *a, const void *b)
{
    const struct block_index *pa = *(const struct block_index *const *)a;
    const struct block_index *pb = *(const struct block_index *const *)b;
    if (pa->nHeight < pb->nHeight) return -1; // raw-return-ok:qsort-comparator
    if (pa->nHeight > pb->nHeight) return 1;
    return 0;
}

/* Per-row callback context for the projection fold. */
struct projection_fold_ctx {
    struct main_state *ms;
    size_t folded;
    bool   failed;
};

/* Fold one disk_block_index row into the in-memory map. Copies the same
 * scalar fields block_index_db.c maps, OMITTING the +1703 file-0 fixup
 * (the projection's nDataPos is this node's own body_persist position,
 * not a zclassicd-LDB position). pprev is linked in a second pass below
 * (the iterate order is height ASC, but a sibling/orphan can precede its
 * parent at the same height, so we must resolve pprev after all rows
 * are inserted — exactly as the flat/LevelDB loaders do). */
static bool projection_fold_cb(const uint8_t hash[32],
                               const struct disk_block_index *dbi,
                               void *user)
{
    struct projection_fold_ctx *c = (struct projection_fold_ctx *)user;

    struct uint256 h;
    memcpy(h.data, hash, 32);

    struct block_index *pindex = chainstate_insert_block_index(
        (struct chainstate *)c->ms, &h);
    if (!pindex) {
        c->failed = true;
        return false;  /* stop iteration on OOM */
    }

    pindex->nHeight              = dbi->nHeight;
    pindex->nFile                = dbi->nFile;
    pindex->nDataPos             = dbi->nDataPos;   /* no +1703 fixup */
    pindex->nUndoPos             = dbi->nUndoPos;
    pindex->nVersion             = dbi->nVersion;
    pindex->hashMerkleRoot       = dbi->hashMerkleRoot;
    pindex->hashFinalSaplingRoot = dbi->hashFinalSaplingRoot;
    pindex->nTime                = dbi->nTime;
    pindex->nBits                = dbi->nBits;
    pindex->nNonce               = dbi->nNonce;
    pindex->nSolution            = NULL;  /* not retained in RAM */
    pindex->nSolutionSize        = 0;
    pindex->nStatus              = dbi->nStatus;
    pindex->nCachedBranchId      = dbi->nCachedBranchId;
    pindex->nTx                  = dbi->nTx;
    if (dbi->has_sprout_value) {
        pindex->nSproutValue     = dbi->nSproutValue;
        pindex->has_sprout_value = true;
    }
    pindex->nSaplingValue        = dbi->nSaplingValue;

    c->folded++;
    return true;
}

/* Second-pass callback: link each in-memory entry's pprev via the
 * disk_block_index.hashPrev carried by the projection. Genesis (and any
 * row whose hashPrev is all-zero) keeps pprev == NULL. */
static bool projection_link_pprev_cb(const uint8_t hash[32],
                                     const struct disk_block_index *dbi,
                                     void *user)
{
    struct main_state *ms = (struct main_state *)user;

    if (uint256_is_null(&dbi->hashPrev))
        return true;  /* genesis / no parent */

    struct uint256 h;
    memcpy(h.data, hash, 32);
    struct block_index *pindex = block_map_find(&ms->map_block_index, &h);
    if (!pindex)
        return true;

    struct block_index *pprev = block_map_find(&ms->map_block_index,
                                               &dbi->hashPrev);
    if (pprev)
        pindex->pprev = pprev;
    return true;
}

/* Seed the active tip from the durable tip_finalize cursor. The cursor
 * counts finalized heights; the tip is at cursor-1. NULL `progress_db`
 * skips the seed (map rebuilt, no tip published). */
static void rebuild_seed_tip(struct main_state *ms, sqlite3 *progress_db)
{
    if (!progress_db)
        return;

    uint64_t cursor = stage_cursor_persisted(progress_db, "tip_finalize",
                                              "block_index_loader");
    if (cursor == 0)
        return;

    int tip_height = (int)cursor - 1;
    uint8_t tip_hash[32];
    if (!tip_finalize_stage_finalized_tip_at(progress_db, tip_height,
                                             tip_hash)) {
        LOG_WARN("block_index",
                 "load_block_index_from_projection: no finalized tip hash at "
                 "h=%d (cursor=%llu)",
                 tip_height, (unsigned long long)cursor);
        return;
    }

    struct uint256 th;
    memcpy(th.data, tip_hash, 32);
    struct block_index *tip = block_map_find(&ms->map_block_index, &th);
    if (!tip) {
        LOG_WARN("block_index",
                 "load_block_index_from_projection: tip hash at h=%d "
                 "(cursor=%llu) not found in folded map",
                 tip_height, (unsigned long long)cursor);
        return;
    }

    tip_finalize_stage_set_authoritative_tip(tip_height, tip_hash);
    struct zcl_result r = chain_set_active_tip(ms, tip, TIP_FROM_RESTORE,
                                               "loader_from_projection");
    if (!r.ok)
        LOG_WARN("block_index",
                 "load_block_index_from_projection: chain_set_active_tip "
                 "failed at h=%d: %s",
                 tip_height, r.message);
}

bool load_block_index_from_projection(struct main_state *ms,
                                      const struct chain_params *params,
                                      struct block_index_projection *bip,
                                      struct sqlite3 *progress_db)
{
    (void)params;
    if (!ms)
        LOG_FAIL("block_index",
                 "load_block_index_from_projection: null main_state");

    /* Cold / unwired: empty map, no tip. The caller (boot) seeds genesis
     * or fast_sync separately. */
    if (!bip)
        return true;

    /* (1) Drain the event log into the projection. */
    uint64_t off = block_index_projection_catch_up(bip);
    if (off == (uint64_t)-1)
        LOG_FAIL("block_index",
                 "load_block_index_from_projection: projection catch_up failed");

    /* (2) Fold every projection row into the in-memory map. */
    struct projection_fold_ctx ctx = { .ms = ms, .folded = 0, .failed = false };
    int64_t t0 = (int64_t)platform_time_wall_time_t();
    if (block_index_projection_iterate(bip, projection_fold_cb, &ctx) != 0 ||
        ctx.failed)
        LOG_FAIL("block_index",
                 "load_block_index_from_projection: fold failed after %zu rows",
                 ctx.folded);

    if (ctx.folded == 0) {
        /* Empty projection — cold datadir. Genesis/fast_sync seeds later. */
        printf("Block index projection: empty — no entries folded\n");
        return true;
    }

    /* Refresh phashBlock — map rehashing during inserts can move bucket
     * storage (block_map keeps the canonical hash in the bucket). */
    {
        size_t iter = 0;
        struct block_index *pi;
        const struct uint256 *hash;
        while (block_map_next(&ms->map_block_index, &iter, &hash, &pi)) {
            if (pi)
                pi->phashBlock = hash;
        }
    }

    /* (3) Link pprev via the carried hashPrev. Re-iterate the projection
     * (one ORDER BY scan) — hashPrev is not retained on the in-memory
     * entry. Resolving after all rows are inserted handles same-height
     * siblings/orphans correctly, exactly as the flat/LevelDB loaders. */
    if (block_index_projection_iterate(bip, projection_link_pprev_cb, ms) != 0)
        LOG_FAIL("block_index",
                 "load_block_index_from_projection: pprev link iterate failed");

    /* (4) Forward pass: nChainWork, nChainTx, skip links, branch id,
     * failed-child propagation — identical to load_block_index post-load. */
    size_t count = ms->map_block_index.size;
    struct block_index **sorted = zcl_malloc(
        count * sizeof(struct block_index *), "projection sorted");
    if (!sorted)
        LOG_FAIL("block_index",
                 "load_block_index_from_projection: malloc failed for %zu entries",
                 count);
    size_t idx = 0, iter = 0;
    struct block_index *pi;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &pi)) {
        if (pi && idx < count)
            sorted[idx++] = pi;
    }
    count = idx;
    qsort(sorted, count, sizeof(struct block_index *), rebuild_cmp_height);
    block_index_forward_pass(sorted, count);
    free(sorted);

    int64_t elapsed = (int64_t)platform_time_wall_time_t() - t0;
    printf("Block index projection: folded %zu entries in %llds\n",
           ctx.folded, (long long)elapsed);

    /* (5) Seed the tip from the durable tip_finalize cursor. */
    rebuild_seed_tip(ms, progress_db);

    return true;
}
