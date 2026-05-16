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

#include "core/uint256.h"
#include "core/arith_uint256.h"
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

/* Pure function: examines input, produces plan. No global state. */
void chain_restore_plan(struct chain_restore_plan *out,
                        const struct chain_restore_input *in);

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

/* ── Post-restore integrity check (P14.11 + P14.12) ────────────────
 *
 * Walks the main_state after an anchor-restore / snapshot-restore path
 * and surfaces the two invariants that `accept_block` / `connect_block`
 * normally establish but the anchor shortcut skips:
 *
 *   P14.11: every pindex above genesis must have `nBits != 0`.
 *           `GetNextWorkRequired` reads `pprev->nBits` and walks back
 *           `nPowAveragingWindow` steps — a zero anywhere in the window
 *           collapses the target to `nProofOfWorkLimit` and rejects
 *           every real-difficulty header as `bad-diffbits`.
 *
 *   P14.12: for every h in [0, chain_active.height], the slot
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
 * Round 4 Part 1.5: boot fail-fast gate honors `tip_window_holes`,
 * not `active_chain_holes`. The latter is informational and stays
 * positive on every live-tip-only boot. */
#define CHAIN_INTEGRITY_TIP_WINDOW 10000

struct chain_integrity_result {
    int  zero_nbits_count;
    int  active_chain_holes;      /* total NULL slots in [0, tip] */
    int  tip_window_holes;        /* NULL slots in [tip-WINDOW, tip] */
    int  tip_height;
    int  first_nbits_zero_height; /* -1 if none */
    int  first_hole_height;       /* -1 if none (overall) */
    int  first_tip_window_hole;   /* -1 if none (within window) */
    bool ok;                      /* zero_nbits_count==0 && tip_window_holes==0 */
};

void chain_integrity_check_post_restore(struct chain_integrity_result *out,
                                        const struct main_state *ms);

/* ── Boot snapshot ───────────────────────────────────────────────
 *
 * Captures the most recent post-restore integrity-check result plus
 * the most recent backfill counters. Updated automatically by
 * chain_integrity_check_post_restore and
 * chain_restore_backfill_nbits_from_disk. Reads via
 * chain_restore_get_boot_snapshot are atomic enough for diagnostic
 * use (single struct copy; no locking). */
struct chain_restore_boot_snapshot {
    bool   has_data;            /* false until first integrity check */
    int64_t boot_time;          /* time(NULL) when struct was last filled */
    /* From the most recent chain_integrity_check_post_restore */
    bool   integrity_ok;
    int    zero_nbits_count;
    int    active_chain_holes;
    int    tip_window_holes;
    int    tip_height;
    int    first_nbits_zero_height;
    int    first_hole_height;
    int    first_tip_window_hole;
    /* From the most recent chain_restore_backfill_nbits_from_disk */
    bool   backfill_ran;
    int    backfill_fixed;
    int    backfill_read_errors;
    int    backfill_off_chain_cleared;
    /* Round 7 A4: capture the most recent chain_restore_plan result so
     * `zcl_state(boot)` can show WHY boot reached the FAILED state
     * (e.g. "coins_best_block set but height unknown — awaiting P2P").
     * Without this, a stuck-at-IDLE boot is invisible until STATE_STUCK
     * fires 300s later via the live watchdog. */
    bool   plan_recorded;
    int    plan_next_state;     /* enum chain_restore_state encoded as int */
    int    plan_anchor_height;
    bool   plan_should_skip_activate;
    char   plan_reason[160];
    /* Round 7 B1: post-boot CSR consistency. csr_snapshot.consistent
     * compares tip_hash == coins_best_block. Diverges only after a
     * crash-window in the disconnect_tip path — boot reconstructs
     * chain_tip from coins_best_block, but a stale block_index could
     * still imply an inconsistency on the first activate pass. */
    bool   csr_consistency_checked;
    bool   csr_consistent;
    int    csr_tip_height;
    int    csr_header_height;
};

