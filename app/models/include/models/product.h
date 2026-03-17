/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Product model — items for sale in a ZSLP token store. */

#ifndef ZCL_MODEL_PRODUCT_H
#define ZCL_MODEL_PRODUCT_H

#include "models/database.h"
#include <stdbool.h>
#include <stdint.h>

struct db_product {
    int64_t id;
    char name[128];
    char description[512];
    int64_t price_zatoshi;   /* price in zatoshi (1 ZCL = 1e8) */
    char token_id[65];       /* ZSLP token ID (hex) */
    uint64_t tokens_per_purchase; /* tokens minted per purchase */
    bool active;
};

bool db_product_save(struct node_db *ndb, const struct db_product *p);
bool db_product_find(struct node_db *ndb, int64_t id, struct db_product *out);
int db_product_list(struct node_db *ndb, struct db_product *out, size_t max);

#endif
