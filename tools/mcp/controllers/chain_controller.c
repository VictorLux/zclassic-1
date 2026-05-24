/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * MCP chain controller: block, chain, UTXO commitment, sync, MMB.
 */

#include "platform/time_compat.h"
#include "../controllers.h"
#include "../router.h"
#include "../rpc_client.h"
#include "../rpc_params.h"

#include "adapters/inbound/shadow_feeder_global.h"
#include "adapters/outbound/persistence/block_log_file.h"
#include "adapters/outbound/persistence/block_log_legacy.h"
#include "application/operations/diff_with_legacy_shadow.h"
#include "controllers/chain_projection.h"
#include "json/json.h"
#include "mcp/metrics.h"
#include "ports/block_log_port.h"
#include "services/header_admit_stage.h"
#include "util/log_macros.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>

/* ── Handlers ───────────────────────────────────────────────── */

static int h_zcl_getblockcount(const struct mcp_request *req,
                               struct mcp_response *res)
{
    (void)req;

    int64_t height = chain_projection_best_block_height();
    if (height >= 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", (long long)height);
        res->body = strdup(buf);
        if (!res->body) {
            res->error = MCP_ERR_HANDLER_FAILED;
            snprintf(res->error_message, sizeof(res->error_message),
                     "getblockcount projection response alloc failed");
            LOG_ERR("mcp.chain", "getblockcount projection response alloc failed");
        }
        return 0;
    }

    fprintf(stderr,  // obs-ok:mcp-chain-projection-fallback
            "[mcp.chain] projection miss: getblockcount rpc fallback\n");
    return mcp_return_rpc_body(res, mcp_node_rpc("getblockcount", NULL),
                               "getblockcount", "mcp.chain");
}

DEFINE_PT(h_zcl_chain_tip,         "getchaintip",       "mcp.chain")
DEFINE_PT(h_zcl_getblockchaininfo, "getblockchaininfo", "mcp.chain")
DEFINE_PT(h_zcl_syncstate,         "syncstate",         "mcp.chain")
DEFINE_PT(h_zcl_validationstatus,  "validationstatus",  "mcp.chain")
DEFINE_PT(h_zcl_dataintegrity,     "getdataintegrity",  "mcp.chain")
DEFINE_PT(h_zcl_mmb,               "getmmrroot",        "mcp.chain")
DEFINE_PT(h_zcl_utxocommitment,    "getutxocommitment", "mcp.chain")
DEFINE_PT(h_zcl_hodlwave,          "gethodlwave",       "mcp.chain")

static int h_zcl_reorg_history(const struct mcp_request *req,
                                struct mcp_response *res)
{
    char params[32];
    snprintf(params, sizeof(params), "[%lld]",
             (long long)json_get_int_or(req->args, "count", 50));
    return mcp_return_rpc_body(res, mcp_node_rpc("getreorghistory", params),
                                "getreorghistory", "mcp.chain");
}

static const char *diff_status_name(enum diff_with_legacy_shadow_status s)
{
    switch (s) {
    case DIFF_STATUS_CONVERGED:        return "CONVERGED";
    case DIFF_STATUS_DIVERGENT:        return "DIVERGENT";
    case DIFF_STATUS_SHADOW_MISSING:   return "SHADOW_MISSING";
    case DIFF_STATUS_PRIMARY_MISSING:  return "PRIMARY_MISSING";
    case DIFF_STATUS_EMPTY_RANGE:      return "EMPTY_RANGE";
    }
    return "UNKNOWN";
}

static int h_zcl_diff_with_legacy_shadow(const struct mcp_request *req,
                                          struct mcp_response *res)
{
    /* Inputs (all optional). */
    int64_t start_h = json_get_int_or(req->args, "start_height", 0);
    int64_t count   = json_get_int_or(req->args, "count", 256);
    const char *legacy_dir = json_get_str_or(req->args, "legacy_datadir", NULL);
    if (!legacy_dir || !legacy_dir[0]) {
        static char def_legacy[1024];
        const char *home = getenv("HOME");
        if (home && home[0]) {
            snprintf(def_legacy, sizeof def_legacy, "%s/.zclassic", home);
            legacy_dir = def_legacy;
        } else {
            res->error = MCP_ERR_HANDLER_FAILED;
            snprintf(res->error_message, sizeof(res->error_message),
                     "legacy_datadir not provided and $HOME is unset");
            LOG_ERR("mcp.chain", "diff_with_legacy_shadow: no legacy_datadir");
            return 0;
        }
    }

