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
#include "../rpc_params.h"

#include "controllers/diagnostics_controller.h"
#include "json/json.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "validation/process_block.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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
    const struct json_value *cnt = json_get(req->args, "count");
    char params[64];
    snprintf(params, sizeof(params), "[%lld]",
             cnt ? (long long)json_get_int(cnt) : 20LL);
    return mcp_return_rpc_body(res, mcp_node_rpc("eventlog", params),
                                "eventlog", "mcp.ops");
}

static int h_zcl_rpc(const struct mcp_request *req, struct mcp_response *res)
{
    const char *m = json_get_str(json_get(req->args, "method"));
    const struct json_value *p = json_get(req->args, "params");
    return mcp_return_rpc_body(res,
                                mcp_node_rpc(m, p ? json_get_str(p) : NULL),
                                m ? m : "(null)", "mcp.ops");
}

/* zcl_sql — SELECT-only SQL passthrough to node.db. Marked destructive
 * in middleware not because it mutates (it can't) but because arbitrary
 * scans against a 100M-row table can be expensive. */
static int h_zcl_sql(const struct mcp_request *req,
                     struct mcp_response *res)
{
    const char *sql = json_get_str(json_get(req->args, "sql"));
    const struct json_value *limit = json_get(req->args, "limit");

    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, sql ? sql : "");
    mcp_params_push_int(&p, limit ? json_get_int(limit) : 10);
    char *pjson = mcp_params_to_json(&p);

    char *out = pjson ? mcp_node_rpc("dbquery", pjson) : NULL;
    free(pjson);
    return mcp_return_rpc_body(res, out, "dbquery", "mcp.ops");
}

/* zcl_node_log — reverse-scan node.log via getnodelog RPC.
 *
 * Server-side regex match + level filter. Bounded memory: chunks the
 * 56 MB live log and stops at max_lines. The big win is that the
 * client (Claude Code) doesn't have to download the whole file just
 * to grep it. */
static int h_zcl_node_log(const struct mcp_request *req,
                          struct mcp_response *res)
{
    const char *pattern = json_get_str(json_get(req->args, "pattern"));
    const struct json_value *since = json_get(req->args, "since_secs");
    const struct json_value *maxl  = json_get(req->args, "max_lines");
    const char *level = json_get_str(json_get(req->args, "level"));

    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, pattern ? pattern : "");
    mcp_params_push_int(&p, since ? json_get_int(since) : 300);
    mcp_params_push_int(&p, maxl  ? json_get_int(maxl)  : 50);
    mcp_params_push_str(&p, level && level[0] ? level : "all");
    char *pjson = mcp_params_to_json(&p);

    char *out = pjson ? mcp_node_rpc("getnodelog", pjson) : NULL;
    free(pjson);
    return mcp_return_rpc_body(res, out, "getnodelog", "mcp.ops");
}

/* zcl_state — generic in-process state dump.
 *
 * Dispatches by `subsystem` to the owning module's `*_dump_state_json`
 * function via the `dumpstate` RPC method. Adding a new subsystem is
 * one dispatcher line in app/controllers/src/diagnostics_controller.c
 * plus one dump function in the owning module — no further MCP
 * plumbing required. See CLAUDE.md "Adding state introspection".
 *
 * Current subsystems include watchdog, boot, block_index, and
 * chain_advance_coordinator. */
static int h_zcl_state(const struct mcp_request *req, struct mcp_response *res)
{
    const char *sub = json_get_str(json_get(req->args, "subsystem"));
    const struct json_value *key_val = json_get(req->args, "key");
    const char *key = key_val ? json_get_str(key_val) : NULL;

    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, sub ? sub : "");
    if (key && key[0])
        mcp_params_push_str(&p, key);
    char *pjson = mcp_params_to_json(&p);

    char *out = pjson ? mcp_node_rpc("dumpstate", pjson) : NULL;
    free(pjson);
    return mcp_return_rpc_body_ctx(res, out, "dumpstate", "mcp.ops",
                                    "subsystem=%s", sub ? sub : "(null)");
}

