/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * MCP ops controller: aggregate / health / observability tools.
 * Core: zcl_status, zcl_health, zcl_kpi, zcl_filemanifest, zcl_events, zcl_rpc
 * Mempool/mining: zcl_getmempoolinfo, zcl_getrawmempool, zcl_getmininginfo
 * Performance: zcl_benchmark, zcl_dbstats */

#include "../controllers.h"
#include "../router.h"
#include "../rpc_client.h"

#include "json/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFINE_PT(name, rpc)                                                   \
    static int name(const struct mcp_request *req, struct mcp_response *res)  \
    {                                                                          \
        (void)req;                                                             \
        char *out = mcp_node_rpc(rpc, NULL);                                   \
        if (!out) return -1;                                                   \
        res->body = out;                                                       \
        return 0;                                                              \
    }

DEFINE_PT(h_zcl_getmempoolinfo, "getmempoolinfo")
DEFINE_PT(h_zcl_getrawmempool,  "getrawmempool")
DEFINE_PT(h_zcl_getmininginfo,  "getmininginfo")
DEFINE_PT(h_zcl_benchmark,      "benchmark")
DEFINE_PT(h_zcl_dbstats,        "db_info")

/* ── Handlers ───────────────────────────────────────────────── */

static int h_zcl_status(const struct mcp_request *req, struct mcp_response *res)
{
    (void)req;
    char *h  = mcp_node_rpc("getblockcount", NULL);
    char *p  = mcp_node_rpc("getpeerinfo", NULL);
    char *s  = mcp_node_rpc("syncstate", NULL);
    char *v  = mcp_node_rpc("validationstatus", NULL);
    char *hc = mcp_node_rpc("healthcheck", NULL);

    int pc = 0;
    if (p) { for (char *c = p; *c; c++) if (*c == '{') pc++; }

    char *out = malloc(32768);
    if (!out) { free(h); free(p); free(s); free(v); free(hc); return -1; }
    snprintf(out, 32768,
             "{\"height\":%s,\"peers\":%d,\"sync\":%s,"
             "\"validation\":%s,\"health\":%s}",
             h ? h : "null", pc, s ? s : "null",
             v ? v : "null", hc ? hc : "null");
    free(h); free(p); free(s); free(v); free(hc);
    res->body = out;
    return 0;
}

static int h_zcl_health(const struct mcp_request *req, struct mcp_response *res)
{
    (void)req;
    char *out = mcp_node_rpc("healthcheck", NULL);
    if (!out) return -1;
    res->body = out;
    return 0;
}

static int h_zcl_filemanifest(const struct mcp_request *req, struct mcp_response *res)
{
    (void)req;
    char *out = mcp_node_rpc("getfilemanifeststatus", NULL);
    if (!out) return -1;
    res->body = out;
    return 0;
}

static int h_zcl_events(const struct mcp_request *req, struct mcp_response *res)
{
    const struct json_value *cnt = json_get(req->args, "count");
    char params[64];
    snprintf(params, sizeof(params), "[%lld]",
             cnt ? (long long)json_get_int(cnt) : 20LL);
    char *out = mcp_node_rpc("eventlog", params);
    if (!out) return -1;
    res->body = out;
    return 0;
}

static int h_zcl_rpc(const struct mcp_request *req, struct mcp_response *res)
{
    const char *m = json_get_str(json_get(req->args, "method"));
    const struct json_value *p = json_get(req->args, "params");
    char *out = mcp_node_rpc(m, p ? json_get_str(p) : NULL);
    if (!out) return -1;
    res->body = out;
    return 0;
}

/* zcl_kpi — single call that returns every subsystem KPI. Used by
 * operators to take the pulse of the node in one shot. Each nested
 * field is the raw result of the corresponding RPC, so field shapes
 * remain stable over time — we just add new top-level fields. */