    if (start_h < 0)          start_h = 0;
    if (count < 1)            count = 1;
    if (count > 10000)        count = 10000;  /* cap per call */
    int64_t end_h = start_h + count - 1;
    if (end_h > UINT32_MAX)   end_h = UINT32_MAX;

    /* Shadow side: must be active. The path is implied by the boot
     * convention (<node_datadir>/blocks.shadow), so we re-derive it
     * from the node datadir rather than threading state through. */
    if (!shadow_feeder_global_is_active()) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "shadow feeder not active — start node with -shadow");
        LOG_ERR("mcp.chain", "diff_with_legacy_shadow: shadow inactive");
        return 0;
    }
    const char *node_datadir = mcp_rpc_client_datadir();
    if (!node_datadir || !node_datadir[0]) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "node datadir not initialized");
        LOG_ERR("mcp.chain", "diff_with_legacy_shadow: no node datadir");
        return 0;
    }
    char shadow_dir[1100];
    snprintf(shadow_dir, sizeof shadow_dir, "%s/blocks.shadow", node_datadir);

    /* Open both ports. */
    struct block_log_file *shadow_h = NULL;
    struct block_log_port  shadow_p = {0};
    struct zcl_result rs = block_log_file_open(shadow_dir, &shadow_h, &shadow_p);
    if (!rs.ok) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "shadow log open(%s) failed: code=%d %s",
                 shadow_dir, rs.code, rs.message);
        LOG_ERR("mcp.chain", "diff_with_legacy_shadow: %s",
                res->error_message);
        return 0;
    }

    struct block_log_legacy *legacy_h = NULL;
    struct block_log_port    legacy_p = {0};
    struct zcl_result rl = block_log_legacy_open(legacy_dir, &legacy_h,
                                                  &legacy_p);
    if (!rl.ok) {
        block_log_file_close(shadow_h);
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "legacy log open(%s) failed: code=%d %s",
                 legacy_dir, rl.code, rl.message);
        LOG_ERR("mcp.chain", "diff_with_legacy_shadow: %s",
                res->error_message);
        return 0;
    }

    /* Run the diff. */
    struct diff_with_legacy_shadow_inputs in = {
        .primary      = &legacy_p,
        .shadow       = &shadow_p,
        .start_height = (uint32_t)start_h,
        .end_height   = (uint32_t)end_h,
    };
    struct diff_with_legacy_shadow_report report = {0};
    struct zcl_result rd = diff_with_legacy_shadow(&in, &report);

    /* Update Prometheus gauges. divergence_count == 1 for any non-CONVERGED
     * /non-EMPTY_RANGE result (the use case stops at the first divergence,
     * so we can't return an exact count without a second walk). For the
     * I-9 soak the binary "any divergence in window" signal is what we
     * actually alert on. */
    int64_t div_count = 0;
    int64_t first_div = -1;
    if (rd.ok) {
        if (report.status != DIFF_STATUS_CONVERGED &&
            report.status != DIFF_STATUS_EMPTY_RANGE) {
            div_count = 1;
            first_div = (int64_t)report.first_divergent_height;
        }
    }
    mcp_metrics_set_shadow_divergence(div_count, first_div, (int64_t)platform_time_wall_time_t());

    /* Build response body. shadow_dir + legacy_dir are bounded by the
     * snprintf caps above; the rest is small ints + a short status
     * string. 4 KB is plenty. */
    char buf[4096];
    int written;
    if (!rd.ok) {
        written = snprintf(buf, sizeof buf,
                "{\"error\":{\"code\":%d,\"message\":\"%s\"},"
                "\"start_height\":%lld,\"end_height\":%lld,"
                "\"shadow_dir\":\"%s\",\"legacy_datadir\":\"%s\"}",
                rd.code, rd.message,
                (long long)start_h, (long long)end_h,
                shadow_dir, legacy_dir);
    } else {
        written = snprintf(buf, sizeof buf,
                "{\"status\":\"%s\","
                "\"checked_count\":%u,"
                "\"first_divergent_height\":%lld,"
                "\"primary_tip\":%u,"
                "\"shadow_tip\":%u,"
                "\"start_height\":%lld,"
                "\"end_height\":%lld,"
                "\"shadow_dir\":\"%s\","
                "\"legacy_datadir\":\"%s\"}",
                diff_status_name(report.status),
                report.checked_count,
                (long long)first_div,
                report.primary_tip,
                report.shadow_tip,
                (long long)start_h, (long long)end_h,
                shadow_dir, legacy_dir);
    }
    char *body = (written > 0 && written < (int)sizeof buf)
                     ? strdup(buf) : NULL;

    block_log_legacy_close(legacy_h);
    block_log_file_close(shadow_h);

    if (!body) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "diff_with_legacy_shadow: asprintf body");
        LOG_ERR("mcp.chain", "diff_with_legacy_shadow: asprintf body");
        return 0;
    }
    res->body = body;
    return 0;
}

