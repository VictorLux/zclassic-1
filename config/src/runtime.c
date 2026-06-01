/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "config/runtime.h"
#include "models/database.h"
#include <stddef.h>

static struct app_runtime_context *g_current_runtime = NULL;

void app_runtime_set_current(struct app_runtime_context *runtime)
{
    g_current_runtime = runtime;
}

const struct app_runtime_context *app_runtime_current(void)
{
    return g_current_runtime;
}

struct db_service *app_runtime_db_service(void)
{
    if (!g_current_runtime)
        return NULL;
    return g_current_runtime->db_service;
}

struct node_db *app_runtime_node_db(void)
{
    struct db_service *svc = app_runtime_db_service();
    return db_service_node_db(svc);
}

bool app_runtime_node_db_handle_open(const struct node_db *ndb)
{
    return ndb && ndb->open;
}

bool app_runtime_node_db_is_open(void)
{
    return app_runtime_node_db_handle_open(app_runtime_node_db());
}

bool app_runtime_node_db_state_set(struct node_db *ndb,
                                   const char *key,
                                   const void *value,
                                   size_t len)
{
    if (!app_runtime_node_db_handle_open(ndb))
        return false;
    return node_db_state_set(ndb, key, value, len);
}

void app_runtime_node_db_sync_flush_if_needed(struct node_db *ndb)
{
    if (app_runtime_node_db_handle_open(ndb) && ndb->sync_in_batch)
        (void)node_db_sync_flush(ndb);
}

bool app_runtime_node_db_wal_checkpoint(struct node_db *ndb)
{
    if (!app_runtime_node_db_handle_open(ndb))
        return false;
    return node_db_wal_checkpoint(ndb);
}

bool app_runtime_node_db_wal_checkpoint_passive(struct node_db *ndb)
{
    if (!app_runtime_node_db_handle_open(ndb) || !ndb->db)
        return false;
    return sqlite3_wal_checkpoint_v2(ndb->db, NULL,
                                     SQLITE_CHECKPOINT_PASSIVE,
                                     NULL, NULL) == SQLITE_OK;
}

sqlite3 *app_runtime_query_db(void)
{
    struct db_service *svc = app_runtime_db_service();
    return db_service_query_db(svc);
}

struct snapshot_sync_service *app_runtime_snapshot_sync(void)
{
    if (!g_current_runtime)
        return NULL;
    return g_current_runtime->snapshot_sync;
}

struct tx_mempool *app_runtime_mempool(void)
{
    if (!g_current_runtime)
        return NULL;
    return g_current_runtime->mempool;
}

struct wallet *app_runtime_wallet(void)
{
    if (!g_current_runtime)
        return NULL;
    return g_current_runtime->wallet;
}
