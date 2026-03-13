/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_DB_MODEL_WALLET_TX_H
#define ZCL_DB_MODEL_WALLET_TX_H

#include "models/database.h"
#include "models/activerecord.h"
#include <stdbool.h>
#include <stdint.h>

struct db_wallet_tx {
    uint8_t txid[32];
    uint8_t *raw_tx;
    size_t raw_tx_len;
    uint8_t block_hash[32];
    bool has_block;
    int block_height;
    int64_t time_received;
    bool from_me;
    int64_t fee;
};

/* Callbacks and validation */
struct ar_callbacks *db_wallet_tx_callbacks(void);
bool db_wallet_tx_validate(const struct db_wallet_tx *t, struct ar_errors *errors);

bool db_wallet_tx_save(struct node_db *ndb, const struct db_wallet_tx *t);
bool db_wallet_tx_find(struct node_db *ndb, const uint8_t txid[32],
                       struct db_wallet_tx *out);
bool db_wallet_tx_delete(struct node_db *ndb, const uint8_t txid[32]);
int db_wallet_tx_count(struct node_db *ndb);
void db_wallet_tx_free(struct db_wallet_tx *t);

/* List recent transactions. Returns count. */
int db_wallet_tx_recent(struct node_db *ndb, struct db_wallet_tx *out,
                        size_t max);

/* List wallet transactions in descending time order with offset paging. */
int db_wallet_tx_list(struct node_db *ndb, struct db_wallet_tx *out,
                      size_t max, size_t offset);

/* List transactions at a given height. */
int db_wallet_tx_at_height(struct node_db *ndb, int height,
                           struct db_wallet_tx *out, size_t max);

/* Wallet UTXOs (transparent outputs belonging to the wallet) */
struct db_wallet_utxo {
    uint8_t txid[32];
    uint32_t vout;
    int64_t value;
    uint8_t address_hash[20];
    uint8_t *script;
    size_t script_len;
    int height;
    uint8_t spent_txid[32];
    int spent_vin;
    bool is_spent;
    bool is_coinbase;
};

/* Validation */
bool db_wallet_utxo_validate(const struct db_wallet_utxo *u,
                              struct ar_errors *errors);

bool db_wallet_utxo_save(struct node_db *ndb, const struct db_wallet_utxo *u);
bool db_wallet_utxo_mark_spent(struct node_db *ndb,
                               const uint8_t txid[32], uint32_t vout,
                               const uint8_t spent_by[32], int vin);
bool db_wallet_utxo_find(struct node_db *ndb,
                         const uint8_t txid[32], uint32_t vout,
                         struct db_wallet_utxo *out);
int64_t db_wallet_utxo_balance(struct node_db *ndb);

/* List unspent wallet UTXOs. Returns count. */
int db_wallet_utxo_list_unspent(struct node_db *ndb,
                                struct db_wallet_utxo *out, size_t max);

/* List all wallet UTXOs (spent + unspent). Returns count. */
int db_wallet_utxo_list_all(struct node_db *ndb,
                            struct db_wallet_utxo *out, size_t max);

/* Coin selection: unspent, non-coinbase (or mature coinbase). */
int db_wallet_utxo_select_coins(struct node_db *ndb, int64_t target,
                                int current_height,
                                struct db_wallet_utxo *out, size_t max);

/* Delete a single wallet UTXO by outpoint. */
bool db_wallet_utxo_delete(struct node_db *ndb,
                            const uint8_t txid[32], uint32_t vout);

/* Count wallet UTXOs for a given txid. */
int db_wallet_utxo_count_for_tx(struct node_db *ndb,
                                 const uint8_t txid[32]);

/* Delete all wallet UTXOs. */
bool db_wallet_utxo_delete_all(struct node_db *ndb);

/* Delete all wallet transactions. */
bool db_wallet_tx_delete_all(struct node_db *ndb);

/* Sapling notes */
struct db_sapling_note {
    uint8_t txid[32];
    uint32_t output_index;
    int64_t value;
    uint8_t rcm[32];
    uint8_t memo[512];
    size_t memo_len;
    uint8_t ivk[32];
    uint8_t diversifier[11];
    uint8_t pk_d[32];
    uint8_t cm[32];
    uint8_t nullifier[32];
    int block_height;
    uint8_t spent_txid[32];
    bool is_spent;
};

bool db_sapling_note_validate(const struct db_sapling_note *n,
                               struct ar_errors *errors);
bool db_sapling_note_save(struct node_db *ndb, const struct db_sapling_note *n);
bool db_sapling_note_mark_spent(struct node_db *ndb,
                                const uint8_t nullifier[32],
                                const uint8_t spent_by[32]);
bool db_sapling_note_is_nullifier_spent(struct node_db *ndb,
                                        const uint8_t nullifier[32]);
int64_t db_sapling_note_balance(struct node_db *ndb);
int64_t db_sapling_note_balance_for_ivk(struct node_db *ndb,
                                        const uint8_t ivk[32]);

/* List unspent notes. Returns count. */
int db_sapling_note_list_unspent(struct node_db *ndb,
                                 struct db_sapling_note *out, size_t max);

/* ── Relationships ─────────────────────────────────────────────── */

/* WalletTx has_many :wallet_utxos */
int db_wallet_tx_utxos(struct node_db *ndb, const uint8_t txid[32],
                        struct db_wallet_utxo *out, size_t max);

/* WalletTx has_many :sapling_notes */
int db_wallet_tx_notes(struct node_db *ndb, const uint8_t txid[32],
                        struct db_sapling_note *out, size_t max);

/* WalletTx belongs_to :block */
struct db_block;
bool db_wallet_tx_block(struct node_db *ndb, const struct db_wallet_tx *t,
                        struct db_block *out);

/* WalletUTXO belongs_to :wallet_key */
struct db_wallet_key;
bool db_wallet_utxo_key(struct node_db *ndb, const struct db_wallet_utxo *u,
                        struct db_wallet_key *out);

/* SaplingNote belongs_to :sapling_key */
struct db_sapling_key;
bool db_sapling_note_key(struct node_db *ndb, const struct db_sapling_note *n,
                         struct db_sapling_key *out);

/* Callbacks for wallet UTXO and sapling note */
struct ar_callbacks *db_wallet_utxo_callbacks(void);
struct ar_callbacks *db_sapling_note_callbacks(void);

#endif
