/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * Store SQLite schema bootstrap and default product seeding. */

#include "controllers/store_controller_internal.h"

/* Ensure store tables exist */
void store_ensure_schema(sqlite3 *db, const char *datadir)
{
    /* Load products from {datadir}/store/products.json if it exists,
     * otherwise seed with demo products. This lets node operators
     * customize their store by editing a simple JSON file. */
    struct node_db cnt_ndb = { .db = db, .open = true };
    bool empty = (db_store_product_count(&cnt_ndb) == 0);

    if (empty && datadir) {
        char json_path[1024];
        snprintf(json_path, sizeof(json_path), "%s/store/products.json", datadir);
        FILE *f = fopen(json_path, "r");
        if (f) {
            /* Parse simple JSON array of products:
             * [{"name":"...","description":"...","price_zcl":0.01,
             *   "token_id":"...","tokens_per_purchase":1}, ...] */
            char buf[16384];
            size_t len = fread(buf, 1, sizeof(buf) - 1, f);
            buf[len] = '\0';
            fclose(f);

            /* Simple JSON array parser — find each {...} object */
            const char *p = buf;
            int loaded = 0;
            while ((p = strchr(p, '{')) != NULL) {
                const char *end = strchr(p, '}');
                if (!end) break;

                /* Extract fields with simple string search */
                char name[256] = "", desc[1024] = "", token[64] = "";
                double price_zcl = 0.0;
                int tokens = 1;

                /* name */
                const char *q = strstr(p, "\"name\"");
                if (q && q < end) {
                    q = strchr(q + 6, '"'); if (q) { q++;
                    const char *e = strchr(q, '"');
                    if (e && (size_t)(e-q) < sizeof(name)) {
                        memcpy(name, q, (size_t)(e-q)); name[e-q] = '\0';
                    }}
                }
                /* description */
                q = strstr(p, "\"description\"");
                if (q && q < end) {
                    q = strchr(q + 13, '"'); if (q) { q++;
                    const char *e = strchr(q, '"');
                    if (e && (size_t)(e-q) < sizeof(desc)) {
                        memcpy(desc, q, (size_t)(e-q)); desc[e-q] = '\0';
                    }}
                }
                /* token_id */
                q = strstr(p, "\"token_id\"");
                if (q && q < end) {
                    q = strchr(q + 10, '"'); if (q) { q++;
                    const char *e = strchr(q, '"');
                    if (e && (size_t)(e-q) < sizeof(token)) {
                        memcpy(token, q, (size_t)(e-q)); token[e-q] = '\0';
                    }}
                }
                /* price_zcl */
                q = strstr(p, "\"price_zcl\"");
                if (q && q < end) {
                    q += 11; while (*q == ':' || *q == ' ') q++;
                    price_zcl = strtod(q, NULL);
                }
                /* tokens_per_purchase */
                q = strstr(p, "\"tokens_per_purchase\"");
                if (q && q < end) {
                    q += 20; while (*q == ':' || *q == ' ') q++;
                    long tval = strtol(q, NULL, 10);
                    tokens = (tval > 0 && tval <= 10000) ? (int)tval : 1;
                }

                if (name[0] && price_zcl > 0) {
                    struct node_db ndb;
                    struct db_store_product product;
                    memset(&ndb, 0, sizeof(ndb));
                    ndb.db = db;
                    ndb.open = true;
                    memset(&product, 0, sizeof(product));
                    snprintf(product.name, sizeof(product.name), "%s", name);
                    snprintf(product.description, sizeof(product.description), "%s", desc);
                    snprintf(product.token_id, sizeof(product.token_id), "%s", token);
                    product.price_zatoshi =
                        (int64_t)(price_zcl * (double)ZATOSHI_PER_ZCL);
                    product.tokens_per_purchase = tokens;
                    product.active = true;
                    if (!db_store_product_save(&ndb, &product)) {
                        p = end + 1;
                        continue;
                    }
                    loaded++;
                }
                p = end + 1;
            }
            if (loaded > 0)
                printf("Store: loaded %d products from %s\n", loaded, json_path);
            else
                printf("Store: %s exists but no valid products found\n", json_path);
            fflush(stdout);
            empty = (loaded == 0);
        }
    }

    /* Fallback: seed demo products if still empty */
    if (empty) {
        struct node_db ndb;
        memset(&ndb, 0, sizeof(ndb));
        ndb.db = db;
        ndb.open = true;
        struct db_store_product products[] = {
            {
                .name = "ZCL23 Access Token",
                .description =
                    "1 token grants access to premium .onion services on the "
                    "ZClassic23 network. Tokens are ZSLP tokens on the ZClassic "
                    "blockchain.",
                .price_zatoshi = 1000000,
                .token_id = "ZCL23ACCESS",
                .tokens_per_purchase = 10,
                .active = true
            },
            {
                .name = "VPN Credit (1 month)",
                .description =
                    "Route traffic through the ZClassic23 onion network. "
                    "1 month of encrypted relay service.",
                .price_zatoshi = 5000000,
                .token_id = "ZCL23VPN",
                .tokens_per_purchase = 1,
                .active = true
            },
            {
                .name = "Storage (1 GB)",
                .description =
                    "Encrypted storage on the ZClassic23 distributed network. "
                    "Data replicated across multiple .onion nodes.",
                .price_zatoshi = 2000000,
                .token_id = "ZCL23STORE",
                .tokens_per_purchase = 1,
                .active = true
            }
        };
        for (size_t i = 0; i < sizeof(products) / sizeof(products[0]); i++) {
            /* Log-and-continue: a failed default-product seed must be
             * observable, but one bad seed should not abort store setup. */
            if (!db_store_product_save(&ndb, &products[i]))
                LOG_WARN("store", "default product seed failed: name=%s",
                         products[i].name);
        }
    }
}

/* Get the .onion address from the onion service layer (may be NULL). */
