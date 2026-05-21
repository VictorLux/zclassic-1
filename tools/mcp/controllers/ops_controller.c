/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * MCP ops controller: status/health/aggregate dashboards.
 *   Core:     zcl_status, zcl_health, zcl_kpi, zcl_events, zcl_rpc,
 *             zcl_mirror_status, zcl_filemanifest, zcl_syncdiag,
 *             zcl_self_heal_stats
 *   Mempool/mining: zcl_getmempoolinfo, zcl_getrawmempool, zcl_getmininginfo
 *   Performance: zcl_benchmark, zcl_dbstats
 *
 * Low-level diagnostic primitives (zcl_sql, zcl_state, zcl_node_log,
 * zcl_profile, zcl_probe_zclassicd, zcl_diff_with_legacy, zcl_replay_*)
 * live in diagnostics_controller.c. */

#include "../controllers.h"
#include "../router.h"
#include "../rpc_client.h"
#include "../rpc_params.h"

#include "json/json.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "validation/process_block.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

DEFINE_PT(h_zcl_getmempoolinfo, "getmempoolinfo", "mcp.ops")
DEFINE_PT(h_zcl_mempool_inspect, "getmempoolfeestats", "mcp.ops")
DEFINE_PT(h_zcl_getrawmempool,  "getrawmempool",  "mcp.ops")
DEFINE_PT(h_zcl_getmininginfo,  "getmininginfo",  "mcp.ops")
DEFINE_PT(h_zcl_benchmark,      "benchmark",      "mcp.ops")
DEFINE_PT(h_zcl_dbstats,        "db_info",        "mcp.ops")


/* ── Handlers ───────────────────────────────────────────────── */

