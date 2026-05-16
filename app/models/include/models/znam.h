/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_DB_MODEL_ZNAM_H
#define ZCL_DB_MODEL_ZNAM_H

#include "models/database.h"
#include "models/activerecord.h"
#include "znam/znam.h"
#include <stdbool.h>

/* ActiveRecord models for the ZCL Names (ZNAM) registry.
 *
 * Three tables, three record types, all defined in znam/znam.h and
 * reused here:
 *   znam_names         → struct znam_entry
 *   znam_text_records  → struct znam_text_record
 *   znam_addr_records  → struct znam_addr_record
 *
 * The validators enforce the format constraints from the on-chain
 * ZNAM protocol (lokad ID "ZNAM"). Names that fail validation should
 * never have been accepted from OP_RETURN parsing in the first place;
 * the validator here is the last line of defense against corrupted
 * blocks reaching at-rest storage. */

struct ar_callbacks *db_znam_entry_callbacks(void);
struct ar_callbacks *db_znam_text_callbacks(void);
struct ar_callbacks *db_znam_addr_callbacks(void);

bool db_znam_entry_validate(const struct znam_entry *entry,
                            struct ar_errors *errors);
bool db_znam_text_validate(const struct znam_text_record *rec,
                           struct ar_errors *errors);
bool db_znam_addr_validate(const struct znam_addr_record *rec,
                           struct ar_errors *errors);

#endif
