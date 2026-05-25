/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Shared legacy bootstrap import primitives.
 *
 * These helpers are the first consolidation point for the three
 * zclassicd-backed bootstrap modes. CLI entry points keep their existing
 * contracts while common filesystem policy moves here. */

#ifndef ZCL_SERVICES_LEGACY_BOOTSTRAP_IMPORTER_H
#define ZCL_SERVICES_LEGACY_BOOTSTRAP_IMPORTER_H

#include <stdint.h>

struct block_tree_db;
struct uint256;

/* Hardlink every blk*.dat from legacy_blocks_dir into our_blocks_dir, with
 * a byte-for-byte copy fallback on EXDEV/EPERM. Existing destination files are
 * skipped. Returns linked+copied count, or -1 after logging a fatal error. */
int64_t legacy_bootstrap_link_blk_files(const char *legacy_blocks_dir,
                                        const char *our_blocks_dir,
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

#endif /* ZCL_SERVICES_LEGACY_BOOTSTRAP_IMPORTER_H */
