/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * blocks_index_legacy_reader: read a Bitcoin Core / zclassicd
 * `blocks/index/` LevelDB and produce a height-indexed array of
 * (hash, file, datapos, ...) for every block-index record.
 *
 * Feeds the direct-import fast-sync path (see
 * app/services/src/legacy_direct_import.c): once we know each height's
 * on-disk location in zclassicd's blk*.dat files, we mmap and ingest
 * with zero JSON-RPC overhead.
 *
 * Open is exclusive; if zclassicd holds the LevelDB LOCK, open fails.
 * Operator must briefly stop zclassicd, or pre-snapshot the directory.
 */

#ifndef ZCL_STORAGE_BLOCKS_INDEX_LEGACY_READER_H
#define ZCL_STORAGE_BLOCKS_INDEX_LEGACY_READER_H

#include "core/uint256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct legacy_block_loc {
    struct uint256 hash;
    int32_t  height;       /* set; -1 means slot empty (no block at h) */
    int32_t  nFile;
    uint32_t nDataPos;
    uint32_t nUndoPos;
    uint32_t nStatus;
};

struct bilr;

/* Open zclassicd's blocks/index/ LevelDB. On success returns true and
 * stores an opaque handle in *out. On failure (most commonly: LOCK
 * held by a running zclassicd) returns false; caller logs.
 * `blocks_index_dir` is the .../blocks/index path. */
bool bilr_open(const char *blocks_index_dir, struct bilr **out);

void bilr_close(struct bilr *r);

/* Iterate every 'b'-prefixed record once. For each height, keep the
 * entry with `BLOCK_HAVE_DATA && !BLOCK_FAILED_MASK` (i.e. the active
 * chain entry). On return: *out_array is a height-indexed array of
 * length *out_count (= max_height + 1). Slots without a usable entry
 * have `height = -1`. Caller frees with `bilr_free_height_map`.
 * Returns false on iterator / deserialization failure. */
bool bilr_load_height_map(struct bilr *r,
                          struct legacy_block_loc **out_array,
                          size_t *out_count);

void bilr_free_height_map(struct legacy_block_loc *array);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_STORAGE_BLOCKS_INDEX_LEGACY_READER_H */
