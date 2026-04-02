/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_DB_MODEL_CONTACT_H
#define ZCL_DB_MODEL_CONTACT_H

#include "models/database.h"
#include "models/activerecord.h"
#include <stdbool.h>
#include <stdint.h>

enum {
    CONTACT_ADDRESS_MAX = 255,
    CONTACT_NAME_MAX = 63
};

struct db_contact {
    char address[CONTACT_ADDRESS_MAX + 1];
    char name[CONTACT_NAME_MAX + 1];
    int64_t last_used;
};

struct ar_callbacks *db_contact_callbacks(void);
bool db_contact_validate(const struct db_contact *c, struct ar_errors *errors);
bool db_contact_save(struct node_db *ndb, const struct db_contact *c);
int db_contact_recent(struct node_db *ndb, struct db_contact *out, size_t max);

#endif
