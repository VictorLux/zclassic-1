/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * ZSLP token model — tracks SLP tokens and transfers in SQLite. */

#ifndef ZCL_DB_MODEL_ZSLP_H
#define ZCL_DB_MODEL_ZSLP_H

#include "models/database.h"
#include <stdbool.h>
#include <stdint.h>

/* Save a ZSLP token GENESIS record. token_id = genesis txid (internal order). */
bool db_zslp_token_save(struct node_db *ndb, const uint8_t token_id[32],
                         const char *ticker, const char *name,
                         int decimals, const char *document_url,
                         int genesis_height, int64_t initial_quantity);

/* Save a ZSLP transfer (GENESIS, SEND, or MINT output). */
bool db_zslp_transfer_save(struct node_db *ndb, const uint8_t txid[32],
                            int block_height, const uint8_t token_id[32],
                            int tx_type, int64_t amount, int vout,
                            const uint8_t *to_addr); /* NULL if unknown */

/* Count tokens and transfers. */
int64_t db_zslp_token_count(struct node_db *ndb);
int64_t db_zslp_transfer_count(struct node_db *ndb);

/* Wipe all ZSLP data (for re-indexing). */
void db_zslp_clear_all(struct node_db *ndb);

#endif
