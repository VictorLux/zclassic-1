/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Local chain ingest — bulk import from a co-located zclassicd's
 * immutable data dir, validated against the static SHA3 anchors.
 *
 * Combines three options from /home/rhett/.claude/plans/make-a-
 * full-detailed-nifty-melody.md:
 *
 *   Option A — read raw blk*.dat from <legacy_datadir>/blocks/ and
 *              apply each block via chain_advance() so the at-tip
 *              ordering invariant holds for every ingested block.
 *
 *   Option B — read <legacy_datadir>/chainstate/ via
 *              chainstate_legacy_reader and bulk-write the UTXO set
 *              at the static SHA3 anchor height in a single
 *              coins_view_sqlite_batch_write_ex transaction.
 *
 *   Option E — SHA3-window verify the raw block payload sequence
 *              against the static g_sha3_windows[] table (when
 *              populated) before any state mutation, so a tampered
 *              local source is rejected up front.
 *
 * Security model: the only local evidence roots are the hardcoded SHA3 UTXO
 * checkpoint in lib/chain/src/checkpoints.c and (when populated) the
 * SHA3 window table in lib/chain/src/sha3_windows.c.  zclassicd is
 * treated as a content store, not as a consensus authority.
 */

#ifndef ZCL_SERVICES_LOCAL_CHAIN_INGEST_H
#define ZCL_SERVICES_LOCAL_CHAIN_INGEST_H

#include <stdbool.h>
#include <stdint.h>

struct main_state;
struct coins_view_cache;
struct chain_params;
struct json_value;

enum local_ingest_result {
    LCI_OK = 0,
    LCI_SOURCE_MISSING,       /* legacy_datadir or its blocks/ does not exist */
    LCI_SHA3_WINDOW_MISMATCH, /* block payload prefix doesn't match static table */
    LCI_CHAINSTATE_MISMATCH,  /* imported UTXO set fails SHA3 anchor verify */
    LCI_ABORTED,              /* signal or operator stop */
    LCI_INTERNAL_ERROR,
    LCI_NUM_RESULTS
};

const char *local_ingest_result_name(enum local_ingest_result r);

/* For zcl_state subsystem=local_ingest. `out` must be json_set_object'd
 * by the caller (matches the *_dump_state_json convention in CLAUDE.md).
 * `key` is unused; pass NULL. */
bool local_chain_ingest_dump_state_json(struct json_value *out,
                                         const char *key);

/* Lightweight detector — does <path>/blocks/blk00000.dat exist? */
bool local_chain_ingest_detect_legacy_datadir(const char *path);

/* T3.3: true iff the SHA3 window evidence prefix was fully verified this
 * boot — either by a fresh phase-1 scan or via a valid evidence
 * cookie. Heights ≤ evidence_prefix_end_height are then known to bit-
 * for-bit match the compile-time anchor and don't need expensive
 * Equihash/Sapling/sig reverify in bg-validation. */
bool local_chain_ingest_evidence_prefix_verified(void);

/* T3.3: max height covered by the compile-time SHA3 window table.
 * Returns -1 when no static windows are compiled in. */
int  local_chain_ingest_evidence_prefix_end_height(void);

#endif /* ZCL_SERVICES_LOCAL_CHAIN_INGEST_H */
