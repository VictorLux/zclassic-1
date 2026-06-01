/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * coins_view_stage_backing — implementation. See the header.
 *
 * The composite is a thin vtable that fans out by operation:
 *   reads  -> projection-backed coins_view (authoritative)
 *   writes -> legacy coins_tip view (keep coins.db mirror warm; B3 single
 *             writer is preserved because the projection is authored by
 *             events, never by this batch_write)
 *   best   -> legacy coins_tip view (preserve connect_block prevblock
 *             invariant) */

#include "storage/coins_view_stage_backing.h"

#include "coins/coins.h"
#include "core/uint256.h"
#include "storage/coins_view_sqlite.h"
#include "util/log_macros.h"

#include <stdio.h>

/* ── composite vtable: reads -> projection, writes/best -> legacy ── */

static bool csb_get_coins_impl(void *self, const struct uint256 *txid,
                               struct coins *out)
{
    struct coins_view_stage_backing *sb =
        (struct coins_view_stage_backing *)self;
    if (!sb) return false;
    return coins_view_get_coins(&sb->proj_view.view, txid, out);
}

static bool csb_have_coins_impl(void *self, const struct uint256 *txid)
{
    struct coins_view_stage_backing *sb =
        (struct coins_view_stage_backing *)self;
    if (!sb) return false;
    return coins_view_have_coins(&sb->proj_view.view, txid);
}

static bool csb_get_best_block_impl(void *self, struct uint256 *hash)
{
    struct coins_view_stage_backing *sb =
        (struct coins_view_stage_backing *)self;
    if (!sb) return false;
    return coins_view_get_best_block(&sb->legacy, hash);
}

static bool csb_batch_write_impl(void *self, struct coins_map *map_coins,
                                 const struct uint256 *hash_block)
{
    struct coins_view_stage_backing *sb =
        (struct coins_view_stage_backing *)self;
    if (!sb) return false;
    if (!sb->legacy.vtable || !sb->legacy.vtable->batch_write)
        LOG_FAIL("coins_view_stage_backing",
                 "batch_write: legacy backing has no batch_write");

    /* The reducer UTXO stage owns the authoritative coins.db write.
     * When the author is STAGE and we hold the coins.db
     * handle, commit this block's validated delta DURABLY to coins.db
     * ourselves (its own BEGIN IMMEDIATE / COMMIT) so coins.db is a STAGE
     * OUTPUT — the SHA3 UTXO checkpoint + gettxoutsetinfo (both read
     * coins.db) stay consistent with the projection the stage authors.
     *
     * This is the destination reducer-path writer, NOT a legacy surface:
     * projection-backed reads are used only when the reducer owns UTXO
     * authorship. */
    if (sb->coins_db &&
        utxo_projection_get_author() == UTXO_AUTHOR_STAGE) {
        bool durable = coins_view_sqlite_batch_write_ex( // one-write-path-ok:reducer-utxo-authority
            sb->coins_db, map_coins, hash_block, NULL);
        if (!durable)
            LOG_FAIL("coins_view_stage_backing",
                     "batch_write: STAGE-owned coins.db commit failed");
        /* Keep the legacy coins_tip mirror warm so the downstream
         * reducer flush + SHA3 checkpoint see a consistent cache. */
    }

    return sb->legacy.vtable->batch_write(sb->legacy.impl, map_coins,
                                          hash_block);
}

static bool csb_get_stats_impl(void *self, struct coins_stats *stats)
{
    struct coins_view_stage_backing *sb =
        (struct coins_view_stage_backing *)self;
    if (!sb) return false;
    if (sb->legacy.vtable && sb->legacy.vtable->get_stats)
        return sb->legacy.vtable->get_stats(sb->legacy.impl, stats);
    return false;
}

static struct coins_view_vtable csb_vtable = {
    .get_coins      = csb_get_coins_impl,
    .have_coins     = csb_have_coins_impl,
    .get_best_block = csb_get_best_block_impl,
    .batch_write    = csb_batch_write_impl,
    .get_stats      = csb_get_stats_impl,
};

bool coins_view_select_connect_backing_ex(struct coins_view *out,
                                          struct coins_view_stage_backing *sb,
                                          const struct coins_view *legacy,
                                          utxo_projection_t *proj,
                                          struct coins_view_sqlite *coins_db)
{
    if (!out || !legacy)
        LOG_FAIL("coins_view_stage_backing",
                 "select: NULL arg (out=%p legacy=%p)",
                 (const void *)out, (const void *)legacy);

    /* Default + LEGACY author: hand back the caller-supplied legacy view. */
    if (utxo_projection_get_author() != UTXO_AUTHOR_STAGE) {
        *out = *legacy;
        return true;
    }

    /* STAGE authority. The projection is required to read the
     * authoritative set; a misconfiguration (author flipped but the
     * projection handle absent) must NOT silently read coins.db as if
     * authoritative — but it also must not crash the connect path.
     * Log it and fall back to the legacy view; reducer/projection parity checks
     * catch an actual STAGE divergence. */
    if (!proj || !sb) {
        *out = *legacy;
        fprintf(stderr, "[coins_view_stage_backing] %s:%d %s(): STAGE author "
                "but %s NULL — falling back to legacy backing\n",
                __FILE__, __LINE__, __func__,
                !proj ? "projection" : "scratch");
        return false;
    }

    if (!coins_view_projection_init(&sb->proj_view, proj)) {
        *out = *legacy;
        fprintf(stderr, "[coins_view_stage_backing] %s:%d %s(): projection "
                "view init failed — falling back to legacy\n",
                __FILE__, __LINE__, __func__);
        return false;
    }
    sb->legacy      = *legacy;
    sb->coins_db    = coins_db;   /* STAGE-owned durable coins.db (may be NULL) */
    sb->view.vtable = &csb_vtable;
    sb->view.impl   = sb;
    *out = sb->view;
    return true;
}

bool coins_view_select_connect_backing(struct coins_view *out,
                                       struct coins_view_stage_backing *sb,
                                       const struct coins_view *legacy,
                                       utxo_projection_t *proj)
{
    /* coins_db == NULL: the legacy coins_tip flush stays the coins.db
     * writer (the existing behavior, unchanged). */
    return coins_view_select_connect_backing_ex(out, sb, legacy, proj, NULL);
}
