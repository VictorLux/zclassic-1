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

/* ── Helpers ───────────────────────────────────────────────── */

#define DEFINE_PT(name, rpc)                                                   \
    static int name(const struct mcp_request *req, struct mcp_response *res)  \
    {                                                                          \
        (void)req;                                                             \
        char *out = mcp_node_rpc(rpc, NULL);                                   \
        if (!out) {                                                            \
            res->error = MCP_ERR_HANDLER_FAILED;                               \
            snprintf(res->error_message, sizeof(res->error_message),           \
                     "RPC %s returned null", rpc);                             \
            LOG_ERR("mcp.chain", "RPC %s returned null", rpc);                 \
        }                                                                      \
        res->body = out;                                                       \
        return 0;                                                              \
    }

/* ── Handlers ───────────────────────────────────────────────── */

DEFINE_PT(h_zcl_getblockcount,     "getblockcount")
DEFINE_PT(h_zcl_getblockchaininfo, "getblockchaininfo")
DEFINE_PT(h_zcl_syncstate,         "syncstate")
DEFINE_PT(h_zcl_validationstatus,  "validationstatus")
DEFINE_PT(h_zcl_dataintegrity,     "getdataintegrity")
DEFINE_PT(h_zcl_mmb,               "getmmrroot")
DEFINE_PT(h_zcl_utxocommitment,    "getutxocommitment")
DEFINE_PT(h_zcl_hodlwave,          "gethodlwave")

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
    if (!out) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "RPC getrawtransaction failed: txid=%s", txid ? txid : "(null)");
        LOG_ERR("mcp.chain", "getrawtransaction failed: txid=%s", txid ? txid : "(null)");
    }
    res->body = out;
    return 0;
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
        if (!hash) {
            res->error = MCP_ERR_HANDLER_FAILED;
            snprintf(res->error_message, sizeof(res->error_message),
                     "RPC getblockhash failed: height=%s", id_str);
            LOG_ERR("mcp.chain", "getblockhash failed: height=%s", id_str);
        }
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
    if (!out) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "RPC getblock failed: id=%s", id_str ? id_str : "(null)");
        LOG_ERR("mcp.chain", "getblock failed: id=%s", id_str ? id_str : "(null)");
    }
    res->body = out;
    return 0;
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

static const struct mcp_tool_route k_routes[] = {
    { "zcl_getblockcount", "chain",
      "Current block height.", NULL, 0, h_zcl_getblockcount },
    { "zcl_getblock", "chain",
      "Get block by height or hash.",
      p_getblock, sizeof(p_getblock) / sizeof(p_getblock[0]), h_zcl_getblock },
    { "zcl_getrawtransaction", "chain",
      "Transaction by id. verbose=1 decodes, verbose=0 returns hex.",
      p_getrawtx, sizeof(p_getrawtx) / sizeof(p_getrawtx[0]),
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
    { "zcl_hodlwave", "chain",
      "UTXO age distribution: 10 buckets from 24h to 5y+.",
      NULL, 0, h_zcl_hodlwave },
};

void mcp_register_chain(void)
{
    for (size_t i = 0; i < sizeof(k_routes) / sizeof(k_routes[0]); i++)
        mcp_router_register(&k_routes[i]);
}
