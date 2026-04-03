/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "config/runtime.h"
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

sqlite3 *app_runtime_query_db(void)
{
    struct db_service *svc = app_runtime_db_service();
    return db_service_query_db(svc);
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
