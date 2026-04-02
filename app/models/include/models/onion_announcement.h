/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_DB_MODEL_ONION_ANNOUNCEMENT_H
#define ZCL_DB_MODEL_ONION_ANNOUNCEMENT_H

#include "models/database.h"
#include "models/activerecord.h"
#include <stdbool.h>
#include <stdint.h>

enum {
    ONION_ADDRESS_MAX = 127,
    ONION_SCRIPT_HEX_MAX = 511
};

struct db_onion_announcement {
    char onion_address[ONION_ADDRESS_MAX + 1];
    int64_t announced_at;
    char script_hex[ONION_SCRIPT_HEX_MAX + 1];
};

struct ar_callbacks *db_onion_announcement_callbacks(void);
bool db_onion_announcement_validate(const struct db_onion_announcement *a,
                                    struct ar_errors *errors);
bool db_onion_announcement_save(struct node_db *ndb,
                                const struct db_onion_announcement *a);
bool db_onion_announcement_exists(struct node_db *ndb,
                                  const char *onion_address);
int db_onion_announcement_recent(struct node_db *ndb,
                                 struct db_onion_announcement *out,
                                 size_t max);

#endif
