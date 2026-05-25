/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Shared legacy bootstrap import primitives.
 *
 * These helpers are the first consolidation point for the three
 * zclassicd-backed bootstrap modes. CLI entry points keep their existing
 * contracts while common filesystem policy moves here. */

#ifndef ZCL_SERVICES_LEGACY_BOOTSTRAP_IMPORTER_H
#define ZCL_SERVICES_LEGACY_BOOTSTRAP_IMPORTER_H

#include "core/uint256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct block_tree_db;
struct chain_params;
struct coins_view_cache;
struct coins_view_sqlite;
struct legacy_block_loc;
struct main_state;
struct node_db;
struct wallet;

enum legacy_bootstrap_import_mode {
    LEGACY_BOOTSTRAP_IMPORT_COLD = 0,
    LEGACY_BOOTSTRAP_IMPORT_DIRECT = 1,
    LEGACY_BOOTSTRAP_IMPORT_ATTACH = 2,
};

struct legacy_bootstrap_import_options {
    enum legacy_bootstrap_import_mode mode;
    struct main_state *ms;
    struct coins_view_sqlite *cvs;
    struct node_db *ndb;
    struct block_tree_db *btdb;
    struct coins_view_cache *coins_tip;
    const struct chain_params *params;
    struct wallet *wallet;
    const char *our_datadir;
    const char *legacy_datadir;
    int from_height;
};

enum loi_outcome {
    LOI_OUTCOME_DID_IMPORT = 0,
    LOI_OUTCOME_NOOP_SAME_TIP = 1,
    LOI_OUTCOME_RECOVERED_FROM_CRASH = 2,
    LOI_OUTCOME_REFUSED_HAS_STATE = 3,
    LOI_OUTCOME_LEGACY_NOT_FOUND = 4,
    LOI_OUTCOME_FAILED = 5,
};

struct legacy_bootstrap_import_result {
    int32_t legacy_tip;
    int64_t block_index_writes;
    int64_t utxos_imported;
    int64_t blk_files_linked;
    int applied;
    int skipped_have_data;
    int skipped_failed;
    int final_tip;
    enum loi_outcome outcome;
    int64_t stages_stamped;
    double total_secs;
    bool evidence_armed;
    bool source_checked;
    bool ok;
};

struct legacy_bootstrap_height_map_result {
    struct legacy_block_loc *map;
    size_t map_count;
    int32_t tip_height;
};

/* Load the legacy blocks/index height map, optionally restricted to the branch
 * ending at tip_filter. The caller owns out->map and frees it with
 * bilr_free_height_map().
 */
bool legacy_bootstrap_load_height_map(
    const char *legacy_index_dir,
    const struct uint256 *tip_filter,
    const char *log_prefix,
    struct legacy_bootstrap_height_map_result *out);

/* Canonical mode-driven bootstrap importer for -cold-import, -fastimport, and
 * -legacy-attach.
 */
bool legacy_bootstrap_import_blocking(
    const struct legacy_bootstrap_import_options *opts,
    struct legacy_bootstrap_import_result *out);

const char *loi_outcome_name(enum loi_outcome o);

size_t loi_stages_to_stamp_count(void);
const char *loi_stages_to_stamp_at(size_t i);

#endif /* ZCL_SERVICES_LEGACY_BOOTSTRAP_IMPORTER_H */
