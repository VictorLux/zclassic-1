/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "controllers/chain_projection.h"

#include "framework/projection.h"
#include "mcp/rpc_client.h"
#include "util/log_macros.h"
#include "util/path_check.h"

#include <stdint.h>

static int64_t chain_projection_query_i64(const char *label, const char *sql)
{
    const char *datadir = mcp_rpc_client_datadir();
    if (!datadir || !datadir[0])
        LOG_RETURN(-1, "chain_projection", "%s: MCP datadir is unset", label);

    char db_path[1024];
    zcl_node_db_path(db_path, sizeof(db_path), datadir);

    projection_t *p = FRAMEWORK_PROJECTION_OPEN(label, db_path);
    if (!p)
        LOG_RETURN(-1, "chain_projection", "%s: projection open failed", label);

    int64_t v = PROJECTION_QUERY_INT64_OR(p, label, sql, -1);
    projection_close(p);
    return v;
}

int64_t chain_projection_best_block_height(void)
{
    return chain_projection_query_i64(
        "chain.best_block_height",
        "SELECT COALESCE(MAX(height), -1) FROM blocks WHERE status>=3");
}

int64_t chain_projection_best_header_height(void)
{
    return chain_projection_query_i64(
        "chain.best_header_height",
        "SELECT COALESCE(MAX(height), -1) FROM blocks");
}
