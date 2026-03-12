/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_DB_MODEL_UTXO_H
#define ZCL_DB_MODEL_UTXO_H

#include "db/db.h"
#include "db/activerecord.h"
#include <stdbool.h>
#include <stdint.h>

enum script_type {
    SCRIPT_P2PKH = 0,
    SCRIPT_P2SH  = 1,
    SCRIPT_OP_RETURN = 2,
    SCRIPT_MULTISIG = 3,
    SCRIPT_OTHER = 255
};

struct db_utxo {
    uint8_t txid[32];
    uint32_t vout;
    int64_t value;
    uint8_t *script;
    size_t script_len;
    enum script_type script_type;
    uint8_t address_hash[20];
    bool has_address;
    int height;
    bool is_coinbase;
};

/* Callbacks and validation */
struct ar_callbacks *db_utxo_callbacks(void);
bool db_utxo_validate(const struct db_utxo *u, struct ar_errors *errors);

bool db_utxo_save(struct node_db *ndb, const struct db_utxo *u);
bool db_utxo_find(struct node_db *ndb, const uint8_t txid[32], uint32_t vout,
                  struct db_utxo *out);
bool db_utxo_exists(struct node_db *ndb, const uint8_t txid[32], uint32_t vout);
bool db_utxo_delete(struct node_db *ndb, const uint8_t txid[32], uint32_t vout);

/* Sum all UTXO values for an address hash. */
int64_t db_utxo_balance_for_address(struct node_db *ndb,
                                     const uint8_t address_hash[20]);

/* List UTXOs for an address. Returns count, fills array up to max. */
int db_utxo_list_for_address(struct node_db *ndb,
                             const uint8_t address_hash[20],
                             struct db_utxo *out, size_t max);

/* Count total UTXOs in the set. */
int64_t db_utxo_count(struct node_db *ndb);

/* ── Relationships ─────────────────────────────────────────────── */

/* belongs_to :transaction — find the tx that created this UTXO */
struct db_tx_index;
bool db_utxo_transaction(struct node_db *ndb, const struct db_utxo *u,
                         struct db_tx_index *out);

#endif
