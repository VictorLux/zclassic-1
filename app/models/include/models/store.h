/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_DB_MODEL_STORE_H
#define ZCL_DB_MODEL_STORE_H

#include "models/database.h"
#include "models/activerecord.h"
#include <stdbool.h>
#include <stdint.h>

enum {
    STORE_PRODUCT_NAME_MAX = 255,
    STORE_PRODUCT_DESC_MAX = 1023,
    STORE_PRODUCT_TOKEN_MAX = 63,
    STORE_ORDER_ADDR_MAX = 127,
    STORE_ORDER_TXID_MAX = 127
};

enum store_order_status {
    STORE_ORDER_PENDING = 0,
    STORE_ORDER_PAID = 1,
    STORE_ORDER_SENT = 2,
    STORE_ORDER_FAILED = 3
};

struct db_store_product {
    int64_t id;
    char name[STORE_PRODUCT_NAME_MAX + 1];
    char description[STORE_PRODUCT_DESC_MAX + 1];
    int64_t price_zatoshi;
    char token_id[STORE_PRODUCT_TOKEN_MAX + 1];
    int tokens_per_purchase;
    bool active;
};

struct db_store_order {
    int64_t id;
    int64_t product_id;
    char customer_addr[STORE_ORDER_ADDR_MAX + 1];
    char payment_addr[STORE_ORDER_ADDR_MAX + 1];
    int64_t amount_zatoshi;
    char payment_txid[STORE_ORDER_TXID_MAX + 1];
    char mint_txid[STORE_ORDER_TXID_MAX + 1];
    int status;
    int64_t created_at;
    int64_t paid_at;
    bool has_paid_at;
};

struct db_store_order_summary {
    int64_t id;
    int status;
    int64_t amount_zatoshi;
    char product_name[STORE_PRODUCT_NAME_MAX + 1];
};

struct db_store_order_view {
    int64_t id;
    int status;
    int64_t amount_zatoshi;
    char payment_addr[STORE_ORDER_ADDR_MAX + 1];
    char customer_addr[STORE_ORDER_ADDR_MAX + 1];
    char payment_txid[STORE_ORDER_TXID_MAX + 1];
    char mint_txid[STORE_ORDER_TXID_MAX + 1];
    char product_name[STORE_PRODUCT_NAME_MAX + 1];
};

struct db_store_pending_payment {
    int64_t id;
    char payment_addr[STORE_ORDER_ADDR_MAX + 1];
    int64_t amount_zatoshi;
    char customer_addr[STORE_ORDER_ADDR_MAX + 1];
    char token_id[STORE_PRODUCT_TOKEN_MAX + 1];
    int64_t tokens_per_purchase;
};

struct ar_callbacks *db_store_product_callbacks(void);
struct ar_callbacks *db_store_order_callbacks(void);

bool db_store_product_validate(const struct db_store_product *p,
                               struct ar_errors *errors);
bool db_store_order_validate(const struct db_store_order *o,
                             struct ar_errors *errors);

bool db_store_product_save(struct node_db *ndb, const struct db_store_product *p);
bool db_store_product_find_active(struct node_db *ndb, int64_t id,
                                  struct db_store_product *out);
int db_store_product_list_active(struct node_db *ndb,
                                 struct db_store_product *out, size_t max);
int db_store_product_count(struct node_db *ndb);
bool db_store_order_save(struct node_db *ndb, const struct db_store_order *o);
bool db_store_order_find_view(struct node_db *ndb, int64_t id,
                              struct db_store_order_view *out);
int db_store_order_list_recent(struct node_db *ndb,
                               struct db_store_order_summary *out, size_t max);
int db_store_order_list_pending_payments(struct node_db *ndb,
                                         struct db_store_pending_payment *out,
                                         size_t max,
                                         int64_t min_created_at);
bool db_store_order_mark_paid(struct node_db *ndb, int64_t id, int status);

/* Payment-check reads. The store payment processor needs the current
 * chain tip (to compute confirmation depth) and the confirmed shielded
 * balance received at a one-time order z-address. These read the
 * consensus `blocks` table and the wallet `wallet_sapling_notes` table
 * with store-specific semantics (no block status filter on the tip; a
 * confirmation-depth ceiling on received notes), so the SQL lives here
 * in the store model rather than in the controller. Both return 0 on
 * any error or no-row. */
int64_t db_store_chain_tip_height(struct node_db *ndb);
int64_t db_store_received_payment(struct node_db *ndb, const char *pay_addr,
                                  int64_t max_height);

#endif