static int h_zcl_kpi(const struct mcp_request *req, struct mcp_response *res)
{
    (void)req;
    char *height    = mcp_node_rpc("getblockcount",     NULL);
    char *peers     = mcp_node_rpc("getpeerinfo",       NULL);
    char *sync      = mcp_node_rpc("syncstate",         NULL);
    char *val       = mcp_node_rpc("validationstatus",  NULL);
    char *health    = mcp_node_rpc("healthcheck",       NULL);
    char *mempool   = mcp_node_rpc("getmempoolinfo",    NULL);
    char *wallet    = mcp_node_rpc("getwalletinfo",     NULL);
    char *chain     = mcp_node_rpc("getblockchaininfo", NULL);
    char *network   = mcp_node_rpc("getnetworkinfo",    NULL);

    int peer_count = 0;
    if (peers) {
        for (char *c = peers; *c; c++) if (*c == '{') peer_count++;
    }

    size_t cap = 65536;
    char *out = malloc(cap);
    if (!out) {
        free(height); free(peers); free(sync); free(val); free(health);
        free(mempool); free(wallet); free(chain); free(network);
        return -1;
    }

    snprintf(out, cap,
        "{"
        "\"height\":%s,"
        "\"peer_count\":%d,"
        "\"sync\":%s,"
        "\"validation\":%s,"
        "\"health\":%s,"
        "\"mempool\":%s,"
        "\"wallet\":%s,"
        "\"chain\":%s,"
        "\"network\":%s"
        "}",
        height  ? height  : "null",
        peer_count,
        sync    ? sync    : "null",
        val     ? val     : "null",
        health  ? health  : "null",
        mempool ? mempool : "null",
        wallet  ? wallet  : "null",
        chain   ? chain   : "null",
        network ? network : "null");

    free(height); free(peers); free(sync); free(val); free(health);
    free(mempool); free(wallet); free(chain); free(network);
    res->body = out;
    return 0;
}

/* ── Route table ─────────────────────────────────────────────── */

static const struct mcp_param_spec p_events[] = {
    { "count", MCP_PARAM_INT, false, "Number of events",
      1, 1000, 0, 0, NULL, "20" },
};
static const struct mcp_param_spec p_rpc[] = {
    { "method", MCP_PARAM_STR, true,  "RPC method name",
      0, 0, 1, 128, NULL, NULL },
    { "params", MCP_PARAM_STR, false, "JSON params array",
      0, 0, 0, 0, NULL, "\"[]\"" },
};

static const struct mcp_tool_route k_routes[] = {
    { "zcl_status", "ops",
      "Node status: block height, peers, sync state, onion address, "
      "bg-validation progress, health checks. The single command to "
      "check if everything is working.",
      NULL, 0, h_zcl_status },
    { "zcl_health", "ops",
      "Health check: pass/fail, chain height, peers, sync, onion.",
      NULL, 0, h_zcl_health },
    { "zcl_kpi", "ops",
      "One-shot KPI dashboard: height, peer_count, sync, validation, "
      "health, mempool, wallet, chain, network — every subsystem in "
      "one response. The flagship operator tool for debugging.",
      NULL, 0, h_zcl_kpi },
    { "zcl_getmempoolinfo", "ops",
      "Mempool size, bytes, usage.",
      NULL, 0, h_zcl_getmempoolinfo },
    { "zcl_getrawmempool", "ops",
      "Array of txids currently in the mempool.",
      NULL, 0, h_zcl_getrawmempool },
    { "zcl_getmininginfo", "ops",
      "Mining stats: hashrate, difficulty, current block, pooled tx.",
      NULL, 0, h_zcl_getmininginfo },
    { "zcl_benchmark", "ops",
      "Hash / malloc / hash160 throughput (sha256d, malloc-4K, hash160 "
      "ops/sec).",
      NULL, 0, h_zcl_benchmark },
    { "zcl_dbstats", "ops",
      "Database health: table counts, SQLite page stats, sizes.",
      NULL, 0, h_zcl_dbstats },
    { "zcl_filemanifest", "ops",
      "File service status: chunks, SHA3 hashes, total size.",
      NULL, 0, h_zcl_filemanifest },
    { "zcl_events", "ops",
      "Recent event log: sync events, peer connections, blocks.",
      p_events, sizeof(p_events) / sizeof(p_events[0]), h_zcl_events },
    { "zcl_rpc", "ops",
      "Call any RPC method directly. 85+ commands available.",
      p_rpc, sizeof(p_rpc) / sizeof(p_rpc[0]), h_zcl_rpc },
};

void mcp_register_ops(void)
{
    for (size_t i = 0; i < sizeof(k_routes) / sizeof(k_routes[0]); i++)
        mcp_router_register(&k_routes[i]);
}
