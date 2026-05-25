/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Chain Restore Service — deterministic chain tip restoration.
 *
 * After importing UTXOs (from LDB, snapshot, or crash recovery), the
 * coins_best_block hash may not match any block in our block index.
 * This service resolves the gap by either:
 *   (a) finding the hash in the block map and setting it as tip, or
 *   (b) recording a non-consensus placeholder anchor at the correct height.
 *
 * Architecture: planning pattern (pure functions) + execution.
 *   1. chain_restore_plan()    — pure: decides what to do
 *   2. chain_restore_execute() — applies the plan to mutable state
 *   3. chain_restore_validate()— verifies post-conditions
 *
 * Replaces inline anchor creation in boot.c (3 copies → 1 service). */

#ifndef ZCL_CHAIN_RESTORE_SERVICE_H
#define ZCL_CHAIN_RESTORE_SERVICE_H

#include "platform/time_compat.h"
#include "core/uint256.h"
#include "core/arith_uint256.h"
#include "services/chain_restore_boot_snapshot.h"
#include "services/chain_restore_repair.h"
#include <stdbool.h>
#include <stdint.h>

struct main_state;
struct block_index;
struct coins_view_cache;

/* ── State machine ─────────────────────────────────────────────── */

enum chain_restore_state {
    CHAIN_RESTORE_UNRESOLVED = 0,   /* coins_best_block not evaluated */
    CHAIN_RESTORE_FOUND_IN_INDEX,   /* hash found in block_map */
    CHAIN_RESTORE_ANCHOR_CREATED,   /* placeholder anchor inserted */
    CHAIN_RESTORE_RESOLVED,         /* chain tip set, ready for sync */
    CHAIN_RESTORE_FAILED,           /* unrecoverable: no hash/height */
};

enum chain_restore_source {
    CHAIN_RESTORE_SRC_NORMAL_BOOT = 0,
    CHAIN_RESTORE_SRC_LDB_IMPORT,
    CHAIN_RESTORE_SRC_SNAPSHOT,
};

/* ── Planning (pure, no side effects) ──────────────────────────── */

struct chain_restore_input {
    struct uint256 coins_best_hash;     /* from LDB or coins_view_cache */
    int            utxo_max_height;     /* SELECT MAX(height) FROM utxos */
    bool           hash_found_in_map;   /* block_map_find returned non-NULL */
    int            found_height;        /* height of found block (if any) */
    bool           found_has_pprev;     /* found->pprev != NULL */
    enum chain_restore_source source;
};

struct chain_restore_plan {
    enum chain_restore_state next_state;
    bool should_create_anchor;
    bool should_set_chain_tip;
    bool should_set_best_header;
    bool should_set_snapshot_anchor;
    bool should_skip_activate;
    int  anchor_height;
    struct uint256 anchor_hash;
    char reason[128];
};

#include "services/chain_restore_planner.h"

/* ── Execution ─────────────────────────────────────────────────── */

/* Create a placeholder anchor block_index at `height` with `hash`.
 * Inserts it into ms->map_block_index as metadata only. The anchor is
 * deliberately not marked BLOCK_HAVE_DATA and receives no synthetic
 * chainwork; it must never win chain selection or become active consensus
 * tip until real block bytes arrive and normal validation fills it in.
 * This is the shared implementation (was duplicated 3x in boot.c). */
struct block_index *chain_restore_create_anchor(
    struct main_state *ms,
    const struct uint256 *hash,
    int height);

/* Apply the plan: create anchor if needed, set chain tip, etc.
 * Returns the anchor or found block_index, NULL on failure. */
struct block_index *chain_restore_execute(
    const struct chain_restore_plan *plan,
    struct main_state *ms);

/* ── Validation (post-execution checks) ────────────────────────── */

struct chain_restore_validation {
    bool coins_hash_valid;       /* hash is not null */
    bool anchor_in_map;          /* anchor found in block_map */
    bool chain_tip_set;          /* active_chain_tip != NULL */
    bool tip_matches_expected;   /* tip height == expected height */
    bool all_ok;                 /* all checks passed */
};

void chain_restore_validate(struct chain_restore_validation *out,
                            const struct main_state *ms,
                            const struct uint256 *expected_hash,
                            int expected_height);

/* ── Post-restore integrity check ────────────────
 *
 * Walks the main_state after an anchor-restore / snapshot-restore path
 * and surfaces the two invariants that `accept_block` / `connect_block`
 * normally establish but the anchor shortcut skips:
 *
 * every pindex above genesis must have `nBits != 0`.
 *           `GetNextWorkRequired` reads `pprev->nBits` and walks back
 *           `nPowAveragingWindow` steps — a zero anywhere in the window
 *           collapses the target to `nProofOfWorkLimit` and rejects
 *           every real-difficulty header as `bad-diffbits`.
 *
 * for every h in [0, chain_active.height], the slot
 *           `chain_active.chain[h]` must be non-NULL. Holes break
 *           `getblockhash` and any consumer that walks the active
 *           chain by height (explorer, bg_validation, etc.).
 *
 * Pure function — does NOT mutate state. Designed to be called at the
 * end of the boot restore path (RED on pre-fix, GREEN after backfill)
 * and from unit tests. Sets `out->ok` iff both counts are zero. */
/* `tip_window_holes` is the number of NULL active_chain slots in the
 * range [max(0, tip - CHAIN_INTEGRITY_TIP_WINDOW), tip]. Holes below
 * this window are an expected by-product of the capped pprev walk
 * during live boot (only ~10k entries populated near the tip on a
 * 3M-block chain) and are not corruption — they get filled on demand
 * by code that needs ancestor lookups.
 *
 * boot fail-fast gate honors `tip_window_holes`,
 * not `active_chain_holes`. The latter is informational and stays
 * positive on every live-tip-only boot. */
#define CHAIN_INTEGRITY_TIP_WINDOW 10000

struct chain_integrity_result {
    int  zero_nbits_count;
    int  active_chain_holes;      /* total NULL slots in [0, tip] */
    int  active_chain_mismatches; /* slots whose block_index height != slot */
    int  tip_window_holes;        /* NULL slots in [tip-WINDOW, tip] */
    int  tip_height;
    int  first_nbits_zero_height; /* -1 if none */
    int  first_hole_height;       /* -1 if none (overall) */
    int  first_mismatch_height;   /* -1 if none */
    int  first_tip_window_hole;   /* -1 if none (within window) */
    bool ok;                      /* zero_nbits_count==0 && tip_window_holes==0 */
};

void chain_integrity_check_post_restore(struct chain_integrity_result *out,
                                        const struct main_state *ms);

/* ── Boot activation decision ──────────────────────────────────── */

enum activation_skip_reason {
    ACTIVATE_OK = 0,
    ACTIVATE_SKIP_LEGACY_IMPORT,
    ACTIVATE_SKIP_ANCHOR_CREATED,
    ACTIVATE_SKIP_NO_UTXOS_AWAITING,
    ACTIVATE_SKIP_REINDEX,
};

struct boot_activation_decision {
    bool should_activate;
    enum activation_skip_reason reason;
    int  chain_height;
    int64_t utxo_count;
    size_t  block_index_size;
};

/* Single function replaces 5 scattered skip_activate mutations.
 * Called once at boot, right before activate_best_chain(). */
void boot_should_activate_chain(struct boot_activation_decision *out,
                                int chain_tip_height,
                                int64_t utxo_count,
                                size_t block_index_size,
                                bool legacy_import,
                                bool anchor_was_created);

#endif
