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
struct blocks_mmap;
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

struct legacy_bootstrap_import_result {
    int32_t legacy_tip;
    int64_t block_index_writes;
    int64_t utxos_imported;
    int64_t blk_files_linked;
    int applied;
    int skipped_have_data;
    int skipped_failed;
    int final_tip;
    int outcome;
    int64_t stages_stamped;
    double total_secs;
    bool evidence_armed;
    bool source_checked;
    bool ok;
};

struct lci_cold_result {
    int legacy_tip;
    int64_t block_index_writes;
    int64_t utxos_imported;
    int64_t blk_files_linked;
    double total_secs;
    bool evidence_armed;
    bool ok;
};

struct ldi_result {
    int applied;
    int skipped_have_data;
    int skipped_failed;
    int final_tip;
    int legacy_tip;
    bool source_checked;
    bool ok;
};

enum loi_outcome {
    LOI_OUTCOME_DID_IMPORT = 0,
    LOI_OUTCOME_NOOP_SAME_TIP = 1,
    LOI_OUTCOME_RECOVERED_FROM_CRASH = 2,
    LOI_OUTCOME_REFUSED_HAS_STATE = 3,
    LOI_OUTCOME_LEGACY_NOT_FOUND = 4,
    LOI_OUTCOME_FAILED = 5,
};

struct loi_result {
    enum loi_outcome outcome;
    int32_t legacy_tip_height;
    int64_t block_index_writes;
    int64_t utxos_imported;
    int64_t blk_files_linked;
    int64_t stages_stamped;
    double total_secs;
    bool evidence_armed;
    bool ok;
};

struct legacy_bootstrap_chainstate_import_result {
    int64_t inserted;
    int64_t records;
    bool got_best_block;
    struct uint256 best_block;
};

struct legacy_bootstrap_height_map_result {
    struct legacy_block_loc *map;
    size_t map_count;
    int32_t tip_height;
};

struct legacy_bootstrap_snapshot_import_options {
    const char *legacy_blocks_dir;
    const char *our_blocks_dir;
    const char *legacy_index_dir;
    const char *chainstate_dir;
    struct block_tree_db *btdb;
    struct coins_view_sqlite *cvs;
    struct node_db *ndb;
    size_t chainstate_batch_limit;
    int32_t min_legacy_tip;
    bool require_best_block;
    const char *block_index_long_op_name;
    const char *chainstate_long_op_name;
    const char *log_prefix;
};

struct legacy_bootstrap_snapshot_import_result {
    int64_t blk_files_linked;
    int64_t block_index_writes;
    int64_t utxos_imported;
    int64_t chainstate_records;
    bool got_best_block;
    struct uint256 best_block;
    struct uint256 legacy_tip_hash;
    int32_t legacy_tip_height;
};

struct legacy_bootstrap_block_source_options {
    const char *legacy_blocks_dir;
    const struct legacy_block_loc *map;
    size_t map_count;
    int legacy_tip;
    int spotcheck_k;
    bool require_spotcheck;
    const char *log_prefix;
    const char *debug_env;
    bool dump_map_on_failure;
};

struct legacy_bootstrap_block_source {
    struct blocks_mmap *bmr;
    bool source_checked;
};

/* Hardlink every blk*.dat from legacy_blocks_dir into our_blocks_dir, with
 * a byte-for-byte copy fallback on EXDEV/EPERM. Existing destination files are
 * skipped. Returns linked+copied count, or -1 after logging a fatal error. */
int64_t legacy_bootstrap_link_blk_files(const char *legacy_blocks_dir,
                                        const char *our_blocks_dir,
                                        const char *log_prefix);

/* Create <datadir>/<stage_subdir> with owner-only permissions and write the
 * full path into out_stage_dir. Returns false after logging or for path
 * truncation. */
bool legacy_bootstrap_make_stage_dir(const char *datadir,
                                     const char *stage_subdir,
                                     char *out_stage_dir,
                                     size_t out_cap,
                                     const char *log_prefix);

/* Snapshot legacy blocks/index and chainstate into stage_dir, retrying
 * manifest_changed races. On a chainstate failure, the blocks/index snapshot is
 * torn down before returning false. */
bool legacy_bootstrap_snapshot_leveldbs(const char *legacy_datadir,
                                        const char *stage_dir,
                                        char *out_idx_path,
                                        size_t idx_cap,
                                        char *out_cs_path,
                                        size_t cs_cap,
                                        const char *log_prefix);

