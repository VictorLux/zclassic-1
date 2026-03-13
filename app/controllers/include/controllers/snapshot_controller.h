/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_SNAPSHOT_CONTROLLER_H
#define ZCL_SNAPSHOT_CONTROLLER_H

#include "models/database.h"
#include <stdbool.h>

struct wallet;

/* Create a snapshot of a legacy C++ node's data directory.
 * Hard-links blk*.dat/rev*.dat (instant), copies LevelDB dirs.
 * Stores in c23_datadir/snapshots/YYYYMMDD_HHMMSS/.
 * Rotates old snapshots, keeping max_keep most recent.
 * Returns path to new snapshot dir, or NULL on error. */
const char *snapshot_create(const char *legacy_datadir,
                            const char *c23_datadir,
                            int max_keep);

/* Import a snapshot into the C23 node in parallel.
 * Reads block index + chainstate LevelDB from snapshot,
 * imports into SQLite ActiveRecord database.
 * Also scans block files for wallet transactions.
 *
 * Three parallel threads, each with own SQLite connection:
 *   T1: block index LevelDB → blocks table
 *   T2: chainstate LevelDB  → utxos table
 *   T3: wallet scan          → wallet_* tables
 *
 * After import, copies block files + LevelDB to c23_datadir
 * for ongoing consensus operation.
 *
 * Returns 0 on success, -1 on error. */
int snapshot_import(const char *snapshot_dir,
                    const char *c23_datadir,
                    struct node_db *ndb,
                    struct wallet *w);

/* Build transaction index from block files in background.
 * Reads block positions from blocks table, parses block files
 * to extract txids, inserts into transactions table.
 * Runs as detached thread. */
void snapshot_build_tx_index_bg(const char *c23_datadir);

#endif
