/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * MCP ops controller: aggregate / health / observability tools.
 * Core: zcl_status, zcl_health, zcl_kpi, zcl_filemanifest, zcl_events, zcl_rpc
 * Mempool/mining: zcl_getmempoolinfo, zcl_getrawmempool, zcl_getmininginfo
 * Performance: zcl_benchmark, zcl_dbstats */

#include "../controllers.h"
#include "../router.h"
#include "../rpc_client.h"
#include "../replay.h"

#include "json/json.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEFINE_PT(name, rpc)                                                   \
    static int name(const struct mcp_request *req, struct mcp_response *res)  \
    {                                                                          \
        (void)req;                                                             \
        char *out = mcp_node_rpc(rpc, NULL);                                   \
        if (!out) {                                                            \
            res->error = MCP_ERR_HANDLER_FAILED;                               \
            snprintf(res->error_message, sizeof(res->error_message),           \
                     "RPC %s returned null", rpc);                             \
            LOG_ERR("mcp.ops", "RPC %s returned null", rpc);                   \
        }                                                                      \
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

    char *out = zcl_malloc(32768, "status_body");
    if (!out) {
        free(h); free(p); free(s); free(v); free(hc);
        res->error = MCP_ERR_INTERNAL;
        snprintf(res->error_message, sizeof(res->error_message),
                 "malloc failed for status response");
        LOG_ERR("mcp.ops", "malloc failed for status body (32768 bytes)");
    }
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
    if (!out) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "RPC healthcheck returned null");
        LOG_ERR("mcp.ops", "healthcheck returned null");
    }
    res->body = out;
    return 0;
}

static int h_zcl_filemanifest(const struct mcp_request *req, struct mcp_response *res)
{
    (void)req;
    char *out = mcp_node_rpc("getfilemanifeststatus", NULL);
    if (!out) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "RPC getfilemanifeststatus returned null");
        LOG_ERR("mcp.ops", "getfilemanifeststatus returned null");
    }
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
    if (!out) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "RPC eventlog returned null");
        LOG_ERR("mcp.ops", "eventlog returned null");
    }
    res->body = out;
    return 0;
}

static int h_zcl_rpc(const struct mcp_request *req, struct mcp_response *res)
{
    const char *m = json_get_str(json_get(req->args, "method"));
    const struct json_value *p = json_get(req->args, "params");
    char *out = mcp_node_rpc(m, p ? json_get_str(p) : NULL);
    if (!out) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "RPC %s returned null", m ? m : "(null)");
        LOG_ERR("mcp.ops", "RPC %s returned null", m ? m : "(null)");
    }
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

/* ── zcl_profile (wave 6): per-thread CPU sampler ────────────
 *
 * Reads /proc/self/task/<tid>/stat for every live thread, sleeps
 * `duration_ms`, reads again, diffs utime + stime, sorts descending,
 * returns the top N.  Designed for "why is this node slow at 3am"
 * — an operator runs one MCP call and gets a hot-thread list
 * without needing gdb/perf/strace on the live process.
 *
 * Blocking: the handler sleeps the calling MCP worker for duration_ms.
 * Clamped to 10 seconds max so a runaway caller can't wedge the stdio
 * loop forever.
 */
#define PROFILE_MAX_THREADS 256

struct profile_sample {
    int      tid;
    char     name[16];
    uint64_t utime;   /* clock ticks */
    uint64_t stime;
};

/* Parse utime (field 14) and stime (field 15) from /proc/<pid>/stat.
 * The `comm` field at position 2 can contain spaces and parens, so we
 * skip everything up to the LAST ')' before counting tokens. */