/* zcl_probe_zclassicd — drift detection against the local zclassicd
 * (legacy C++ ZClassic impl). Picks a random height if none supplied,
 * fans through to the `probezclassicd` RPC, returns the raw RPC body
 * (height, our_hash, their_hash, match, ...). */
static int h_zcl_probe_zclassicd(const struct mcp_request *req,
                                 struct mcp_response *res)
{
    const struct json_value *h_val = json_get(req->args, "height");
    int height = -1;
    if (h_val) {
        if (h_val->type == JSON_INT)
            height = (int)json_get_int(h_val);
        else if (h_val->type == JSON_STR)
            height = atoi(json_get_str(h_val));
    }

    /* If height not supplied, ask the node for getblockcount and pick
     * a random height in [0, tip-100]. */
    if (height < 0) {
        char *tip_s = mcp_node_rpc("getblockcount", NULL);
        int tip = tip_s ? atoi(tip_s) : 0;
        free(tip_s);
        int max_h = tip - 100;
        if (max_h <= 0) {
            res->error = MCP_ERR_HANDLER_FAILED;
            snprintf(res->error_message, sizeof(res->error_message),
                     "node not synced: tip=%d", tip);
            LOG_ERR("mcp.ops", "probe_zclassicd: tip too low (%d)", tip);
        }
        /* Cheap random: time-based. Deterministic-test code paths
         * pass an explicit height anyway. */
        unsigned seed = (unsigned)time(NULL) ^ (unsigned)getpid();
        height = (int)(rand_r(&seed) % (unsigned)max_h);
    }

    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_int(&p, height);
    char *pjson = mcp_params_to_json(&p);
    char *out = pjson ? mcp_node_rpc("probezclassicd", pjson) : NULL;
    free(pjson);
    return mcp_return_rpc_body_ctx(res, out, "probezclassicd", "mcp.ops",
                                    "h=%d", height);
}

/* Tiny helper: pull "key":N out of a JSON-ish string, return N or -1.
 * Not a real parser — we only read fields we know our own RPCs emit. */
static long long ops_scan_int(const char *body, const char *key)
{
    if (!body || !key) return -1;  // raw-return-ok:sentinel
    char needle[64];
    int n = snprintf(needle, sizeof(needle), "\"%s\":", key);
    if (n <= 0 || (size_t)n >= sizeof(needle)) return -1;  // raw-return-ok:sentinel
    const char *p = strstr(body, needle);
    if (!p) return -1;  // raw-return-ok:sentinel
    p += (size_t)n;
    while (*p == ' ') p++;
    char *end = NULL;
    long long v = strtoll(p, &end, 10);
    if (end == p) return -1;  // raw-return-ok:sentinel
    return v;
}

static bool ops_scan_bool(const char *body, const char *key)
{
    if (!body || !key) return false;
    char needle[64];
    int n = snprintf(needle, sizeof(needle), "\"%s\":", key);
    if (n <= 0 || (size_t)n >= sizeof(needle)) return false;
    const char *p = strstr(body, needle);
    if (!p) return false;
    p += (size_t)n;
    while (*p == ' ') p++;
    return strncmp(p, "true", 4) == 0;
}

/* zcl_diff_with_legacy — one-call "are we tracking zclassicd?" check.
 * Composes getmirrorstatus (height delta + lag) with probezclassicd at
 * local_tip-6 (hash comparison at a recent-but-stable block) and
 * derives a single-word verdict so an operator can answer "did my
 * change converge?" without composing four RPCs by hand. */
static int h_zcl_diff_with_legacy(const struct mcp_request *req,
                                  struct mcp_response *res)
{
    (void)req;

