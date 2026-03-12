/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_DB_MODEL_BLOCK_H
#define ZCL_DB_MODEL_BLOCK_H

#include "db/db.h"
#include "db/activerecord.h"
#include <stdbool.h>
#include <stdint.h>

struct db_block {
    uint8_t hash[32];
    int height;
    uint8_t prev_hash[32];
    int32_t version;
    uint8_t merkle_root[32];
    uint32_t time;
    uint32_t bits;
    uint8_t nonce[32];
    uint8_t *solution;
    size_t solution_len;
    uint8_t chain_work[32];
    int status;
    int file_num;
    int data_pos;
    int undo_pos;
    int num_tx;
    uint8_t sapling_root[32];
    uint8_t sprout_root[32];
    int64_t sapling_value;
    int64_t sprout_value;
};

/* Callbacks — register before/after save/destroy hooks */
struct ar_callbacks *db_block_callbacks(void);

/* Validate — runs before save, returns true if valid */
bool db_block_validate(const struct db_block *b, struct ar_errors *errors);

bool db_block_save(struct node_db *ndb, const struct db_block *b);
bool db_block_find_by_hash(struct node_db *ndb, const uint8_t hash[32],
                           struct db_block *out);
bool db_block_find_by_height(struct node_db *ndb, int height,
                             struct db_block *out);
bool db_block_delete(struct node_db *ndb, const uint8_t hash[32]);
int db_block_max_height(struct node_db *ndb);
int db_block_count(struct node_db *ndb);

/* Batch insert for initial block index load. Call within begin/commit. */
bool db_block_save_batch(struct node_db *ndb, const struct db_block *blocks,
                         size_t count);

/* ── Relationships ─────────────────────────────────────────────── */

/* has_many :transactions — find all txids in this block */
struct db_tx_index;
int db_block_transactions(struct node_db *ndb, const uint8_t hash[32],
                          struct db_tx_index *out, size_t max);

/* has_many :utxos — find UTXOs created in this block */
struct db_utxo;
int db_block_utxos(struct node_db *ndb, int height,
                   struct db_utxo *out, size_t max);

/* belongs_to :prev_block — find the parent block */
bool db_block_prev(struct node_db *ndb, const struct db_block *b,
                   struct db_block *out);

/* has_one :next_block — find the block at height+1 */
bool db_block_next(struct node_db *ndb, const struct db_block *b,
                   struct db_block *out);

#endif
