/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Block Index Loader: block_index.bin flat cache, SQLite cache, LevelDB block
 * tree compatibility, and projection-backed boot rebuild.
 *
 * Background
 * ----------
 * This service isolates block-index I/O behind a clean boundary. The flat,
 * SQLite, and LevelDB loaders remain compatibility/fallback paths; the
 * reducer-aligned boot path rebuilds from block_index_projection through
 * load_block_index_from_projection().
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
#include <stddef.h>
#include <stdint.h>

/* Forward declarations — avoids pulling in heavy headers */
struct main_state;
struct node_db;
struct chain_params;
struct block_tree_db;
struct block_index_projection;
struct sqlite3;

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

/* ── Shared internal helpers ─────────────────────────────────────── */

struct block_index;

/* Forward pass over a height-sorted block_index array: recompute
 * nChainWork, nChainTx, skip links, cached branch id, and failed-child
 * propagation from each entry's (already-linked) pprev. Shared by the
 * LevelDB loader (load_block_index) and the projection rebuild so both compute
 * the pointer-graph-derived fields identically. `sorted` must be height-ASC
 * ordered. */
void block_index_forward_pass(struct block_index **sorted, size_t count);

/* ── Projection-backed boot rebuild (event_log -> projection -> map) ───── */

/* Rebuild the in-memory block_index map purely from the
 * block_index_projection (the log-derived authoritative source), then
 * seed the active tip from the tip_finalize cursor in progress.kv.
 *
 * This is the reducer-aligned boot rebuild. It does NOT touch any
 * $HOME/.zclassic path.
 *
 * Sequence:
 *   1. block_index_projection_catch_up(bip) — drain the event log.
 *   2. block_index_projection_iterate — fold every disk_block_index into
 *      ms->map_block_index via chainstate_insert_block_index (fields per
 *      block_index_db.c, OMITTING the +1703 file-0 fixup since the
 *      projection's nDataPos is this node's own body_persist position).
 *   3. Link pprev via the carried hashPrev.
 *   4. Forward pass: recompute nChainWork, nChainTx, skip links, branch
 *      ids, failed-child propagation (mirrors load_block_index post-load).
 *   5. Seed the tip: read the tip_finalize cursor from `progress_db`
 *      (stage_cursor table), look up the finalized tip hash at cursor-1,
 *      find that block_index, set the authoritative tip + publish via
 *      chain_set_active_tip.
 *
 * Empty projection (cold datadir) → no entries folded, no tip set,
 * returns true (the node sits at genesis; fast_sync seeds it).
 *
 * `bip` is the open projection; `progress_db` is the progress.kv handle
 * (progress_store_db()). Both may be NULL — a NULL `bip` makes this a
 * no-op (returns true, empty map); a NULL `progress_db` skips the tip
 * seed (map rebuilt, no tip published).
 *
 * Returns true on success (including the empty case), false on a hard
 * fold/iterate error. */
bool load_block_index_from_projection(struct main_state *ms,
                                      const struct chain_params *params,
                                      struct block_index_projection *bip,
                                      struct sqlite3 *progress_db);

/* FORWARD-ONLY finalized-tip seed for the NORMAL boot path.
 *
 * After the normal loaders establish the active tip from the coins/UTXO
 * authority, this adopts the durable tip_finalize frontier (cursor-1) ONLY
 * when it is a strictly-higher, CONTIGUOUS forward extension of the current
 * chain — every intermediate block HAVE_DATA + script-valid + failure-free,
 * with the pprev walk landing pointer-equal on the current active tip.
 * Otherwise it is a no-op. It never rewinds the tip, never swaps a fork, and
 * never mutates the tip_finalize_log or any cursor (read only), so a
 * sparse/header-only frontier yields a no-op rather than a hole.
 *
 * Returns 1 = seeded forward, 0 = no-op, -1 = error.
 *
 * BUILDING BLOCK — NOT YET WIRED on the normal boot path. A repro-on-copy
 * showed the in-memory active tip is still genesis (height 0) at the boot
 * recovery section (the coins/UTXO authority sets the persisted CSR tip but
 * not the in-memory active_chain there), so the correct call site is AFTER
 * the active tip is established to the coins frontier — to be pinned down by
 * the §3.1 wiring follow-up. Safe to call anywhere: it no-ops unless the
 * finalized frontier is a small, strictly-higher, contiguous extension. */
int block_index_loader_seed_tip_from_finalized(struct main_state *ms,
                                               struct sqlite3 *progress_db);

#endif /* ZCL_SERVICES_BLOCK_INDEX_LOADER_H */
