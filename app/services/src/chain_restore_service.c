/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Chain Restore Service — deterministic chain tip restoration.
 * See chain_restore_service.h for architecture overview. */

#include "services/chain_restore_service.h"
#include "validation/main_state.h"
#include "validation/chainstate.h"
#include "chain/chain.h"
#include <string.h>

/* ── Validation ────────────────────────────────────────────────── */

void chain_restore_validate(struct chain_restore_validation *out,
                            const struct main_state *ms,
                            const struct uint256 *expected_hash,
                            int expected_height)
{
    memset(out, 0, sizeof(*out));

    out->coins_hash_valid = expected_hash && !uint256_is_null(expected_hash);

    if (expected_hash) {
        struct block_index *found = block_map_find(
            &ms->map_block_index, expected_hash);
        out->anchor_in_map = (found != NULL);
    }

    struct block_index *tip = active_chain_tip(&ms->chain_active);
    out->chain_tip_set = (tip != NULL);

    if (tip && expected_height > 0)
        out->tip_matches_expected = (tip->nHeight == expected_height);

    out->all_ok = out->coins_hash_valid
               && out->anchor_in_map
               && out->chain_tip_set
               && out->tip_matches_expected;
}

/* ── Post-restore integrity check ────────────── */

void chain_integrity_check_post_restore(struct chain_integrity_result *out,
                                        const struct main_state *ms)
{
    memset(out, 0, sizeof(*out));
    out->first_nbits_zero_height = -1;
    out->first_hole_height = -1;
    out->first_mismatch_height = -1;
    out->first_tip_window_hole = -1;

    if (!ms) {
        out->ok = false;
        return;
    }

    /* every pindex with on-disk data must have nBits != 0.
     *
     * skip nBits=0 entries that have no BLOCK_HAVE_DATA bit.
     * Those are metadata-anchor placeholders left by chain_restore when
     * coins_best_block was unrecoverable from disk. They never enter
     * validation walks (no header is loaded), so a zero nBits on them
     * is harmless. Failing the integrity gate on such an entry —
     * which is the only thing we ever WRITE during the anchor-recovery
     * path — would crash-loop the node forever. */
    size_t iter = 0;
    struct block_index *pi;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &pi)) {
        if (!pi || pi->nHeight <= 0)
            continue;
        if (!(pi->nStatus & BLOCK_HAVE_DATA))
            continue;
        if (pi->nBits == 0) {
            out->zero_nbits_count++;
            if (out->first_nbits_zero_height < 0 ||
                pi->nHeight < out->first_nbits_zero_height)
                out->first_nbits_zero_height = pi->nHeight;
        }
    }

    /* chain_active.chain[h] non-NULL for h in [0, tip]. */
    out->tip_height = active_chain_height(&ms->chain_active);
    int window_lo = out->tip_height - CHAIN_INTEGRITY_TIP_WINDOW;
    if (window_lo < 0) window_lo = 0;
    for (int h = 0; h <= out->tip_height; h++) {
        struct block_index *at = active_chain_at(&ms->chain_active, h);
        if (at == NULL) {
            out->active_chain_holes++;
            if (out->first_hole_height < 0 || h < out->first_hole_height)
                out->first_hole_height = h;
            if (h >= window_lo) {
                out->tip_window_holes++;
                if (out->first_tip_window_hole < 0 ||
                    h < out->first_tip_window_hole)
                    out->first_tip_window_hole = h;
            }
        } else if (at->nHeight != h) {
            out->active_chain_mismatches++;
            if (out->first_mismatch_height < 0 ||
                h < out->first_mismatch_height)
                out->first_mismatch_height = h;
        } else if (h > 0 && at->pprev != active_chain_at(&ms->chain_active, h - 1)) {
            out->active_chain_mismatches++;
            if (out->first_mismatch_height < 0 ||
                h < out->first_mismatch_height)
                out->first_mismatch_height = h;
        }
    }

    /* `ok` reflects operational health.
     *
     * The capped pprev walk during live boot populates ~10k DISTINCT
     * heights, but those heights can be scattered (the walk follows
     * pprev pointers which may collapse heights in a partly-restored
     * block_map). So tip_window_holes can be positive even on a sane
     * live boot.
     *
     * The operational requirement is weaker: the tip itself must be
     * resolvable (active_chain_at(tip_h) == tip), and nBits must be
     * intact across the whole map. Lookups by height that miss go
     * through block_map walks; they're slower but correct.
     *
     * Keep tip_window_holes / first_*_height fields as diagnostic
     * counters but don't gate `ok` on them. `ok` requires only nBits
     * clean + tip slot populated. */
    bool tip_slot_ok =
        (out->tip_height < 0) ||
        (active_chain_at(&ms->chain_active, out->tip_height) != NULL);
    out->ok = (out->zero_nbits_count == 0 && tip_slot_ok &&
               out->tip_window_holes == 0);
    out->ok = out->ok && out->active_chain_mismatches == 0;

    /* Cache the result for `dumpstate subsystem=boot` / `zcl_state`. */
    chain_restore_record_integrity_result(out);
}

/* ── Boot activation decision ──────────────────────────────────── */

void boot_should_activate_chain(struct boot_activation_decision *out,
                                int chain_tip_height,
                                int64_t utxo_count,
                                size_t block_index_size,
                                bool legacy_import,
                                bool anchor_was_created)
{
    memset(out, 0, sizeof(*out));
    out->chain_height = chain_tip_height;
    out->utxo_count = utxo_count;
    out->block_index_size = block_index_size;

    if (legacy_import) {
        out->should_activate = false;
        out->reason = ACTIVATE_SKIP_LEGACY_IMPORT;
        return;
    }

    if (anchor_was_created) {
        out->should_activate = false;
        out->reason = ACTIVATE_SKIP_ANCHOR_CREATED;
        return;
    }

    /* No UTXOs + many headers = awaiting P2P snapshot.
     * Connecting blocks from genesis would mark valid blocks FAILED. */
    if (utxo_count < 100000 && chain_tip_height == 0
        && block_index_size > 1000) {
        out->should_activate = false;
        out->reason = ACTIVATE_SKIP_NO_UTXOS_AWAITING;
        return;
    }

    out->should_activate = true;
    out->reason = ACTIVATE_OK;
}