static int h_zcl_status(const struct mcp_request *req, struct mcp_response *res)
{
    (void)req;
    char *h  = mcp_node_rpc("getblockcount", NULL);
    char *p  = mcp_node_rpc("getpeerinfo", NULL);
    char *s  = mcp_node_rpc("syncstate", NULL);
    char *v  = mcp_node_rpc("validationstatus", NULL);
    char *hc = mcp_node_rpc("healthcheck", NULL);
    char *ci = mcp_node_rpc("getblockchaininfo", NULL);
    char *cac = mcp_node_rpc("dumpstate", "[\"chain_advance_coordinator\"]");

    int pc = 0, inbound = 0, outbound = 0, zcl23_cnt = 0, magicbean_cnt = 0;
    if (p) {
        for (char *c = p; *c; c++) if (*c == '{') pc++;
        /* Count inbound vs outbound */
        const char *sp = p;
        while ((sp = strstr(sp, "\"inbound\"")) != NULL) {
            sp += strlen("\"inbound\"");
            while (*sp == ' ' || *sp == ':') sp++;
            if (strncmp(sp, "true", 4) == 0)
                inbound++;
            else
                outbound++;
        }
        /* Count by client type via subver */
        sp = p;
        while ((sp = strstr(sp, "\"subver\"")) != NULL) {
            sp += strlen("\"subver\"");
            while (*sp == ' ' || *sp == ':' || *sp == '"') sp++;
            if (strstr(sp, "ZClassic-C23") != NULL &&
                (strchr(sp, '"') == NULL || strstr(sp, "ZClassic-C23") < strchr(sp, '"')))
                zcl23_cnt++;
            else if (strstr(sp, "MagicBean") != NULL &&
                     (strchr(sp, '"') == NULL || strstr(sp, "MagicBean") < strchr(sp, '"')))
                magicbean_cnt++;
        }
    }

    /* Extract header_height from getblockchaininfo best_header_height */
    int header_height = 0;
    if (ci) {
        const char *bhh = strstr(ci, "\"best_header_height\"");
        if (bhh) {
            bhh += strlen("\"best_header_height\"");
            while (*bhh == ' ' || *bhh == ':') bhh++;
            header_height = atoi(bhh);
        }
    }

    /* Extract max peer starting_height from getpeerinfo */
    int max_peer_height = 0;
    if (p) {
        const char *sp = p;
        while ((sp = strstr(sp, "\"startingheight\"")) != NULL) {
            sp += strlen("\"startingheight\"");
            while (*sp == ' ' || *sp == ':') sp++;
            int sh = atoi(sp);
            if (sh > max_peer_height) max_peer_height = sh;
        }
    }

    int block_height = h ? atoi(h) : 0;
    int header_gap = max_peer_height - header_height;
    if (header_gap < 0) header_gap = 0;
    bool sync_behind = header_gap > 144;

    enum { ZCL_STATUS_BODY_CAP = 65536 };
    char *out = zcl_malloc(ZCL_STATUS_BODY_CAP, "status_body");
    if (!out) {
        free(h); free(p); free(s); free(v); free(hc); free(ci); free(cac);
        res->error = MCP_ERR_INTERNAL;
        snprintf(res->error_message, sizeof(res->error_message),
                 "malloc failed for status response");
        LOG_ERR("mcp.ops", "malloc failed for status body");
        return -1;  // raw-return-ok:logged-oom
    }
    /* Extract memory_rss_mb and uptime_seconds from healthcheck response */
    int64_t memory_rss_mb = -1;
    int64_t uptime_secs = 0;
    if (hc) {
        const char *rss = strstr(hc, "\"memory_rss_mb\"");
        if (rss) {
            rss += strlen("\"memory_rss_mb\"");
            while (*rss == ' ' || *rss == ':') rss++;
            memory_rss_mb = atoll(rss);
        }
        const char *ut = strstr(hc, "\"uptime_seconds\"");
        if (ut) {
            ut += strlen("\"uptime_seconds\"");
            while (*ut == ' ' || *ut == ':') ut++;
            uptime_secs = atoll(ut);
        }
    }

    snprintf(out, ZCL_STATUS_BODY_CAP,
             "{\"height\":%d,\"header_height\":%d,"
             "\"max_peer_height\":%d,\"header_gap\":%d,"
             "\"sync_behind\":%s,"
             "\"peers\":%d,"
             "\"connections\":{\"total\":%d,\"inbound\":%d,"
             "\"outbound\":%d,\"zcl23\":%d,\"magicbean\":%d},"
             "\"memory_rss_mb\":%lld,\"uptime_secs\":%lld,"
             "\"sync\":%s,"
             "\"validation\":%s,\"health\":%s,"
             "\"chain_advance\":%s}",
             block_height, header_height,
             max_peer_height, header_gap,
             sync_behind ? "true" : "false",
             pc,
             pc, inbound, outbound, zcl23_cnt, magicbean_cnt,
             (long long)memory_rss_mb, (long long)uptime_secs,
             s ? s : "null",
             v ? v : "null", hc ? hc : "null",
             cac ? cac : "null");
    free(h); free(p); free(s); free(v); free(hc); free(ci); free(cac);
    res->body = out;
    return 0;
}

static int h_zcl_health(const struct mcp_request *req, struct mcp_response *res)
{
    (void)req;
    return mcp_return_rpc_body(res, mcp_node_rpc("healthcheck", NULL),
                                "healthcheck", "mcp.ops");
}

DEFINE_PT(h_zcl_mirror_status, "getmirrorstatus",       "mcp.ops")
DEFINE_PT(h_zcl_filemanifest,  "getfilemanifeststatus", "mcp.ops")

static int h_zcl_events(const struct mcp_request *req, struct mcp_response *res)
{
    char params[64];
    snprintf(params, sizeof(params), "[%lld]",
             (long long)json_get_int_or(req->args, "count", 20));
    return mcp_return_rpc_body(res, mcp_node_rpc("eventlog", params),
                                "eventlog", "mcp.ops");
}

