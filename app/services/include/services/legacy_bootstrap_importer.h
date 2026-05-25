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
struct coins_view_sqlite;

struct legacy_bootstrap_chainstate_import_result {
    int64_t inserted;
    int64_t records;
    bool got_best_block;
    struct uint256 best_block;
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

#endif /* ZCL_SERVICES_LEGACY_BOOTSTRAP_IMPORTER_H */