/* ── S-11 mini-diff: staged header_admit vs in-memory active_chain ──── */

static const char *
header_admit_diff_status_name(enum header_admit_diff_status s)
{
    switch (s) {
    case HEADER_ADMIT_DIFF_CONVERGED:    return "CONVERGED";
    case HEADER_ADMIT_DIFF_DIVERGENT:    return "DIVERGENT";
    case HEADER_ADMIT_DIFF_LOG_AHEAD:    return "LOG_AHEAD";
    case HEADER_ADMIT_DIFF_CHAIN_AHEAD:  return "CHAIN_AHEAD";
    case HEADER_ADMIT_DIFF_EMPTY:        return "EMPTY";
    case HEADER_ADMIT_DIFF_NOT_READY:    return "NOT_READY";
    }
    return "UNKNOWN";
}

/* Render 32-byte hash as 64-char lowercase hex. Caller owns the buffer
 * which must hold at least 65 bytes. */
static void hex32(const uint8_t *in, char *out)
{
    static const char *hx = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[2*i]   = hx[(in[i] >> 4) & 0xF];
        out[2*i+1] = hx[in[i]        & 0xF];
    }
    out[64] = 0;
}

static int h_zcl_diff_staged_header_admit(const struct mcp_request *req,
                                           struct mcp_response *res)
{
    int64_t start_h = json_get_int_or(req->args, "start_height", -1);
    int64_t end_h   = json_get_int_or(req->args, "end_height",   -1);

    if (start_h < -1)            start_h = -1;
    if (end_h   < -1)            end_h   = -1;
    if (start_h > (int64_t)INT32_MAX) start_h = INT32_MAX;
    if (end_h   > (int64_t)INT32_MAX) end_h   = INT32_MAX;