static bool parse_task_stat(const char *buf, uint64_t *utime, uint64_t *stime)
{
    const char *p = strrchr(buf, ')');
    if (!p) return false;
    p++;
    /* After the ')', the next token is state (field 3).  utime is
     * field 14, stime field 15 — 11 and 12 tokens ahead. */
    int fields_to_skip = 11; /* 3 -> 14 is 11 steps */
    for (int i = 0; i < fields_to_skip; i++) {
        while (*p == ' ') p++;
        while (*p && *p != ' ') p++;
    }
    while (*p == ' ') p++;
    char *end = NULL;
    *utime = strtoull(p, &end, 10);
    if (end == p) return false;
    p = end;
    while (*p == ' ') p++;
    *stime = strtoull(p, &end, 10);
    return end != p;
}

static size_t read_task_snapshot(struct profile_sample *out, size_t cap)
{
    DIR *d = opendir("/proc/self/task");
    if (!d) return 0;
    size_t n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < cap) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        int tid = atoi(e->d_name);
        if (tid <= 0) continue;

        char path[128];
        snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        char buf[1024];
        size_t r = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        if (r == 0) continue;
        buf[r] = '\0';

        uint64_t u = 0, s = 0;
        if (!parse_task_stat(buf, &u, &s)) continue;

        out[n].tid = tid;
        out[n].utime = u;
        out[n].stime = s;

        snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
        FILE *cf = fopen(path, "r");
        out[n].name[0] = '\0';
        if (cf) {
            if (fgets(out[n].name, sizeof(out[n].name), cf)) {
                size_t L = strlen(out[n].name);
                if (L > 0 && out[n].name[L - 1] == '\n')
                    out[n].name[L - 1] = '\0';
            }
            fclose(cf);
        }
        n++;
    }
    closedir(d);
    return n;
}

struct profile_delta {
    int      tid;
    char     name[16];
    int64_t  utime_ticks;
    int64_t  stime_ticks;
    int64_t  total_ticks;
};

static int profile_delta_cmp(const void *a, const void *b)
{
    const struct profile_delta *pa = a;
    const struct profile_delta *pb = b;
    if (pb->total_ticks != pa->total_ticks)
        return (pb->total_ticks > pa->total_ticks) ? 1 : -1;
    return 0;
}

static int h_zcl_profile(const struct mcp_request *req,
                          struct mcp_response *res)
{
    const struct json_value *dv = json_get(req->args, "duration_ms");
    const struct json_value *nv = json_get(req->args, "top_n");

    int64_t duration_ms = dv ? json_get_int(dv) : 1000;
    int64_t top_n       = nv ? json_get_int(nv) : 10;
    if (duration_ms < 100)   duration_ms = 100;
    if (duration_ms > 10000) duration_ms = 10000;
    if (top_n < 1)           top_n = 1;
    if (top_n > 64)          top_n = 64;