    char *mstatus = mcp_node_rpc("getmirrorstatus", NULL);
    if (!mstatus) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "getmirrorstatus returned null — mirror service down?");
        LOG_ERR("mcp.ops", "diff_with_legacy: getmirrorstatus null");
        return 0;
    }
    int local_h    = (int)ops_scan_int(mstatus, "local_height");
    int legacy_h   = (int)ops_scan_int(mstatus, "legacy_height");
    int lag        = (int)ops_scan_int(mstatus, "lag");
    bool reachable = ops_scan_bool(mstatus, "reachable");

    char *probe = NULL;
    int probe_h = -1;
    bool hash_match = false;
    if (reachable && local_h > 100) {
        probe_h = local_h - 6;
        struct mcp_params p;
        mcp_params_init(&p);
        mcp_params_push_int(&p, probe_h);
        char *pjson = mcp_params_to_json(&p);
        probe = pjson ? mcp_node_rpc("probezclassicd", pjson) : NULL;
        free(pjson);
        if (probe) hash_match = ops_scan_bool(probe, "match");
    }

    const char *verdict;
    if (!reachable)               verdict = "legacy_unreachable";
    else if (probe && !hash_match) verdict = "diverged";
    else if (lag == 0 && hash_match) verdict = "converged";
    else if (lag > 0 && lag <= 10) verdict = "tracking";
    else if (lag > 10)             verdict = "lagging";
    else                           verdict = "unknown";

    size_t cap = 8192;
    char *out = zcl_malloc(cap, "diff_with_legacy_body");
    if (!out) {
        free(mstatus); free(probe);
        res->error = MCP_ERR_INTERNAL;
        snprintf(res->error_message, sizeof(res->error_message),
                 "malloc failed for diff_with_legacy response");
        LOG_ERR("mcp.ops", "malloc failed for diff_with_legacy (%zu)", cap);
        return 0;
    }
    snprintf(out, cap,
        "{\"verdict\":\"%s\","
         "\"local_height\":%d,"
         "\"legacy_height\":%d,"
         "\"lag\":%d,"
         "\"reachable\":%s,"
         "\"probe_height\":%d,"
         "\"hash_match\":%s,"
         "\"mirror_status\":%s,"
         "\"probe_zclassicd\":%s}",
        verdict, local_h, legacy_h, lag,
        reachable ? "true" : "false",
        probe_h,
        hash_match ? "true" : "false",
        mstatus,
        probe ? probe : "null");

    free(mstatus); free(probe);
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

/* ── zcl_profile — per-thread CPU sampler ────────────────────
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

/* ── zcl_syncdiag — deep sync diagnostics ─────────────────────
 *
 * Combines getsyncdiag (watchdog, header counters, chain/header heights)
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
/* p_state.subsystem.enum_csv + description are derived from the live
 * g_dumpers registry at mcp_register_ops() time (see populate below).
 * Hence non-const: we patch the pointers once at boot. New subsystems
 * added to diagnostics_controller.c auto-propagate to the MCP schema. */
