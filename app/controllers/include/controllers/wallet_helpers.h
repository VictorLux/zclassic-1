/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_CONTROLLERS_WALLET_HELPERS_H
#define ZCL_CONTROLLERS_WALLET_HELPERS_H

#include "rpc/server.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

struct wallet;
struct main_state;
struct wallet_db;
struct tx_mempool;
struct connman;
struct node_db;
struct coins_view_cache;
struct json_value;
struct transaction;
struct db_wallet_tx;

/* Shared wallet state — set once at boot, read by all wallet controllers */
extern struct wallet *g_wallet;
extern struct main_state *g_main_state;
extern const char *g_datadir;
extern struct wallet_db *g_wallet_db;
extern struct tx_mempool *g_mempool;
extern struct connman *g_connman_ptr;
extern struct node_db *g_node_db;
extern struct coins_view_cache *g_coins_tip;

#define ENSURE_WALLET(result) do {                        \
    if (!g_wallet) {                                      \
        json_set_str((result), "Wallet not available");   \
        return false;                                     \
    }                                                     \
} while (0)

/* Amount formatting/parsing */
void format_amount(int64_t satoshis, char *out, size_t out_size);
int64_t parse_amount(const struct json_value *v);

/* Transaction history helpers */
int wallet_history_count(void);
bool wallet_history_db_ready(void);
bool wallet_db_tx_deserialize(const struct db_wallet_tx *dbtx,
                              struct transaction *tx);
int wallet_db_tx_confirmations(const struct db_wallet_tx *dbtx);
void append_one_entry(struct json_value *result,
                      const char *txid, int vout_n,
                      const char *category, const char *address,
                      int64_t amount, int64_t fee,
                      int confirmations, int64_t time_received);
bool wallet_append_tx_entry(const struct transaction *tx,
                            bool from_me, int64_t fee,
                            int confirmations, int64_t time_received,
                            struct json_value *result);

#endif
