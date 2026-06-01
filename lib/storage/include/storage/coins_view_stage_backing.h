/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * coins_view_stage_backing — authority-gated backing selection for
 * connect_block's input lookups.
 *
 * The reducer's per-block validator builds a `coins_view_cache` over a
 * `struct coins_view backing` that wraps the global coins.db-backed
 * `coins_tip` cache. This module supplies that backing, gated on the
 * single-writer authority flag (`utxo_projection_get_author()`):
 *
 *   author == UTXO_AUTHOR_LEGACY            →  the legacy view.
 *       The selector hands back the `struct coins_view` supplied by the
 *       caller (a copy of the coins_tip cache view).
 *
 *   author == UTXO_AUTHOR_STAGE              →  a COMPOSITE view:
 *       - get_coins / have_coins resolve through the UTXO **projection**
 *         (the authoritative set; closes the validation feedback loop —
 *         FRAMEWORK.md §0 Prime Directive).
 *       - get_best_block / batch_write delegate to the underlying legacy
 *         coins_tip view. batch_write here is NOT a second projection
 *         writer: the stage authors EV_UTXO_ADD/SPEND, and this flush only
 *         keeps the legacy coins.db mirror warm + carries the in-RAM cache
 *         deltas up to coins_tip. best_block delegation preserves the
 *         connect_block view/prevblock invariant.
 *
 * The RAM read-cache layer (`coins_view_cache`) is untouched and sits in
 * front of whichever backing is chosen, so the cache speedup is preserved
 * for both modes and connect_block does not own backing-selection details. */

#ifndef ZCL_STORAGE_COINS_VIEW_STAGE_BACKING_H
#define ZCL_STORAGE_COINS_VIEW_STAGE_BACKING_H

#include "coins/coins_view.h"
#include "storage/coins_view_projection.h"
#include "storage/utxo_projection.h"

#include <stdbool.h>

struct coins_view_sqlite;

/* A composite coins_view: reads from the projection, writes/best-block
 * to the legacy view. Used only when author == UTXO_AUTHOR_STAGE. Holds
 * its own published `struct coins_view view` (must be first for the
 * impl-pointer cast) plus the two delegate views (both borrowed). */
struct coins_view_stage_backing {
    struct coins_view view;                  /* published vtable (first) */
    struct coins_view_projection proj_view;  /* read side (owns nothing) */
    struct coins_view legacy;                /* write/best-block side (copy) */
    /* Authoritative coins.db handle owned by the reducer UTXO stage. When
     * non-NULL and author == UTXO_AUTHOR_STAGE,
     * batch_write commits the per-block delta DURABLY to coins.db itself
     * (the stage-owned authoritative write) instead of relying on the
     * legacy coins_tip flush. NULL (the default + the 4-arg selector)
     * keeps writes delegated to the supplied legacy view. Borrowed; not owned. */
    struct coins_view_sqlite *coins_db;
};

/* Authority-gated backing selection (extended form).
 *
 * `out`      — receives the chosen `struct coins_view` to hand to
 *              coins_view_cache_init(). MUST outlive the cache.
 * `sb`       — caller-owned scratch for the composite mode; only its
 *              `view` is referenced by `out` when STAGE is active. May be
 *              ignored by the caller's lifetime reasoning under LEGACY
 *              (it is left untouched there).
 * `legacy`   — the legacy coins_tip-backed view (from
 *              coins_view_cache_as_view(coins_tip)).
 * `proj`     — the global UTXO projection (may be NULL).
 * `coins_db` — the authoritative coins.db sqlite handle. When non-NULL
 *              and author == STAGE, batch_write commits the per-block
 *              delta DURABLY to coins.db itself (the stage-owned
 *              authoritative write) AND keeps the legacy coins_tip mirror
 *              warm. NULL (and the LEGACY author) keeps writes delegated to
 *              the supplied legacy view. Borrowed; not owned.
 *              May be NULL.
 *
 * Behavior:
 *   - author == LEGACY, or STAGE with proj == NULL  →  *out = *legacy
 *     (the existing path, unchanged). Returns false in the NULL-proj
 *     STAGE case after logging — the caller treats this as "stay legacy".
 *   - author == STAGE with a valid proj             →  builds the
 *     composite in *sb (carrying `coins_db`) and sets *out to sb->view.
 *     Returns true.
 *
 * Returns false only on a NULL required arg or a STAGE-without-projection
 * misconfiguration (with *out defaulted to *legacy so the caller can
 * proceed safely on the legacy path). Never aborts. */
bool coins_view_select_connect_backing_ex(struct coins_view *out,
                                           struct coins_view_stage_backing *sb,
                                           const struct coins_view *legacy,
                                           utxo_projection_t *proj,
                                           struct coins_view_sqlite *coins_db);

/* Four-arg selector: identical to the _ex form with coins_db == NULL
 * (the legacy coins_tip flush remains the coins.db writer). Used by the
 * parity test; callers that own the authoritative coins.db handle should
 * call the _ex form so the STAGE path can own the durable commit. */
bool coins_view_select_connect_backing(struct coins_view *out,
                                        struct coins_view_stage_backing *sb,
                                        const struct coins_view *legacy,
                                        utxo_projection_t *proj);

#endif /* ZCL_STORAGE_COINS_VIEW_STAGE_BACKING_H */