static char g_state_subsystems_csv[512];
static char g_state_subsystem_desc[768];
static struct mcp_param_spec p_state[] = {
    { "subsystem", MCP_PARAM_STR, true,
      "Subsystem name (filled at register-time from g_dumpers registry)",
      0, 0, 1, 64, NULL, NULL },
    { "key", MCP_PARAM_STR, false,
      "Subsystem-specific key (block_index: height or hex hash)",
      0, 0, 0, 128, NULL, NULL },
};
static const struct mcp_param_spec p_probe_zclassicd[] = {
    { "height", MCP_PARAM_INT, false,
      "Block height to probe (omit for random in [0, tip-100])",
      0, 0x7fffffff, 0, 0, NULL, NULL },
};
static const struct mcp_param_spec p_sql[] = {
    { "sql",   MCP_PARAM_STR, true,
      "SELECT-only query (no DDL, no semicolons, auto-LIMIT)",
      0, 0, 1, 1024, NULL, NULL },
    { "limit", MCP_PARAM_INT, false,
      "Row cap (1..100; auto-appended if SQL lacks LIMIT)",
      1, 100, 0, 0, NULL, "10" },
};
static const struct mcp_param_spec p_node_log[] = {
    { "pattern",    MCP_PARAM_STR, true,
      "POSIX-extended regex matched against each log line",
      0, 0, 1, 256, NULL, NULL },
    { "since_secs", MCP_PARAM_INT, false,
      "Only consider lines from the last N seconds (0..86400)",
      0, 86400, 0, 0, NULL, "300" },
    { "max_lines",  MCP_PARAM_INT, false,
      "Cap on returned lines",
      1, 500, 0, 0, NULL, "50" },
    { "level",      MCP_PARAM_STR, false,
      "Level filter",
      0, 0, 0, 16, "all,info,warn,error,fatal", "\"all\"" },
};

static const struct mcp_tool_route k_routes[] = {
    { "zcl_status", "ops",
      "Node status: block height, peers, sync state, onion address, "
      "bg-validation progress, health checks, and chain advance source "
      "scoring. The single command to check if everything is working.",
      NULL, 0, h_zcl_status },
    { "zcl_health", "ops",
      "Health check: pass/fail, chain height, peers, sync, onion.",
      NULL, 0, h_zcl_health },
    { "zcl_mirror_status", "ops",
      "Canonical zclassic23/zclassicd mirror lockstep status: both "
      "heights and hashes, lag, reachability, running state, and "
      "catch-up counters.",
      NULL, 0, h_zcl_mirror_status },
    { "zcl_kpi", "ops",
      "One-shot KPI dashboard: height, peer_count, sync, validation, "
      "health, mempool, wallet, chain, network — every subsystem in "
      "one response. The flagship operator tool for debugging.",
      NULL, 0, h_zcl_kpi },
    { "zcl_self_heal_stats", "ops",
      "Self-heal UTXO recovery counters: tx-index hits, bounded scan "
      "hits/exhaustion, total scanned blocks, and active scan depth.",
      NULL, 0, h_zcl_self_heal_stats },
    { "zcl_getmempoolinfo", "ops",
      "Mempool size, bytes, usage.",
      NULL, 0, h_zcl_getmempoolinfo },
    { "zcl_mempool_inspect", "ops",
      "Mempool fee-rate (zat/byte) and age histograms. Power-user "
      "signal for transaction fee construction and congestion diagnosis.",
      NULL, 0, h_zcl_mempool_inspect },
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
      p_events, PARAM_COUNT(p_events), h_zcl_events },
    { "zcl_rpc", "ops",
      "Call any RPC method directly. 85+ commands available.",
      p_rpc, PARAM_COUNT(p_rpc), h_zcl_rpc },
    { "zcl_state", "ops",
      "Generic in-process state dump. See params.subsystem.enum for the "
      "live list (derived from g_dumpers in diagnostics_controller.c). "
      "For block_index, pass `key`=height or hex hash. New subsystems "
      "plug in via *_dump_state_json (see CLAUDE.md).",
      p_state, PARAM_COUNT(p_state), h_zcl_state },
    { "zcl_probe_zclassicd", "ops",
      "Drift detection: ask the local zclassicd (independent ZClassic "
      "impl) for getblockhash(H) and compare to our block_index. Picks a "
      "random height if `height` is omitted. Returns {height, our_hash, "
      "their_hash, match}.",
      p_probe_zclassicd,
      PARAM_COUNT(p_probe_zclassicd),
      h_zcl_probe_zclassicd },
    { "zcl_diff_with_legacy", "ops",
      "One-call \"are we tracking zclassicd?\" check. Composes mirror "
      "status (height delta + lag) with a probe_zclassicd hash compare "
      "at local_tip-6, returns a single-word verdict (converged / "
      "tracking / lagging / diverged / legacy_unreachable) plus the "
      "raw inputs for triage.",
      NULL, 0, h_zcl_diff_with_legacy },
    { "zcl_node_log", "ops",
      "Reverse-scan node.log server-side with regex + level filter. Avoids "
      "downloading the 56 MB log just to grep. Returns newest matches first.",
      p_node_log, PARAM_COUNT(p_node_log),
      h_zcl_node_log },
    { "zcl_sql", "ops",
      "SELECT-only SQL passthrough to node.db. Hard validation + 2s timeout. "
      "Marked destructive (rate-gated) because arbitrary scans can be costly.",
      p_sql, PARAM_COUNT(p_sql), h_zcl_sql },
    { "zcl_profile", "ops",
      "Per-thread CPU sampler: reads /proc/self/task/*/stat before "
      "and after `duration_ms`, returns top N threads by CPU delta "
      "with name, user_ms, sys_ms, cpu_pct. For diagnosing slow "
      "nodes without attaching gdb.",
      p_profile, PARAM_COUNT(p_profile), h_zcl_profile },
    { "zcl_syncdiag", "ops",
      "Deep sync diagnostics: sync state, chain height, best header "
      "height, peer max height, header gap, watchdog status and "
      "escalation level, header batch counters, download queue size "
      "and in-flight count. The single tool for diagnosing sync stalls.",
      NULL, 0, h_zcl_syncdiag },
    { "zcl_replay_dump", "ops",
      "Dump the MCP request/response replay buffer (last 100 calls). "
      "Shows tool name, args, response, timestamp, duration, error status.",
      p_replay_dump, PARAM_COUNT(p_replay_dump),
      h_zcl_replay_dump },
    { "zcl_replay_exec", "ops",
      "Re-execute a previously recorded MCP request by index from the "
      "replay buffer. Useful for debugging and regression testing.",
      p_replay_exec, PARAM_COUNT(p_replay_exec),
      h_zcl_replay_exec },
};

