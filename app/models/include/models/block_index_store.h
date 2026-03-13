/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * BlockIndexStore model — validates and copies block index LevelDB.
 *
 * ActiveRecord pattern:
 *   struct block_index_store idx = { .src_dir = "...", .dst_dir = "..." };
 *   struct ar_errors errors;
 *   ar_errors_clear(&errors);
 *   if (block_index_store_validate(&idx, &errors))
 *       block_index_store_save(&idx);
 */

#ifndef ZCL_MODELS_BLOCK_INDEX_STORE_H
#define ZCL_MODELS_BLOCK_INDEX_STORE_H

#include "models/activerecord.h"
#include <stdbool.h>
#include <stdint.h>

struct block_index_store {
    const char *src_dir;    /* Source blocks/index/ directory */
    const char *dst_dir;    /* Destination blocks/index/ directory */

    /* Populated by validate */
    int num_sst_files;      /* .ldb / .sst file count */
    int64_t total_bytes;
    bool has_manifest;      /* MANIFEST file present */
    bool has_current;       /* CURRENT file present */

    /* Populated by save */
    bool copy_ok;
};

/* validates_presence_of :src_dir, :dst_dir, :has_manifest, :has_current
 * validates :num_sst_files, numericality: { greater_than: 0 } */
bool block_index_store_validate(struct block_index_store *idx,
                                 struct ar_errors *errors);

/* save: clean copy (rm -rf dst, cp -a src dst) — LevelDB needs consistent state */
bool block_index_store_save(struct block_index_store *idx);

/* Summary string */
void block_index_store_summary(const struct block_index_store *idx,
                                char *out, size_t len);

#endif
