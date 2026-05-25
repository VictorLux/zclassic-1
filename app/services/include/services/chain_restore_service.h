/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Chain Restore Service — deterministic chain tip restoration.
 *
 * After importing UTXOs (from LDB, snapshot, or crash recovery), the
 * coins_best_block hash may not match any block in our block index.
 * This service resolves the gap by either:
 *   (a) finding the hash in the block map and setting it as tip, or
 *   (b) recording a non-consensus placeholder anchor at the correct height.
 *
 * Architecture: planning pattern (pure functions) + execution.
 *   1. chain_restore_plan()    — pure: decides what to do
 *   2. chain_restore_execute() — applies the plan to mutable state
 *   3. chain_restore_validate()— verifies post-conditions
 *
 * Replaces inline anchor creation in boot.c (3 copies → 1 service). */

#ifndef ZCL_CHAIN_RESTORE_SERVICE_H
#define ZCL_CHAIN_RESTORE_SERVICE_H

#include "platform/time_compat.h"
#include "core/uint256.h"
#include "core/arith_uint256.h"
#include "services/chain_restore_boot_activation.h"
#include "services/chain_restore_boot_snapshot.h"
#include "services/chain_restore_executor.h"
#include "services/chain_restore_integrity.h"
#include "services/chain_restore_repair.h"
#include <stdbool.h>
#include <stdint.h>

struct main_state;
struct block_index;
struct coins_view_cache;

/* ── State machine ─────────────────────────────────────────────── */

enum chain_restore_state {
    CHAIN_RESTORE_UNRESOLVED = 0,   /* coins_best_block not evaluated */
    CHAIN_RESTORE_FOUND_IN_INDEX,   /* hash found in block_map */
    CHAIN_RESTORE_ANCHOR_CREATED,   /* placeholder anchor inserted */
    CHAIN_RESTORE_RESOLVED,         /* chain tip set, ready for sync */
    CHAIN_RESTORE_FAILED,           /* unrecoverable: no hash/height */
};

enum chain_restore_source {
    CHAIN_RESTORE_SRC_NORMAL_BOOT = 0,
    CHAIN_RESTORE_SRC_LDB_IMPORT,
    CHAIN_RESTORE_SRC_SNAPSHOT,
};

/* ── Planning (pure, no side effects) ──────────────────────────── */

struct chain_restore_input {
    struct uint256 coins_best_hash;     /* from LDB or coins_view_cache */
    int            utxo_max_height;     /* SELECT MAX(height) FROM utxos */
    bool           hash_found_in_map;   /* block_map_find returned non-NULL */
    int            found_height;        /* height of found block (if any) */
    bool           found_has_pprev;     /* found->pprev != NULL */
    enum chain_restore_source source;
};

struct chain_restore_plan {
    enum chain_restore_state next_state;
    bool should_create_anchor;
    bool should_set_chain_tip;
    bool should_set_best_header;
    bool should_set_snapshot_anchor;
    bool should_skip_activate;
    int  anchor_height;
    struct uint256 anchor_hash;
    char reason[128];
};

#include "services/chain_restore_planner.h"

#endif
