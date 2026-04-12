/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Block Index Loader — read/write block_index.bin flat file, SQLite cache,
 * and LevelDB block tree.
 *
 * Background
 * ----------
 * Extracted from boot_index.c (boot decomposition Phase A) to isolate
 * block-index I/O behind a clean service boundary. The three load paths
 * (flat, SQLite, LevelDB) are tried in order during boot: flat is O(1)
 * via mmap, SQLite is seconds, LevelDB is 10-15s. The first to succeed
 * wins.
 *
 * On-disk flat format (block_index.bin)
 * -------------------------------------
 *   [4B magic "ZCLI" = 0x5A434C49]
 *   [4B count (LE)]
 *   [count * 192B block_index_flat entries, height-sorted]
 *
 * Each entry is a packed struct containing hash, prev_hash, height,
 * PoW metadata, chain_work, and Sapling root.
 *
 * Integrity is verified by the sibling block_index_integrity service
 * (bii_verify) using a SHA3-256 sidecar file.
 */

#ifndef ZCL_SERVICES_BLOCK_INDEX_LOADER_H
#define ZCL_SERVICES_BLOCK_INDEX_LOADER_H

#include <stdbool.h>
#include <stdint.h>

/* Forward declarations — avoids pulling in heavy headers */
struct main_state;
struct node_db;
struct chain_params;
struct block_tree_db;

/* ── Flat file (block_index.bin) ─────────────────────────── */

/* Save all block_index entries to a height-sorted flat file.
 * Creates <datadir>/block_index.bin.  Overwrites if present. */
void save_block_index_flat(const char *datadir, struct main_state *ms);

/* Load block_index.bin via mmap.  Returns true if >= 1 entry loaded.
 * Allocates a contiguous arena for all entries, links pprev by hash,
 * recomputes nChainWork/nChainTx from pprev chain. */
bool load_block_index_flat(const char *datadir, struct main_state *ms);

/* ── SQLite cache (block_index_cache table) ──────────────── */

/* Write all block_index entries to the block_index_cache table.
 * Commits in 50K-row batches for write throughput. */
void save_block_index_recent(struct node_db *ndb, struct main_state *ms);

/* Load block_index from the block_index_cache table.
 * Returns true if >= 1 entry loaded.  Requires >= 1000 cached rows. */
bool load_block_index_sqlite(struct node_db *ndb, struct main_state *ms);

/* ── LevelDB block tree ──────────────────────────────────── */

/* Load block_index from LevelDB block tree database.
 * Post-processes: recomputes nChainWork, nChainTx, skip links,
 * branch IDs, and failed-child propagation.
 * If btdb_open is false and no entries exist, inserts genesis. */
bool load_block_index(struct main_state *ms,
                       const struct chain_params *params,
                       struct block_tree_db *btdb, bool btdb_open);

#endif /* ZCL_SERVICES_BLOCK_INDEX_LOADER_H */