    static struct profile_sample s1[PROFILE_MAX_THREADS];
    static struct profile_sample s2[PROFILE_MAX_THREADS];
    size_t n1 = read_task_snapshot(s1, PROFILE_MAX_THREADS);
    if (n1 == 0) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "failed to read /proc/self/task (no threads found)");
        LOG_ERR("mcp.ops", "profile: read_task_snapshot returned 0 (pre-sample)");
    }

    struct timespec ts = {
        .tv_sec  = duration_ms / 1000,
        .tv_nsec = (duration_ms % 1000) * 1000000L,
    };
    nanosleep(&ts, NULL);

    size_t n2 = read_task_snapshot(s2, PROFILE_MAX_THREADS);
    if (n2 == 0) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "failed to read /proc/self/task (no threads found post-sample)");
        LOG_ERR("mcp.ops", "profile: read_task_snapshot returned 0 (post-sample)");
    }

    /* Compute deltas for threads present in both samples. */
    static struct profile_delta deltas[PROFILE_MAX_THREADS];
    size_t nd = 0;
    for (size_t i = 0; i < n2; i++) {
        const struct profile_sample *a = NULL;
        for (size_t j = 0; j < n1; j++) {
            if (s1[j].tid == s2[i].tid) { a = &s1[j]; break; }
        }
        if (!a) continue;
        int64_t du = (int64_t)s2[i].utime - (int64_t)a->utime;
        int64_t dss = (int64_t)s2[i].stime - (int64_t)a->stime;
        if (du < 0) du = 0;
        if (dss < 0) dss = 0;
        deltas[nd].tid = s2[i].tid;
        snprintf(deltas[nd].name, sizeof(deltas[nd].name), "%s", s2[i].name);
        deltas[nd].utime_ticks = du;
        deltas[nd].stime_ticks = dss;
        deltas[nd].total_ticks = du + dss;
        nd++;
    }

    qsort(deltas, nd, sizeof(deltas[0]), profile_delta_cmp);

    long clk_tck = sysconf(_SC_CLK_TCK);
    if (clk_tck <= 0) clk_tck = 100;

    size_t cap = 16384;
    char *out = zcl_malloc(cap, "profile_body");
    if (!out) {
        res->error = MCP_ERR_INTERNAL;
        snprintf(res->error_message, sizeof(res->error_message),
                 "malloc failed for profile response");
        LOG_ERR("mcp.ops", "malloc failed for profile body (%zu bytes)", cap);
    }
    size_t pos = 0;
    pos += (size_t)snprintf(out + pos, cap - pos,
        "{\"duration_ms\":%lld,\"sampled_threads\":%zu,\"top_threads\":[",
        (long long)duration_ms, nd);

    size_t emit = (nd < (size_t)top_n) ? nd : (size_t)top_n;
    for (size_t i = 0; i < emit; i++) {
        int64_t user_ms = deltas[i].utime_ticks * 1000 / clk_tck;
        int64_t sys_ms  = deltas[i].stime_ticks * 1000 / clk_tck;
        if (pos + 256 >= cap) break;
        pos += (size_t)snprintf(out + pos, cap - pos,
            "%s{\"tid\":%d,\"name\":\"%s\","
            "\"user_ms\":%lld,\"sys_ms\":%lld,"
            "\"cpu_pct\":%.1f}",
            i == 0 ? "" : ",",
            deltas[i].tid, deltas[i].name,
            (long long)user_ms, (long long)sys_ms,
            100.0 * (double)(user_ms + sys_ms) / (double)duration_ms);
    }
    pos += (size_t)snprintf(out + pos, cap - pos, "]}");

    res->body = out;
    return 0;
}

/* ── Replay recorder handlers ───────────────────────────────── */

static int h_zcl_replay_dump(const struct mcp_request *req,
                               struct mcp_response *res)
{
    const struct json_value *cnt = json_get(req->args, "count");
    size_t count = cnt ? (size_t)json_get_int(cnt) : 0;
    char *out = mcp_replay_dump(count);
    if (!out) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "replay dump returned null");
        LOG_ERR("mcp.ops", "mcp_replay_dump returned null (count=%zu)", count);
    }
    res->body = out;
    return 0;
}

static int h_zcl_replay_exec(const struct mcp_request *req,
                               struct mcp_response *res)
{
    const struct json_value *idx_v = json_get(req->args, "index");
    if (!idx_v) {
        snprintf(res->error_message, sizeof(res->error_message),
                 "index is required");
        res->error = MCP_ERR_MISSING_PARAM;
        LOG_ERR("mcp.ops", "replay_exec: index param missing");
    }
    int64_t idx = json_get_int(idx_v);
    size_t total = mcp_replay_count();
    if (idx < 0 || (size_t)idx >= total) {
        snprintf(res->error_message, sizeof(res->error_message),
                 "index %lld out of range [0, %zu)",
                 (long long)idx, total);
        res->error = MCP_ERR_OUT_OF_RANGE;
        LOG_ERR("mcp.ops", "replay_exec: index %lld out of range [0, %zu)",
                (long long)idx, total);
    }