static int h_zcl_rpc(const struct mcp_request *req, struct mcp_response *res)
{
    const char *m = json_get_str(json_get(req->args, "method"));
    return mcp_return_rpc_body(res,
                                mcp_node_rpc(m, json_get_str_or(req->args, "params", NULL)),
                                m ? m : "(null)", "mcp.ops");
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
    char *out = zcl_malloc(cap, "kpi_body");
    if (!out) {
        free(height); free(peers); free(sync); free(val); free(health);
        free(mempool); free(wallet); free(chain); free(network);
        res->error = MCP_ERR_INTERNAL;
        snprintf(res->error_message, sizeof(res->error_message),
                 "malloc failed for KPI response");
        LOG_ERR("mcp.ops", "malloc failed for kpi body (%zu bytes)", cap);
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

static int h_zcl_self_heal_stats(const struct mcp_request *req,
                                  struct mcp_response *res)
{
    (void)req;
    struct self_heal_scan_stats stats;
    process_block_self_heal_stats_snapshot(&stats);

    char *out = zcl_malloc(512, "self_heal_stats_body");
    if (!out) {
        res->error = MCP_ERR_INTERNAL;
        snprintf(res->error_message, sizeof(res->error_message),
                 "malloc failed for self-heal stats response");
        LOG_ERR("mcp.ops", "malloc failed for self-heal stats body");
        return 0;
    }

    snprintf(out, 512,
        "{"
        "\"tx_index_hits\":%llu,"
        "\"scan_hits\":%llu,"
        "\"scan_exhausted\":%llu,"
        "\"scan_blocks_checked_total\":%llu,"
        "\"scan_depth_limit\":%d"
        "}",
        (unsigned long long)stats.tx_index_hits,
        (unsigned long long)stats.scan_hits,
        (unsigned long long)stats.scan_exhausted,
        (unsigned long long)stats.scan_blocks_checked_total,
        process_block_self_heal_scan_depth_limit());
    res->body = out;
    return 0;
}

/* ── zcl_syncdiag ─────────────────────────────────────────────── */

/* Combines getsyncdiag (watchdog, header counters, chain/header heights)
 * with download queue stats and peer max height into a single response
 * for diagnosing sync issues without multiple tool calls. */
static int h_zcl_syncdiag(const struct mcp_request *req,
                           struct mcp_response *res)
{
    (void)req;
    char *diag = mcp_node_rpc("getsyncdiag", NULL);
    char *dl   = mcp_node_rpc("downloadstats", NULL);
    char *pi   = mcp_node_rpc("getpeerinfo", NULL);

    /* Extract peer_max_height from getpeerinfo (max starting_height) */
    int peer_max_height = 0;
    if (pi) {
        /* Scan for "startingheight": N — take the maximum */
        const char *p = pi;
        while ((p = strstr(p, "\"startingheight\"")) != NULL) {
            p += strlen("\"startingheight\"");
            while (*p == ' ' || *p == ':') p++;
            int h = atoi(p);
            if (h > peer_max_height) peer_max_height = h;
        }
    }

    size_t cap = 16384;
    char *out = zcl_malloc(cap, "syncdiag_body");
    if (!out) {
        free(diag); free(dl); free(pi);
        res->error = MCP_ERR_INTERNAL;
        snprintf(res->error_message, sizeof(res->error_message),
                 "malloc failed for syncdiag response");
        LOG_ERR("mcp.ops", "malloc failed for syncdiag body (%zu bytes)", cap); return -1;
    }

    /* Merge diag object with download stats and peer_max_height.
     * diag is a JSON object like {...} — strip the trailing '}',
     * append the extra fields, re-close. */
    if (diag) {
        size_t dlen = strlen(diag);
        /* Find last '}' */
        while (dlen > 0 && diag[dlen - 1] != '}') dlen--;
        if (dlen > 0) diag[dlen - 1] = '\0';  /* strip trailing } */

        snprintf(out, cap,
            "%s,\"peer_max_height\":%d,"
            "\"download\":%s}",
            diag,
            peer_max_height,
            dl ? dl : "null");
    } else {
        snprintf(out, cap,
            "{\"error\":\"getsyncdiag RPC failed\","
            "\"peer_max_height\":%d,"
            "\"download\":%s}",
            peer_max_height,
            dl ? dl : "null");
    }

    free(diag); free(dl); free(pi);
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
      "bg-validation progress, health checks, and chain advance source "
      "scoring. The single command to check if everything is working.",
      NULL, 0, h_zcl_status, 0, NULL },
    { "zcl_health", "ops",
      "Health check: pass/fail, chain height, peers, sync, onion.",
      NULL, 0, h_zcl_health, 0, NULL },
    { "zcl_mirror_status", "ops",
      "Canonical zclassic23/zclassicd mirror lockstep status: both "
      "heights and hashes, lag, reachability, running state, and "
      "catch-up counters.",
      NULL, 0, h_zcl_mirror_status, 0, NULL },
    { "zcl_kpi", "ops",
      "One-shot KPI dashboard: height, peer_count, sync, validation, "
      "health, mempool, wallet, chain, network — every subsystem in "
      "one response. The flagship operator tool for debugging.",
      NULL, 0, h_zcl_kpi, 0, NULL },
    { "zcl_self_heal_stats", "ops",
      "Self-heal UTXO recovery counters: tx-index hits, bounded scan "
      "hits/exhaustion, total scanned blocks, and active scan depth.",
      NULL, 0, h_zcl_self_heal_stats, 0, NULL },
    { "zcl_getmempoolinfo", "ops",
      "Mempool size, bytes, usage.",
      NULL, 0, h_zcl_getmempoolinfo, 0, NULL },
    { "zcl_mempool_inspect", "ops",
      "Mempool fee-rate (zat/byte) and age histograms. Power-user "
      "signal for transaction fee construction and congestion diagnosis.",
      NULL, 0, h_zcl_mempool_inspect, 0, NULL },
    { "zcl_getrawmempool", "ops",
      "Array of txids currently in the mempool.",
      NULL, 0, h_zcl_getrawmempool, 0, NULL },
    { "zcl_getmininginfo", "ops",
      "Mining stats: hashrate, difficulty, current block, pooled tx.",
      NULL, 0, h_zcl_getmininginfo, 0, NULL },
    { "zcl_benchmark", "ops",
      "Hash / malloc / hash160 throughput (sha256d, malloc-4K, hash160 "
      "ops/sec).",
      NULL, 0, h_zcl_benchmark, 0, NULL },
    { "zcl_dbstats", "ops",
      "Database health: table counts, SQLite page stats, sizes.",
      NULL, 0, h_zcl_dbstats, 0, NULL },
    { "zcl_filemanifest", "ops",
      "File service status: chunks, SHA3 hashes, total size.",
      NULL, 0, h_zcl_filemanifest, 0, NULL },
    { "zcl_events", "ops",
      "Recent event log: sync events, peer connections, blocks.",
      p_events, PARAM_COUNT(p_events), h_zcl_events, 0, NULL },
    { "zcl_rpc", "ops",
      "Call any RPC method directly. 85+ commands available.",
      p_rpc, PARAM_COUNT(p_rpc), h_zcl_rpc,
      .flags = MCP_TOOL_FLAG_DESTRUCTIVE /* arbitrary RPC — skip in self_test */ },
    { "zcl_syncdiag", "ops",
      "Deep sync diagnostics: sync state, chain height, best header "
      "height, peer max height, header gap, watchdog status and "
      "escalation level, header batch counters, download queue size "
      "and in-flight count. The single tool for diagnosing sync stalls.",
      NULL, 0, h_zcl_syncdiag, 0, NULL },
};

void mcp_register_ops(void)
{
    for (size_t i = 0; i < PARAM_COUNT(k_routes); i++)
        mcp_router_register(&k_routes[i]);
}