/* Tools that mutate state or are otherwise unsafe to call via self_test. */
static const char *const k_ops_destructive[] = {
    "zcl_rpc",                    /* arbitrary RPC — skip by default */
};

/* Canonical self_test args for tools that need a required param but
 * have a known-safe probe value. */
static const struct {
    const char *tool;
    const char *args_json;
} k_ops_self_test_args[] = {
    /* zcl_profile sleeps duration_ms per call — clamp to 100ms so
     * the full self_test sweep doesn't balloon by a second. */
    { "zcl_profile", "{\"duration_ms\":100,\"top_n\":3}" },
};

void mcp_register_ops(void)
{
    /* Derive p_state.subsystem schema from the live g_dumpers registry so
     * adding a new *_dump_state_json subsystem in diagnostics_controller.c
     * automatically updates the MCP-visible enum and description with
     * zero further plumbing. */
    diagnostics_subsystems_csv(g_state_subsystems_csv,
                               sizeof(g_state_subsystems_csv));
    snprintf(g_state_subsystem_desc, sizeof(g_state_subsystem_desc),
             "Subsystem name (one of: %s)", g_state_subsystems_csv);
    p_state[0].enum_csv    = g_state_subsystems_csv;
    p_state[0].description = g_state_subsystem_desc;

    for (size_t i = 0; i < PARAM_COUNT(k_routes); i++)
        mcp_router_register(&k_routes[i]);
    for (size_t i = 0;
         i < PARAM_COUNT(k_ops_destructive); i++)
        mcp_router_set_flags(k_ops_destructive[i],
                             MCP_TOOL_FLAG_DESTRUCTIVE);
    for (size_t i = 0;
         i < PARAM_COUNT(k_ops_self_test_args); i++)
        mcp_router_set_self_test_args(k_ops_self_test_args[i].tool,
                                       k_ops_self_test_args[i].args_json);
}