void chain_restore_get_boot_snapshot(struct chain_restore_boot_snapshot *out);

/* Internal — called from chain_restore_service.c. Updates the boot
 * snapshot so the next dumpstate / zcl_state call sees fresh values. */
void chain_restore_record_integrity_result(
    const struct chain_integrity_result *r);
void chain_restore_record_backfill_result(int fixed,
                                          int read_errors,
                                          int off_chain_cleared);
/* Round 7 A4: record the chain_restore_plan outcome. */
void chain_restore_record_plan_result(const struct chain_restore_plan *p);
/* Round 7 B1: snapshot CSR consistency into the boot snapshot. */
void chain_restore_record_csr_consistency(bool consistent,
                                          int tip_height,
                                          int header_height);

/* State-dump convention (see CLAUDE.md "Adding state introspection"). */
struct json_value;
bool chain_restore_dump_state_json(struct json_value *out, const char *key);

/* ── Post-restore repair (P14.11 + P14.12 GREEN) ──────────────────
 *
 * After an anchor-restore / snapshot-restore / block-file-scan path
 * completes, the block_index map and active_chain can exhibit the
 * two shapes that `chain_integrity_check_post_restore` flags:
 *
 *   P14.11: entries with `nBits==0` — created by the anchor path,
 *           or loaded from a block_index_cache row that lost its
 *           nBits column (e.g. a pre-P14.11 cache snapshot).
 *
 *   P14.12: `chain_active.chain[h]==NULL` for heights below the tip —
 *           `active_chain_set_tip` walks pprev and writes NULL into
 *           every slot where the walk dead-ends, leaving holes the
 *           explorer / `getblockhash` / bg_validation all fall over on.
 *
 * `chain_restore_rebuild_active_chain` walks tip->pprev and fills
 * slots; for any remaining holes below the deepest pprev-reachable
 * entry it falls back to a block_map scan keyed by nHeight (preferring
 * the BLOCK_HAVE_DATA + highest nChainWork candidate per height — the
 * live-node LDB snapshot is linear so there's one entry per height).
 *
 * `chain_restore_backfill_nbits_from_disk` walks block_map and, for
 * every pindex with `nBits==0 && nDataPos>0 && (nStatus &
 * BLOCK_HAVE_DATA)`, reads the block via `read_block_from_disk_index_
 * pread` and assigns `pindex->nBits = block.header.nBits`. Entries
 * without on-disk data (synthetic anchors) are skipped and will remain
 * flagged by the integrity check until the block arrives via P2P.
 *
 * `chain_restore_finalize` composes the two and runs the integrity
 * check; returns true iff the post-finalize check is clean. Logs the
 * result either way. `datadir == NULL` skips the disk-backfill limb
 * (used by unit tests that don't wire a real data directory). */

int chain_restore_rebuild_active_chain(struct main_state *ms,
                                       struct block_index *tip);

int chain_restore_backfill_nbits_from_disk(struct main_state *ms,
                                           const char *datadir);

/* Clear BLOCK_FAILED_VALID + BLOCK_FAILED_CHILD on entries strictly
 * above the active tip. After a body-pull / direct-import path writes
 * new blocks past a previously-stuck tip, stale FAILED flags from old
 * IBD attempts prevent find_most_work_chain from selecting through
 * them. Re-validation under evidence-mode is cheap; genuinely-invalid
 * blocks get re-flagged by the next connect_tip attempt. Returns the
 * number of entries cleared. */
int chain_restore_clear_failed_above_tip(struct main_state *ms);

bool chain_restore_block_is_consensus_backed(const struct block_index *tip);

bool chain_restore_block_is_consensus_backed_on_disk(
    const struct block_index *tip,
    const char *datadir);

struct block_index *chain_restore_nearest_consensus_backed_ancestor(
    struct block_index *tip);

struct block_index *chain_restore_nearest_consensus_backed_ancestor_on_disk(
    struct block_index *tip,
    const char *datadir);

bool chain_restore_finalize(struct main_state *ms, const char *datadir);

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