    struct header_admit_diff_report rep;
    if (!header_admit_stage_diff((int32_t)start_h, (int32_t)end_h, &rep)) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "diff_staged_header_admit: bad inputs");
        LOG_ERR("mcp.chain", "diff_staged_header_admit: bad inputs");
        return 0;
    }

    /* Body: scalars + an array of up to HEADER_ADMIT_DIFF_MAX_SAMPLES
     * samples. Each sample is ≤ ~260 bytes (two hex hashes + flags), so
     * a 16 KB buffer comfortably holds 32 samples + header. */
    char buf[16384];
    int n = snprintf(buf, sizeof buf,
            "{\"status\":\"%s\","
            "\"start_height\":%d,\"end_height\":%d,"
            "\"checked_count\":%d,"
            "\"match_count\":%d,"
            "\"mismatch_count\":%d,"
            "\"missing_in_log_count\":%d,"
            "\"missing_in_chain_count\":%d,"
            "\"first_divergent_height\":%d,"
            "\"log_max_height\":%d,"
            "\"chain_tip_height\":%d,"
            "\"cursor\":%d,"
            "\"samples\":[",
            header_admit_diff_status_name(rep.status),
            rep.start_height, rep.end_height,
            rep.checked_count, rep.match_count, rep.mismatch_count,
            rep.missing_in_log_count, rep.missing_in_chain_count,
            rep.first_divergent_height,
            rep.log_max_height, rep.chain_tip_height, rep.cursor);
    if (n < 0 || n >= (int)sizeof buf) goto body_too_big;

    for (int i = 0; i < rep.sample_count; i++) {
        const struct header_admit_diff_sample *s = &rep.samples[i];
        char log_hex[65]   = {0};
        char chain_hex[65] = {0};
        if (s->log_present)   hex32(s->log_hash,   log_hex);
        if (s->chain_present) hex32(s->chain_hash, chain_hex);
        int m = snprintf(buf + n, sizeof(buf) - n,
                "%s{\"height\":%d,"
                "\"log_present\":%s,\"chain_present\":%s,"
                "\"log_hash\":\"%s\",\"chain_hash\":\"%s\"}",
                (i == 0) ? "" : ",",
                s->height,
                s->log_present   ? "true" : "false",
                s->chain_present ? "true" : "false",
                log_hex, chain_hex);
        if (m < 0 || (n + m) >= (int)sizeof buf) goto body_too_big;
        n += m;
    }

    int tail = snprintf(buf + n, sizeof(buf) - n, "]}");
    if (tail < 0 || (n + tail) >= (int)sizeof buf) goto body_too_big;

    char *body = strdup(buf);
    if (!body) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "diff_staged_header_admit: strdup");
        LOG_ERR("mcp.chain", "diff_staged_header_admit: strdup");
        return 0;
    }
    res->body = body;
    return 0;

body_too_big:
    res->error = MCP_ERR_HANDLER_FAILED;
    snprintf(res->error_message, sizeof(res->error_message),
             "diff_staged_header_admit: body buffer overflow");
    LOG_ERR("mcp.chain", "diff_staged_header_admit: body buffer overflow");
    return 0;
}

/* ── Phase 4b: zcl_utxo_projection_diff (24h cutover soak gate) ────
 *
 * Compares the projection's SHA3 commitment to the legacy coins.db
 * commitment (the one served by `getutxocommitment` RPC). When this
 * returns `match: true` continuously for 24 hours on a live node,
 * the 4b-cutover PR is unblocked: the legacy update_coins SQLite
 * write can be disabled and the projection becomes authoritative.
 *
 * The legacy commitment requires a SELECT walk of the entire utxo
 * table — bounded but not free. The projection commitment walks its
 * own table the same way. Both can take 1-2 seconds on a fully-synced
 * tip. We return both hashes + a boolean so operators / soak scripts
 * can compute deltas without re-running. */

#include "storage/utxo_projection.h"

static int h_zcl_utxo_projection_diff(const struct mcp_request *req,
                                      struct mcp_response *res)
{
    (void)req;