/* Copy HAVE_DATA, non-failed block-index records from a legacy/snapshot
 * blocks/index LevelDB into our block_tree_db. Returns records written, or -1
 * after logging a fatal error. */
int64_t legacy_bootstrap_copy_block_index(const char *legacy_index_dir,
                                          struct block_tree_db *our_btdb,
                                          struct uint256 *out_tip_hash,
                                          int32_t *out_tip_height,
                                          const char *long_op_name,
                                          const char *log_prefix);

/* Load the legacy blocks/index height map, optionally restricted to the branch
 * ending at tip_filter. The caller owns out->map and frees it with
 * bilr_free_height_map().
 */
bool legacy_bootstrap_load_height_map(
    const char *legacy_index_dir,
    const struct uint256 *tip_filter,
    const char *log_prefix,
    struct legacy_bootstrap_height_map_result *out);

/* Bulk-import UTXOs from a legacy/snapshot chainstate LevelDB into coins.db.
 * batch_limit selects the transaction size; long_op_name may be NULL. Returns
 * false after logging if open, allocation, iteration, or bulk insertion fails.
 */
bool legacy_bootstrap_import_chainstate_utxos(
    const char *chainstate_dir,
    struct coins_view_sqlite *cvs,
    size_t batch_limit,
    const char *long_op_name,
    const char *log_prefix,
    struct legacy_bootstrap_chainstate_import_result *out);

/* Persist the pending CSR anchor consumed by boot's cold-import resolver. */
bool legacy_bootstrap_record_pending_csr_anchor(
    struct node_db *ndb,
    const struct uint256 *best_block,
    int32_t best_height,
    int64_t utxo_count,
    const char *log_prefix);

/* Shared imported-snapshot body for cold import and legacy-attach. The caller
 * owns snapshot creation/destruction and any mode-specific preflight or final
 * cursor stamping.
 */
bool legacy_bootstrap_import_snapshot_state(
    const struct legacy_bootstrap_snapshot_import_options *opts,
    struct legacy_bootstrap_snapshot_import_result *out);

/* Canonical mode-driven bootstrap importer. Public legacy CLI services remain
 * thin compatibility wrappers around this function.
 */
bool legacy_bootstrap_import_blocking(
    const struct legacy_bootstrap_import_options *opts,
    struct legacy_bootstrap_import_result *out);

bool legacy_cold_import_blocking(
    struct main_state *ms,
    struct coins_view_sqlite *cvs,
    struct node_db *ndb,
    struct block_tree_db *btdb,
    const char *our_datadir,
    const char *legacy_datadir,
    struct lci_cold_result *out);

bool legacy_direct_import_range_blocking(
    struct main_state *ms,
    struct coins_view_cache *coins_tip,
    const struct chain_params *params,
    struct wallet *wallet,
    const char *our_datadir,
    const char *legacy_datadir,
    int from_height,
    struct ldi_result *out);

bool legacy_oneshot_import_run(
    const char *our_datadir,
    const char *legacy_datadir,
    struct main_state *ms,
    struct coins_view_sqlite *cvs,
    struct node_db *ndb,
    struct block_tree_db *btdb,
    struct loi_result *out);

const char *loi_outcome_name(enum loi_outcome o);

size_t loi_stages_to_stamp_count(void);
const char *loi_stages_to_stamp_at(size_t i);

/* Open legacy blk*.dat through the mmap reader and apply the mode's SHA3
 * spotcheck policy. On success, caller must close out->bmr with
 * legacy_bootstrap_close_block_source().
 */
bool legacy_bootstrap_open_block_source(
    const struct legacy_bootstrap_block_source_options *opts,
    struct legacy_bootstrap_block_source *out);

void legacy_bootstrap_close_block_source(
    struct legacy_bootstrap_block_source *src);

/* Verify k random compile-time SHA3 windows against block payloads served by
 * bmr/map. debug_env may name an environment variable that forces one window
 * before random picks. dump_map_on_failure enables heavier parent-link
 * diagnostics for cold-import aborts.
 */
bool legacy_bootstrap_spotcheck_sha3_windows(
    struct blocks_mmap *bmr,
    const struct legacy_block_loc *map,
    size_t map_count,
    int legacy_tip,
    int k,
    const char *log_prefix,
    const char *debug_env,
    bool dump_map_on_failure);

#endif /* ZCL_SERVICES_LEGACY_BOOTSTRAP_IMPORTER_H */
