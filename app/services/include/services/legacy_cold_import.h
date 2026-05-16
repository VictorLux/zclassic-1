/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * legacy_cold_import — cold-start to a sibling zclassicd's current tip
 * in ~60 seconds. The breakthrough beyond Phase 3's warm catch-up:
 * never walks blocks. Imports state directly.
 *
 * Pipeline (refusing to run if the local datadir already has
 * substantial state):
 *
 *   1. bilr_open(legacy/blocks/index) + load height map.
 *   2. SHA3 spot-check K=5 random anchor windows from mmap'd payloads.
 *   3. Hardlink legacy/blocks/blk*.dat into our blocks/ (instant).
 *   4. Bulk-write block_index entries from the bilr map into our
 *      LevelDB via WriteBatch (one fsync at end).
 *   5. Bulk-import legacy chainstate UTXOs into our coins.db using
 *      coins_view_sqlite_bulk_insert (5000-record batches).
 *   6. Read legacy chainstate's 'B' key and persist it as a pending
 *      cold-import anchor. Boot resolves that anchor through CSR after
 *      the block index is loaded; this function never publishes a tip.
 *
 * After this runs, the normal boot path takes over:
 *   - block_index_loader reads our LevelDB into the in-memory block_map.
 *   - chain_restore_service walks pprev from best_block_hash, rebuilds
 *     active_chain, sets tip.
 *   - bg_validation kicks in over hours to bit-exact re-verify every
 *     historic block ("process every bit" guarantee).
 *
 * Sapling tree is left empty by this path; the background validator
 * builds it up as it walks from genesis. Wallet shielded operations
 * become available once bg_validation reaches the relevant
 * Sapling-active heights (~10-30 min after boot).
 *
 * Requires: zclassicd stopped (its blocks/index/ LevelDB LOCK must be
 * released), legacy datadir on the same filesystem as our datadir (for
 * hardlinks). Failures are non-fatal at the CLI level — we fall
 * through to the normal sync path.
 */

#ifndef ZCL_SERVICES_LEGACY_COLD_IMPORT_H
#define ZCL_SERVICES_LEGACY_COLD_IMPORT_H

#include <stdbool.h>
#include <stdint.h>

struct main_state;
struct coins_view_sqlite;
struct block_tree_db;
struct node_db;

struct lci_cold_result {
    int      legacy_tip;        /* max height observed in legacy blocks/index */
    int64_t  block_index_writes;
    int64_t  utxos_imported;
    int64_t  blk_files_linked;
    double   total_secs;
    bool     evidence_armed;       /* SHA3 spot-check passed */
    bool     ok;
};

/* Run the cold-import pipeline. Returns true on success.
 *
 * Refuses to run when active_chain in `ms` is non-trivial (height >
 * threshold) — designed for empty-or-near-empty datadirs. */
bool legacy_cold_import_blocking(
    struct main_state *ms,
    struct coins_view_sqlite *cvs,
    struct node_db *ndb,
    struct block_tree_db *btdb,
    const char *our_datadir,
    const char *legacy_datadir,
    struct lci_cold_result *out);

#endif /* ZCL_SERVICES_LEGACY_COLD_IMPORT_H */