    /* Legacy commitment via the existing in-process RPC. The RPC
     * returns a JSON object {sha3_hash, height, utxo_count, ...}. We
     * extract sha3_hash without a full JSON parse — same pattern as
     * h_zcl_diff_with_legacy in diagnostics_controller. */
    char *legacy_body = mcp_node_rpc("getutxocommitment", NULL);
    if (!legacy_body) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "getutxocommitment RPC returned null — chain not loaded?");
        LOG_ERR("mcp.chain", "utxo_projection_diff: getutxocommitment null");
        return 0;
    }
    char legacy_hex[65] = {0};
    {
        const char *p = strstr(legacy_body, "\"sha3_hash\":\"");
        if (p) {
            p += strlen("\"sha3_hash\":\"");
            size_t k = 0;
            while (k < 64 && p[k] && p[k] != '"') {
                legacy_hex[k] = p[k];
                k++;
            }
            legacy_hex[k] = '\0';
        }
    }
    int64_t legacy_height = 0, legacy_count = 0;
    {
        const char *p = strstr(legacy_body, "\"height\":");
        if (p) legacy_height = strtoll(p + strlen("\"height\":"), NULL, 10);
        p = strstr(legacy_body, "\"utxo_count\":");
        if (p) legacy_count  = strtoll(p + strlen("\"utxo_count\":"), NULL, 10);
    }
    free(legacy_body);

    /* Projection commitment via in-process call. NULL handle means
     * Phase 4b shadow mode is not wired (legitimate during -mcp
     * helper invocations); report that explicitly. */
    utxo_projection_t *p = utxo_projection_get_global();
    if (!p) {
        char buf[512];
        snprintf(buf, sizeof(buf),
            "{\"match\":false,\"reason\":\"projection_not_open\","
             "\"legacy_sha3\":\"%s\","
             "\"legacy_height\":%lld,"
             "\"legacy_utxo_count\":%lld}",
            legacy_hex, (long long)legacy_height, (long long)legacy_count);
        res->body = strdup(buf);
        if (!res->body) {
            res->error = MCP_ERR_HANDLER_FAILED;
            snprintf(res->error_message, sizeof(res->error_message),
                     "utxo_projection_diff: strdup");
            LOG_ERR("mcp.chain", "utxo_projection_diff: strdup");
        }
        return 0;
    }

    /* One last catch_up so we don't false-positive on a transient
     * "events emitted but not yet consumed" gap. */
    (void)utxo_projection_catch_up(p);

    uint8_t proj_hash[32];
    if (utxo_projection_commitment(p, proj_hash) != 0) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "utxo_projection_commitment failed");
        LOG_ERR("mcp.chain", "utxo_projection_diff: commitment failed");
        return 0;
    }
    char proj_hex[65] = {0};
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        proj_hex[2*i]     = hx[(proj_hash[i] >> 4) & 0xF];
        proj_hex[2*i + 1] = hx[ proj_hash[i]       & 0xF];
    }

    uint64_t proj_count = utxo_projection_count(p);
    bool match = (legacy_hex[0] != '\0' &&
                  strcmp(legacy_hex, proj_hex) == 0);

    char buf[1024];
    snprintf(buf, sizeof(buf),
        "{\"match\":%s,"
         "\"legacy_sha3\":\"%s\","
         "\"projection_sha3\":\"%s\","
         "\"legacy_height\":%lld,"
         "\"legacy_utxo_count\":%lld,"
         "\"projection_utxo_count\":%llu}",
        match ? "true" : "false",
        legacy_hex, proj_hex,
        (long long)legacy_height, (long long)legacy_count,
        (unsigned long long)proj_count);
    res->body = strdup(buf);
    if (!res->body) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "utxo_projection_diff: strdup body");
        LOG_ERR("mcp.chain", "utxo_projection_diff: strdup body");
    }
    return 0;
}

static int h_zcl_utxo_audit(const struct mcp_request *req,
                            struct mcp_response *res)
{
    const char *remote = json_get_str_or(req->args, "remote_sha3", NULL);
    const char *source = json_get_str_or(req->args, "source",      NULL);

    struct mcp_params p;
    mcp_params_init(&p);
    if (remote && remote[0]) {
        mcp_params_push_str(&p, remote);
        mcp_params_push_int(&p, json_get_int_or(req->args, "remote_height", 0));
        mcp_params_push_str(&p, source && source[0] ? source : "trusted-peer");
    }
    char *params = mcp_params_to_json(&p);
    char *out = mcp_node_rpc("getutxoaudit", params);
    free(params);
    return mcp_return_rpc_body(res, out, "getutxoaudit", "mcp.chain");
}

static int h_zcl_getrawtransaction(const struct mcp_request *req,
                                    struct mcp_response *res)
{
    const char *txid = json_get_str(json_get(req->args, "txid"));
    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, txid);
    mcp_params_push_int(&p, json_get_int_or(req->args, "verbose", 1));
    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("getrawtransaction", params) : NULL;
    free(params);
    return mcp_return_rpc_body_ctx(res, out, "getrawtransaction", "mcp.chain",
                                   "txid=%s", txid ? txid : "(null)");
}

