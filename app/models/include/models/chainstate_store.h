/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ChainstateStore model — validates and copies chainstate LevelDB.
 *
 * ActiveRecord pattern:
 *   struct chainstate_store cs = { .src_dir = "...", .dst_dir = "..." };
 *   struct ar_errors errors;
 *   ar_errors_clear(&errors);
 *   if (chainstate_store_validate(&cs, &errors))
 *       chainstate_store_save(&cs);
 */

#ifndef ZCL_MODELS_CHAINSTATE_STORE_H
#define ZCL_MODELS_CHAINSTATE_STORE_H

#include "models/activerecord.h"
#include <stdbool.h>
#include <stdint.h>

struct chainstate_store {
    const char *src_dir;    /* Source chainstate/ directory */
    const char *dst_dir;    /* Destination chainstate/ directory */

    /* Populated by validate */
    int num_sst_files;      /* .ldb / .sst file count */
    int64_t total_bytes;
    bool has_manifest;
    bool has_current;

    /* Populated by save */
    bool copy_ok;
};

/* validates_presence_of :src_dir, :dst_dir, :has_manifest, :has_current
 * validates :total_bytes, numericality: { greater_than: 0 } */
bool chainstate_store_validate(struct chainstate_store *cs,
                                struct ar_errors *errors);

/* save: clean copy (rm -rf dst, cp -a src dst) — LevelDB needs consistent state */
bool chainstate_store_save(struct chainstate_store *cs);

/* Summary string */
void chainstate_store_summary(const struct chainstate_store *cs,
                               char *out, size_t len);

#endif