    /* Dump to get the entry, parse tool name, then re-dispatch. */
    char *dump = mcp_replay_dump(0);
    if (!dump) {
        res->error = MCP_ERR_INTERNAL;
        snprintf(res->error_message, sizeof(res->error_message),
                 "replay dump failed during exec");
        LOG_ERR("mcp.ops", "replay_exec: mcp_replay_dump returned null");
    }

    struct json_value arr;
    if (!json_read(&arr, dump, strlen(dump)) || arr.type != JSON_ARR ||
        (size_t)idx >= arr.num_children) {
        free(dump);
        snprintf(res->error_message, sizeof(res->error_message),
                 "replay parse failed");
        res->error = MCP_ERR_INTERNAL;
        LOG_ERR("mcp.ops", "replay_exec: json parse failed for index %lld",
                (long long)idx);
    }

    const struct json_value *entry = &arr.children[idx];
    const struct json_value *tv = json_get(entry, "tool");
    const char *tool = tv ? json_get_str(tv) : NULL;
    if (!tool || !tool[0]) {
        json_free(&arr);
        free(dump);
        snprintf(res->error_message, sizeof(res->error_message),
                 "entry has no tool name");
        res->error = MCP_ERR_INTERNAL;
        LOG_ERR("mcp.ops", "replay_exec: entry %lld has no tool name",
                (long long)idx);
    }

    /* Re-dispatch with no args (the original args are stored as escaped
     * JSON string, not structured — just re-call the tool fresh). */
    char *result = mcp_router_dispatch(tool, NULL);
    json_free(&arr);
    free(dump);

    if (!result) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "replay re-dispatch failed: tool=%s", tool);
        LOG_ERR("mcp.ops", "replay_exec re-dispatch failed: tool=%s", tool);
    }
    res->body = result;
    return 0;
}

/* ── Route table ─────────────────────────────────────────────── */

static const struct mcp_param_spec p_replay_dump[] = {
    { "count", MCP_PARAM_INT, false,
      "Number of most recent entries to return (0 = all)",
      0, MCP_REPLAY_RING_SIZE, 0, 0, NULL, "0" },
};
static const struct mcp_param_spec p_replay_exec[] = {
    { "index", MCP_PARAM_INT, true,
      "Index into the replay buffer (0 = oldest)",
      0, MCP_REPLAY_RING_SIZE - 1, 0, 0, NULL, NULL },
};

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
static const struct mcp_param_spec p_profile[] = {
    { "duration_ms", MCP_PARAM_INT, false,
      "Sample window in ms (clamped to [100, 10000])",
      100, 10000, 0, 0, NULL, "1000" },
    { "top_n", MCP_PARAM_INT, false,
      "Max threads returned, sorted by CPU (clamped to [1, 64])",
      1, 64, 0, 0, NULL, "10" },
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
    { "zcl_profile", "ops",
      "Per-thread CPU sampler: reads /proc/self/task/*/stat before "
      "and after `duration_ms`, returns top N threads by CPU delta "
      "with name, user_ms, sys_ms, cpu_pct. For diagnosing slow "
      "nodes without attaching gdb.",
      p_profile, sizeof(p_profile) / sizeof(p_profile[0]), h_zcl_profile },
    { "zcl_replay_dump", "ops",
      "Dump the MCP request/response replay buffer (last 100 calls). "
      "Shows tool name, args, response, timestamp, duration, error status.",
      p_replay_dump, sizeof(p_replay_dump) / sizeof(p_replay_dump[0]),
      h_zcl_replay_dump },
    { "zcl_replay_exec", "ops",
      "Re-execute a previously recorded MCP request by index from the "
      "replay buffer. Useful for debugging and regression testing.",
      p_replay_exec, sizeof(p_replay_exec) / sizeof(p_replay_exec[0]),
      h_zcl_replay_exec },
};

void mcp_register_ops(void)
{
    for (size_t i = 0; i < sizeof(k_routes) / sizeof(k_routes[0]); i++)
        mcp_router_register(&k_routes[i]);
}