static int h_zcl_getblock(const struct mcp_request *req, struct mcp_response *res)
{
    const char *id_str = json_get_str(json_get(req->args, "block_id"));
    int verbosity = (int)json_get_int_or(req->args, "verbosity", 1);

    bool is_num = id_str && id_str[0];
    for (const char *c = id_str; is_num && *c; c++)
        if (*c < '0' || *c > '9') is_num = false;

    char clean[128] = {0};
    const char *hash_str = id_str;
    if (is_num) {
        struct mcp_params ph;
        mcp_params_init(&ph);
        mcp_params_push_int(&ph, id_str ? atoll(id_str) : 0);
        char *php = mcp_params_to_json(&ph);
        char *hash = php ? mcp_node_rpc("getblockhash", php) : NULL;
        free(php);
        if (!hash)
            return mcp_return_rpc_body_ctx(res, NULL, "getblockhash", "mcp.chain",
                                           "height=%s", id_str ? id_str : "(null)");
        size_t ci = 0;
        for (size_t i = 0; hash[i] && ci < 127; i++)
            if (hash[i] != '"' && hash[i] != '\n') clean[ci++] = hash[i];
        clean[ci] = 0;
        free(hash);
        hash_str = clean;
    }

    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, hash_str);
    mcp_params_push_int(&p, verbosity);
    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("getblock", params) : NULL;
    free(params);
    return mcp_return_rpc_body_ctx(res, out, "getblock", "mcp.chain",
                                   "id=%s", id_str ? id_str : "(null)");
}

/* ── Route table ─────────────────────────────────────────────── */

static const struct mcp_param_spec p_getblock[] = {
    { "block_id",  MCP_PARAM_STR, true,  "Height or hash",
      0, 0, 1, 128, NULL, NULL },
    { "verbosity", MCP_PARAM_INT, false, "0=hex, 1=JSON, 2=JSON+tx",
      0, 2, 0, 0, NULL, "1" },
};

static const struct mcp_param_spec p_getrawtx[] = {
    { "txid",    MCP_PARAM_STR, true,  "Transaction id (hex)",
      0, 0, 1, 128, NULL, NULL },
    { "verbose", MCP_PARAM_INT, false, "0=hex, 1=JSON",
      0, 1, 0, 0, NULL, "1" },
};

static const struct mcp_param_spec p_reorg_history[] = {
    { "count", MCP_PARAM_INT, false,
      "Max reorg events to return (1..1024)",
      1, 1024, 0, 0, NULL, "50" },
};

static const struct mcp_param_spec p_diff_shadow[] = {
    { "start_height", MCP_PARAM_INT, false,
      "First height to compare (inclusive).",
      0, 100000000, 0, 0, NULL, "0" },
    { "count", MCP_PARAM_INT, false,
      "How many heights to compare (capped at 10000 per call).",
      1, 10000, 0, 0, NULL, "256" },
    { "legacy_datadir", MCP_PARAM_STR, false,
      "Legacy zclassicd data directory. Defaults to $HOME/.zclassic.",
      0, 0, 0, 1023, NULL, NULL },
};

static const struct mcp_param_spec p_diff_staged[] = {
    { "start_height", MCP_PARAM_INT, false,
      "First height to compare (inclusive). -1 = 0.",
      -1, 100000000, 0, 0, NULL, "-1" },
    { "end_height", MCP_PARAM_INT, false,
      "Last height to compare (inclusive). -1 = min(log_max, chain_tip). "
      "Range hard-capped at 10000 heights per call.",
      -1, 100000000, 0, 0, NULL, "-1" },
};

static const struct mcp_param_spec p_utxo_audit[] = {
    { "remote_sha3", MCP_PARAM_STR, false,
      "Trusted peer SHA3 commitment to compare against.",
      0, 0, 64, 64, NULL, NULL },
    { "remote_height", MCP_PARAM_INT, false,
      "Trusted peer height for the commitment.",
      0, 100000000, 0, 0, NULL, "0" },
    { "source", MCP_PARAM_STR, false,
      "Trusted peer or operator label.",
      0, 0, 0, 63, NULL, "\"trusted-peer\"" },
};

