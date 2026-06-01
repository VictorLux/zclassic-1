/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * utxo_apply_delta — the B5 reorg-unwind keystone, split out of
 * utxo_apply_stage.c (file-size ceiling E1). Internal to the
 * utxo_apply job: the per-block inverse-delta record (the stage analogue
 * of legacy block_undo) plus the stage-side disconnect path.
 *
 * Not a public API — only utxo_apply_stage.c includes this. The two
 * delta types are shared by compute_block_delta (which fills them) and
 * the persistence/unwind code (which serializes and inverts them). */

#ifndef ZCL_JOBS_UTXO_APPLY_DELTA_H
#define ZCL_JOBS_UTXO_APPLY_DELTA_H

#include "core/uint256.h"
#include "jobs/utxo_apply_stage.h"  /* utxo_apply_lookup_fn */

#include <sqlite3.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct block;
struct main_state;
struct stage;

/* One created/restored coin in a block delta. */
struct delta_entry {
    struct uint256 txid;
    uint32_t vout;
    int64_t value;
    /* B3 authorship: added entries also carry what EV_UTXO_ADD needs.
     * `script` aliases into the live `struct block` and is valid only
     * until block_free — emission must happen before the block is freed.
     *
     * B5 reorg-unwind: SPENT entries ALSO carry the full pre-image
     * (height + is_coinbase + script) so a stage-side disconnect can
     * emit a correct restore-ADD (the inverse of the SPEND), matching
     * the former disconnect block path byte-for-byte. For a spent entry the
     * script bytes live in the owned `script_owned` buffer (the live
     * block's tx_out is gone by disconnect time), and `script` aliases
     * into it; `height` is the coin's ORIGINAL creation height. For an
     * added entry `script` aliases the live block and `script_owned`
     * is NULL. */
    const uint8_t *script;
    uint32_t script_len;
    bool is_coinbase;
    uint32_t height;        /* spent: coin's original creation height */
    uint8_t *script_owned;  /* spent: owned copy of the restore script */
};

/* The full transparent UTXO delta of one block. */
struct delta_summary {
    bool ok;
    const char *status;
    const char *failure_kind;
    uint8_t failure_detail[36];
    size_t spent_count;
    size_t added_count;
    int64_t total_value_delta;
    /* On a successful delta these own the spent/added arrays and are
     * handed back to the caller (which emits then frees). On any
     * failure path compute_block_delta frees them and leaves NULL. */
    struct delta_entry *spent;
    struct delta_entry *added;
};

/* Free a delta-entry array, releasing any owned restore-scripts in the
 * first `n` entries (only spent entries below the running count carry
 * one). `n` may be 0 to free only the array block. */
void free_delta_arr(struct delta_entry *arr, size_t n);
/* Free both arrays of a summary (spent/added) up to their counts. */
void free_delta(struct delta_summary *s);

/* Compute the transparent UTXO delta of one block, capturing the full
 * pre-image of each spent coin (value/height/is_coinbase/script) so a
 * later disconnect can emit a correct restore-ADD. Unresolved prevouts
 * are looked up via `lookup` (NULL → all external coins are "absent").
 * On success `out->spent` / `out->added` own the arrays (caller emits
 * then free_delta()s); on any failure `out` is freed and ok==false with
 * a status/failure_kind set. `block_height` is this block's height (used
 * as the restore height for an intra-block create-then-spend coin). */
void utxo_apply_compute_block_delta(const struct block *blk,
                                    uint32_t block_height,
                                    utxo_apply_lookup_fn lookup,
                                    void *lookup_user,
                                    struct delta_summary *out);

/* Ensure the durable per-block inverse-delta table exists. */
bool utxo_apply_ensure_delta_schema(sqlite3 *db);

/* Persist the per-block inverse-delta for `height`, stamped with the
 * OLD branch hash. MUST run inside the stage txn so it lands atomically
 * with the log row + cursor. Called only on a successful apply. */
bool utxo_apply_delta_persist(sqlite3 *db, int height,
                              const struct uint256 *branch_hash,
                              const struct delta_summary *s);

/* The stage-side disconnect path. Detects an active-chain reorg below
 * the persisted cursor, walks down to the fork point, emits inverse
 * UTXO events for the abandoned blocks (when author==STAGE), deletes the
 * now-invalid log+delta rows, and rewinds the cursor to the fork
 * boundary — bounded by ZCL_FINALITY_DEPTH. Self-contained transaction
 * for the row deletes + cursor write. On any unwind, *unwound_counter is
 * incremented and *last_blocked_unix is stamped. Returns false on error
 * (the caller surfaces JOB_FATAL); cursor untouched on error. */
bool utxo_apply_reorg_unwind_if_needed(sqlite3 *db,
                                       struct stage *stage,
                                       struct main_state *ms,
                                       _Atomic uint64_t *unwound_counter,
                                       _Atomic int64_t *last_blocked_unix);

#endif /* ZCL_JOBS_UTXO_APPLY_DELTA_H */
