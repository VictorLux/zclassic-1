/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * legacy_direct_import — fast-sync from a sibling zclassicd's on-disk
 * data without JSON-RPC.
 *
 * Reads zclassicd's `blocks/index/` LevelDB to build a height-ordered
 * map, mmap()'s the `blk*.dat` files, and drives `process_new_block`
 * with zero-copy payload slices. Bypasses HTTP, JSON, hex decode,
 * loopback TCP — the whole RPC stack.
 *
 * Requires the legacy LevelDB to be unlocked (zclassicd stopped, or a
 * pre-snapshot of the directory). On lock contention, the open fails
 * and the caller should fall back to RPC-based legacy_body_pull.
 *
 * Companion to app/services/src/legacy_body_pull.c (RPC fallback).
 */

#ifndef ZCL_SERVICES_LEGACY_DIRECT_IMPORT_H
#define ZCL_SERVICES_LEGACY_DIRECT_IMPORT_H

#include <stdbool.h>

struct main_state;
struct coins_view_cache;
struct chain_params;
struct wallet;

struct ldi_result {
    int  applied;           /* blocks actually accepted by process_new_block */
    int  skipped_have_data; /* already on disk */
    int  skipped_failed;    /* BLOCK_FAILED_MASK set (rare after 3.0 clear) */
    int  final_tip;         /* active_chain_height at exit */
    int  legacy_tip;        /* max_height observed in legacy blocks/index */
    bool trust_armed;       /* SHA3 spot-check passed; g_assume_valid_height bumped */
    bool ok;                /* true iff loop finished without abort */
};

/* Walk `[from_height+1 .. legacy_tip]` reading payloads directly from
 * `<legacy_datadir>/blocks/blk*.dat`, ingesting via process_new_block.
 * Spot-checks K=3 SHA3 windows before arming trust-mode.
 *
 * `from_height == -1` means "start from active_chain_height + 1".
 *
 * `wallet` may be NULL — if non-NULL, a single-pass wallet_rescan over
 * the imported range runs at the end (since wallet sync was deferred
 * during the trust-mode pull).
 *
 * Returns true on success (ok). Always populates *out if non-NULL. */
bool legacy_direct_import_range_blocking(
    struct main_state *ms,
    struct coins_view_cache *coins_tip,
    const struct chain_params *params,
    struct wallet *wallet,
    const char *our_datadir,
    const char *legacy_datadir,
    int from_height,
    struct ldi_result *out);

#endif /* ZCL_SERVICES_LEGACY_DIRECT_IMPORT_H */
