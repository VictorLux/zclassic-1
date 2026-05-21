/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * MCP chain controller: block, chain, UTXO commitment, sync, MMB.
 */

#include "../controllers.h"
#include "../router.h"
#include "../rpc_client.h"
#include "../rpc_params.h"

#include "json/json.h"
#include "util/log_macros.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* ── Handlers ───────────────────────────────────────────────── */

DEFINE_PT(h_zcl_getblockcount,     "getblockcount",     "mcp.chain")
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
    const struct json_value *cv = json_get(req->args, "count");
    int64_t count = cv ? json_get_int(cv) : 50;
    char params[32];
    snprintf(params, sizeof(params), "[%lld]", (long long)count);
    return mcp_return_rpc_body(res, mcp_node_rpc("getreorghistory", params),
                                "getreorghistory", "mcp.chain");
}

static int h_zcl_utxo_audit(const struct mcp_request *req,
                            struct mcp_response *res)
{
    const struct json_value *remote_v = json_get(req->args, "remote_sha3");
    const struct json_value *source_v = json_get(req->args, "source");
    const struct json_value *height_v = json_get(req->args, "remote_height");
    const char *remote = remote_v ? json_get_str(remote_v) : NULL;
    const char *source = source_v ? json_get_str(source_v) : NULL;

    struct mcp_params p;
    mcp_params_init(&p);
    if (remote && remote[0]) {
        mcp_params_push_str(&p, remote);
        mcp_params_push_int(&p, height_v ? json_get_int(height_v) : 0);
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
    const struct json_value *verb = json_get(req->args, "verbose");
    int verbose = verb ? (int)json_get_int(verb) : 1;
    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, txid);
    mcp_params_push_int(&p, verbose);
    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("getrawtransaction", params) : NULL;
    free(params);
    return mcp_return_rpc_body_ctx(res, out, "getrawtransaction", "mcp.chain",
                                   "txid=%s", txid ? txid : "(null)");
}

static int h_zcl_getblock(const struct mcp_request *req, struct mcp_response *res)
{
    const char *id_str = json_get_str(json_get(req->args, "block_id"));
    const struct json_value *verb = json_get(req->args, "verbosity");
    int verbosity = verb ? (int)json_get_int(verb) : 1;

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
      "Current block height.", NULL, 0, h_zcl_getblockcount },
    { "zcl_chain_tip", "chain",
      "Active chain tip in one call: hash, height, time, age_seconds, "
      "work, bits, difficulty. Power-user shortcut that bundles "
      "getbestblockhash + getblockheader + chainwork.",
      NULL, 0, h_zcl_chain_tip },
    { "zcl_getblock", "chain",
      "Get block by height or hash.",
      p_getblock, PARAM_COUNT(p_getblock), h_zcl_getblock },
    { "zcl_getrawtransaction", "chain",
      "Transaction by id. verbose=1 decodes, verbose=0 returns hex.",
      p_getrawtx, PARAM_COUNT(p_getrawtx),
      h_zcl_getrawtransaction },
    { "zcl_getblockchaininfo", "chain",
      "Chain state: height, best block, difficulty, chain work, value pools.",
      NULL, 0, h_zcl_getblockchaininfo },
    { "zcl_syncstate", "chain",
      "Sync state machine: phase, progress, header/block/UTXO status.",
      NULL, 0, h_zcl_syncstate },
    { "zcl_validationstatus", "chain",
      "Background validation: verified height, sigs, proofs, blocks/sec.",
      NULL, 0, h_zcl_validationstatus },
    { "zcl_dataintegrity", "chain",
      "SHA3-256 hashes over all consensus tables.",
      NULL, 0, h_zcl_dataintegrity },
    { "zcl_mmb", "chain",
      "Merkle Mountain Belt root. FlyClient chain verification.",
      NULL, 0, h_zcl_mmb },
    { "zcl_utxocommitment", "chain",
      "SHA3-256 over entire UTXO set in canonical order.",
      NULL, 0, h_zcl_utxocommitment },
    { "zcl_utxo_audit", "chain",
      "Post-IBD UTXO drift audit. Computes local commitment and optionally compares a trusted peer SHA3.",
      p_utxo_audit, PARAM_COUNT(p_utxo_audit),
      h_zcl_utxo_audit },
    { "zcl_hodlwave", "chain",
      "UTXO age distribution: 10 buckets from 24h to 5y+.",
      NULL, 0, h_zcl_hodlwave },
    { "zcl_reorg_history", "chain",
      "Recent chain.reorg_* events (start, disconnect_failed, "
      "recovery_complete). Power-user lens on chain stability.",
      p_reorg_history, PARAM_COUNT(p_reorg_history),
      h_zcl_reorg_history },
};

/* Canonical self_test args. zcl_getblock requires a height — pick "1"
 * because it exists on every synced node. */
static const struct {
    const char *tool;
    const char *args_json;
} k_chain_self_test_args[] = {
    { "zcl_getblock", "{\"block_id\":\"1\"}" },
};

void mcp_register_chain(void)
{
    for (size_t i = 0; i < PARAM_COUNT(k_routes); i++)
        mcp_router_register(&k_routes[i]);
    for (size_t i = 0;
         i < PARAM_COUNT(k_chain_self_test_args);
         i++)
        mcp_router_set_self_test_args(k_chain_self_test_args[i].tool,
                                       k_chain_self_test_args[i].args_json);
}
