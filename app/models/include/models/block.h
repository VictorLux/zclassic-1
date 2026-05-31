/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_DB_MODEL_BLOCK_H
#define ZCL_DB_MODEL_BLOCK_H

#include "models/database.h"
#include "models/activerecord.h"
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
bool db_block_save_canonical(struct node_db *ndb, const struct db_block *b);
bool db_block_find_by_hash(struct node_db *ndb, const uint8_t hash[32],
                           struct db_block *out);
bool db_block_find_by_height(struct node_db *ndb, int height,
                             struct db_block *out);

/* Load just the Equihash solution BLOB for a connected (status>=3) block
 * at `height` into `out` (up to `max` bytes), setting *out_len. The full
 * read helpers above deliberately drop the solution bytes (only the
 * length is recorded); this is the narrow accessor that materialises the
 * real solution for header re-validation.
 *
 * Returns false on: closed db, no matching connected row, an empty/NULL
 * solution column, or a solution larger than `max`. A false return MUST
 * be treated as "no usable solution" — callers must NOT fabricate or skip
 * Equihash verification on false. */
bool db_block_load_solution_by_height(struct node_db *ndb, int height,
                                      unsigned char *out, size_t *out_len,
                                      size_t max);

bool db_block_delete(struct node_db *ndb, const uint8_t hash[32]);
int db_block_max_height(struct node_db *ndb);
/* Max stored block height regardless of block status (no status>=3 filter).
 * The block explorer uses this for its native chain-height fallback when no
 * main_state is attached (e.g. the standalone GTK browser): it wants the
 * highest height present in the `blocks` table irrespective of validation
 * status. Returns -1 when the db is closed or the table is empty, so callers
 * can treat "< 1" as "no chain". */
int db_block_max_height_any_status(struct node_db *ndb);
int db_block_count(struct node_db *ndb);

/* Connected-tip height + block time in one read (status>=3), each
 * COALESCE'd to 0 so an empty chain yields 0/0 rather than NULL. Used by
 * the API wallet panel for confirmation/now display. Returns false only
 * when the db is closed; *_out are zeroed on every non-row path. */
bool db_block_tip_height_and_time(struct node_db *ndb,
                                  int64_t *height_out, int64_t *time_out);

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

/* scope :hashes_in_range — block hashes for a height range (ASC order).
 * Returns count of hashes written to hashes_out (up to max). */
int db_block_hashes_in_range(struct node_db *ndb,
                             int start_height, int end_height,
                             uint8_t (*hashes_out)[32], size_t max);

#endif
