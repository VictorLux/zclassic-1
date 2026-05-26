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

bool coins_view_select_connect_backing(struct coins_view *out,
                                       struct coins_view_stage_backing *sb,
                                       const struct coins_view *legacy,
                                       utxo_projection_t *proj)
{
    if (!out || !legacy)
        LOG_FAIL("coins_view_stage_backing",
                 "select: NULL arg (out=%p legacy=%p)",
                 (const void *)out, (const void *)legacy);

    /* Default + the dormant LEGACY path: hand back the legacy view
     * verbatim. Byte-identical to the path that ships today. */
    if (utxo_projection_get_author() != UTXO_AUTHOR_STAGE) {
        *out = *legacy;
        return true;
    }

    /* STAGE authority. The projection is required to read the
     * authoritative set; a misconfiguration (author flipped but the
     * projection handle absent) must NOT silently read coins.db as if
     * authoritative — but it also must not crash the connect path.
     * Log it and fall back to the legacy view (the caller proceeds; the
     * cutover canary in B7 is what catches an actual STAGE divergence). */
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
    sb->view.vtable = &csb_vtable;
    sb->view.impl   = sb;
    *out = sb->view;
    return true;
}