static const struct mcp_tool_route k_routes[] = {
    { "zcl_getblockcount", "chain",
      "Current block height.", NULL, 0, h_zcl_getblockcount, 0, NULL },
    { "zcl_chain_tip", "chain",
      "Active chain tip in one call: hash, height, time, age_seconds, "
      "work, bits, difficulty. Power-user shortcut that bundles "
      "getbestblockhash + getblockheader + chainwork.",
      NULL, 0, h_zcl_chain_tip, 0, NULL },
    { "zcl_getblock", "chain",
      "Get block by height or hash.",
      p_getblock, PARAM_COUNT(p_getblock), h_zcl_getblock,
      /* required block_id, but height "1" exists on every synced node. */
      .self_test_args = "{\"block_id\":\"1\"}" },
    { "zcl_getrawtransaction", "chain",
      "Transaction by id. verbose=1 decodes, verbose=0 returns hex.",
      p_getrawtx, PARAM_COUNT(p_getrawtx),
      h_zcl_getrawtransaction, 0, NULL },
    { "zcl_getblockchaininfo", "chain",
      "Chain state: height, best block, difficulty, chain work, value pools.",
      NULL, 0, h_zcl_getblockchaininfo, 0, NULL },
    { "zcl_syncstate", "chain",
      "Sync state machine: phase, progress, header/block/UTXO status.",
      NULL, 0, h_zcl_syncstate, 0, NULL },
    { "zcl_validationstatus", "chain",
      "Background validation: verified height, sigs, proofs, blocks/sec.",
      NULL, 0, h_zcl_validationstatus, 0, NULL },
    { "zcl_dataintegrity", "chain",
      "SHA3-256 hashes over all consensus tables.",
      NULL, 0, h_zcl_dataintegrity, 0, NULL },
    { "zcl_mmb", "chain",
      "Merkle Mountain Belt root. FlyClient chain verification.",
      NULL, 0, h_zcl_mmb, 0, NULL },
    { "zcl_utxocommitment", "chain",
      "SHA3-256 over entire UTXO set in canonical order.",
      NULL, 0, h_zcl_utxocommitment, 0, NULL },
    { "zcl_utxo_audit", "chain",
      "Post-IBD UTXO drift audit. Computes local commitment and optionally compares a trusted peer SHA3.",
      p_utxo_audit, PARAM_COUNT(p_utxo_audit),
      h_zcl_utxo_audit, 0, NULL },
    { "zcl_hodlwave", "chain",
      "UTXO age distribution: 10 buckets from 24h to 5y+.",
      NULL, 0, h_zcl_hodlwave, 0, NULL },
    { "zcl_reorg_history", "chain",
      "Recent chain.reorg_* events (start, disconnect_failed, "
      "recovery_complete). Power-user lens on chain stability.",
      p_reorg_history, PARAM_COUNT(p_reorg_history),
      h_zcl_reorg_history, 0, NULL },
    { "zcl_diff_with_legacy_shadow", "chain",
      "Compare the legacy block_log against the in-process shadow "
      "log over a height window. Requires -shadow at startup. "
      "Updates zcl_shadow_divergence_count Prometheus gauge.",
      p_diff_shadow, PARAM_COUNT(p_diff_shadow),
      h_zcl_diff_with_legacy_shadow, 0, NULL },
    { "zcl_diff_staged_header_admit", "chain",
      "S-11 mini-diff: compare the staged header_admit_log (S-2 output) "
      "against the live in-memory active_chain over a height window. "
      "Reports CONVERGED / DIVERGENT / LOG_AHEAD / CHAIN_AHEAD with "
      "counts and up to 32 sample mismatches. Read-only diagnostic.",
      p_diff_staged, PARAM_COUNT(p_diff_staged),
      h_zcl_diff_staged_header_admit, 0, NULL },
    { "zcl_utxo_projection_diff", "chain",
      "Phase 4b shadow-diff gate: SHA3 commitment of the legacy "
      "coins.db UTXO set vs the same commitment derived from the "
      "event-log-driven utxo_projection. Returns {match, "
      "legacy_sha3, projection_sha3, legacy_height, legacy_utxo_count, "
      "projection_utxo_count}. The 4b-cutover PR (which disables the "
      "legacy SQLite write and makes the projection authoritative) is "
      "gated on 24 hours of `match: true` on a live node.",
      NULL, 0, h_zcl_utxo_projection_diff, 0, NULL },
};

void mcp_register_chain(void)
{
    for (size_t i = 0; i < PARAM_COUNT(k_routes); i++)
        mcp_router_register(&k_routes[i]);
}
