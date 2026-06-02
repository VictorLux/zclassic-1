/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * Internal helper contract shared between the node_db connection-handle
 * source files (database.c, database_migrate.c, database_modes.c). NOT
 * part of the public model API — callers must include models/database.h.
 *
 * This header keeps shared connection-handle helpers visible only to the
 * database model siblings that own migrations, runtime modes, and health
 * activity stamping. */

#ifndef ZCL_DB_MODEL_DATABASE_INTERNAL_H
#define ZCL_DB_MODEL_DATABASE_INTERNAL_H

#include "models/database.h"
#include <sqlite3.h>

/* Execute `sql`, logging any error with `where` context. Returns the
 * sqlite3 rc so callers can make tolerance decisions. (Defined in
 * database.c; used by migrations and performance-mode helpers.) */
int db_exec_checked(sqlite3 *db, const char *sql, const char *where);

/* Apply the idempotent baseline schema DDL (SCHEMA[]). Defined in
 * database_schema.c; called once from node_db_open(). Returns false on a
 * real schema regression (boot must halt). */
bool create_schema(struct node_db *ndb);

/* Like db_exec_checked but tolerates a known-benign error substring
 * (e.g. "duplicate column name" when re-applying idempotent ALTERs).
 * (Defined in database.c; used by the migration runner.) */
int db_exec_tolerant(sqlite3 *db, const char *sql, const char *where,
                     const char *tolerable_substr);

/* Record turbo-mode transition in the runtime health snapshot.
 * (Defined in database.c; used by performance-mode helpers.) */
void node_db_note_turbo_mode(struct node_db *ndb, bool turbo_mode,
                             const char *op, int rc);

/* Stamp last-activity time + sqlite rc + op label into the runtime
 * health snapshot. (Defined in database.c; used by the KV state store
 * in database_migrate.c.) */
void node_db_note_activity(struct node_db *ndb, const char *op, int rc);

#endif
