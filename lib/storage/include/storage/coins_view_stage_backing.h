/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * coins_view_stage_backing — B4-wiring: the authority-gated backing
 * selection for connect_block's input lookups.
 *
 * connect_tip builds a per-block `coins_view_cache` over a `struct
 * coins_view backing` that wraps the global coins.db-backed `coins_tip`
 * cache. This module supplies that backing, gated on the single-writer
 * authority flag (`utxo_projection_get_author()`):
 *
 *   author == UTXO_AUTHOR_LEGACY (default)  →  the legacy view, UNCHANGED.
 *       The selector hands back the exact `struct coins_view` it was
 *       given (a copy of the coins_tip cache view). Byte-for-byte the
 *       same path that ships today. The whole STAGE machinery is DORMANT.
 *
 *   author == UTXO_AUTHOR_STAGE              →  a COMPOSITE view:
 *       - get_coins / have_coins resolve through the UTXO **projection**
 *         (the authoritative set; closes the validation feedback loop —
 *         FRAMEWORK.md §0 Prime Directive).
 *       - get_best_block / batch_write delegate to the underlying legacy
 *         coins_tip view. batch_write here is NOT a second projection
 *         writer: the stage authors EV_UTXO_ADD/SPEND (B3), and this
 *         flush only keeps the legacy coins.db mirror warm + carries the
 *         in-RAM cache deltas up to coins_tip exactly as today. best_block
 *         delegation preserves the connect_block view/prevblock invariant.
 *
 * The RAM read-cache layer (`coins_view_cache`) is untouched and sits in
 * front of whichever backing is chosen, so the cache speedup is preserved
 * for free in both modes. connect_block call sites are byte-identical.
 *
 * Dormant until B7 flips the author to STAGE. */

#ifndef ZCL_STORAGE_COINS_VIEW_STAGE_BACKING_H
#define ZCL_STORAGE_COINS_VIEW_STAGE_BACKING_H

#include "coins/coins_view.h"
#include "storage/coins_view_projection.h"
#include "storage/utxo_projection.h"

#include <stdbool.h>

/* A composite coins_view: reads from the projection, writes/best-block
 * to the legacy view. Used only when author == UTXO_AUTHOR_STAGE. Holds
 * its own published `struct coins_view view` (must be first for the
 * impl-pointer cast) plus the two delegate views (both borrowed). */
struct coins_view_stage_backing {
    struct coins_view view;                  /* published vtable (first) */
    struct coins_view_projection proj_view;  /* read side (owns nothing) */
    struct coins_view legacy;                /* write/best-block side (copy) */
};

/* Authority-gated backing selection.
 *
 * `out`     — receives the chosen `struct coins_view` to hand to
 *             coins_view_cache_init(). MUST outlive the cache.
 * `sb`      — caller-owned scratch for the composite mode; only its
 *             `view` is referenced by `out` when STAGE is active. May be
 *             ignored by the caller's lifetime reasoning under LEGACY
 *             (it is left untouched there).
 * `legacy`  — the legacy coins_tip-backed view (from
 *             coins_view_cache_as_view(coins_tip)).
 * `proj`    — the global UTXO projection (may be NULL).
 *
 * Behavior:
 *   - author == LEGACY, or STAGE with proj == NULL  →  *out = *legacy
 *     (the existing path, unchanged). Returns false in the NULL-proj
 *     STAGE case after logging — the caller treats this as "stay legacy".
 *   - author == STAGE with a valid proj             →  builds the
 *     composite in *sb and sets *out to sb->view. Returns true.
 *
 * Returns false only on a NULL required arg or a STAGE-without-projection
 * misconfiguration (with *out defaulted to *legacy so the caller can
 * proceed safely on the legacy path). Never aborts. */
bool coins_view_select_connect_backing(struct coins_view *out,
                                        struct coins_view_stage_backing *sb,
                                        const struct coins_view *legacy,
                                        utxo_projection_t *proj);

#endif /* ZCL_STORAGE_COINS_VIEW_STAGE_BACKING_H */
