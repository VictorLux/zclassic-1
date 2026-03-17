/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Order model — tracks purchases and payment state. */

#ifndef ZCL_MODEL_ORDER_H
#define ZCL_MODEL_ORDER_H

#include "models/database.h"
#include <stdbool.h>
#include <stdint.h>

enum order_status {
    ORDER_PENDING = 0,     /* awaiting payment */
    ORDER_PAID = 1,        /* payment confirmed */
    ORDER_MINTED = 2,      /* tokens sent to customer */
    ORDER_FAILED = 3,      /* payment expired or failed */
};

struct db_order {
    int64_t id;
    int64_t product_id;
    char customer_addr[128]; /* customer's t-address for token delivery */
    char payment_addr[128];  /* z-address generated for this order */
    int64_t amount_zatoshi;  /* expected payment amount */
    char payment_txid[65];   /* txid of confirmed payment */
    char mint_txid[65];      /* txid of ZSLP token mint */
    enum order_status status;
    int64_t created_at;
    int64_t paid_at;
};

bool db_order_save(struct node_db *ndb, const struct db_order *o);
bool db_order_find(struct node_db *ndb, int64_t id, struct db_order *out);
int db_order_list_pending(struct node_db *ndb, struct db_order *out, size_t max);
bool db_order_update_status(struct node_db *ndb, int64_t id,
                             enum order_status status, const char *txid);

#endif
