/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * MCP app controller: names, messaging, tokens, file market, atomic
 * swaps — everything built on top of the base chain. */

#include "../controllers.h"
#include "../router.h"
#include "../rpc_client.h"
#include "../rpc_params.h"

#include "json/json.h"
#include "util/log_macros.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFINE_PT(name, rpc)                                                   \
    static int name(const struct mcp_request *req, struct mcp_response *res)  \
    {                                                                          \
        (void)req;                                                             \
        char *out = mcp_node_rpc(rpc, NULL);                                   \
        if (!out) {                                                            \
            res->error = MCP_ERR_HANDLER_FAILED;                               \
            snprintf(res->error_message, sizeof(res->error_message),           \
                     "RPC %s returned null", rpc);                             \
            LOG_ERR("mcp.app", "RPC %s returned null", rpc);                   \
        }                                                                      \
        res->body = out;                                                       \
        return 0;                                                              \
    }

/* ── ZSLP tokens ────────────────────────────────────────────── */

DEFINE_PT(h_zcl_tokens, "zslp_listtokens")

/* ── Names (ZNAM) ───────────────────────────────────────────── */

DEFINE_PT(h_zcl_name_list, "name_list")

static int h_zcl_name_resolve(const struct mcp_request *req, struct mcp_response *res)
{
    const char *n = json_get_str(json_get(req->args, "name"));
    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, n);
    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("name_resolve", params) : NULL;
    free(params);
    if (!out) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "RPC name_resolve failed: name=%s", n ? n : "(null)");
        LOG_ERR("mcp.app", "name_resolve failed: name=%s", n ? n : "(null)");
    }
    res->body = out;
    return 0;
}

static int h_zcl_name_register(const struct mcp_request *req, struct mcp_response *res)
{
    const char *n = json_get_str(json_get(req->args, "name"));
    const char *t = json_get_str(json_get(req->args, "type"));
    const char *v = json_get_str(json_get(req->args, "value"));
    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, n);
    mcp_params_push_str(&p, t);
    mcp_params_push_str(&p, v);
    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("name_register", params) : NULL;
    free(params);
    if (!out) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "RPC name_register failed: name=%s", n ? n : "(null)");
        LOG_ERR("mcp.app", "name_register failed: name=%s", n ? n : "(null)");
    }
    res->body = out;
    return 0;
}

/* ── Messaging (ZMSG) ───────────────────────────────────────── */

static int h_zcl_msg_send_named(const struct mcp_request *req, struct mcp_response *res)
{
    const char *n = json_get_str(json_get(req->args, "name"));
    const char *m = json_get_str(json_get(req->args, "message"));
    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, n);
    mcp_params_push_str(&p, m);
    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("msg_send_named", params) : NULL;
    free(params);
    if (!out) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "RPC msg_send_named failed: name=%s", n ? n : "(null)");
        LOG_ERR("mcp.app", "msg_send_named failed: name=%s", n ? n : "(null)");
    }
    res->body = out;
    return 0;
}

static int h_zcl_msg_send(const struct mcp_request *req, struct mcp_response *res)
{
    int64_t pid = json_get_int(json_get(req->args, "peer_id"));
    const char *m = json_get_str(json_get(req->args, "message"));
    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_int(&p, pid);
    mcp_params_push_str(&p, m);
    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("msg_send", params) : NULL;
    free(params);
    if (!out) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "RPC msg_send failed: peer_id=%lld", (long long)pid);
        LOG_ERR("mcp.app", "msg_send failed: peer_id=%lld", (long long)pid);
    }
    res->body = out;
    return 0;
}

static int h_zcl_msg_inbox(const struct mcp_request *req, struct mcp_response *res)
{
    const struct json_value *uo = json_get(req->args, "unread_only");
    char *out = (uo && json_get_bool(uo))
                 ? mcp_node_rpc("msg_inbox", "[true]")
                 : mcp_node_rpc("msg_inbox", NULL);
    if (!out) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "RPC msg_inbox returned null");
        LOG_ERR("mcp.app", "msg_inbox returned null");
    }
    res->body = out;
    return 0;
}

static int h_zcl_msg_read(const struct mcp_request *req, struct mcp_response *res)
{
    const char *mid = json_get_str(json_get(req->args, "msg_id"));
    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, mid);
    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("msg_read", params) : NULL;
    free(params);
    if (!out) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "RPC msg_read failed: msg_id=%s", mid ? mid : "(null)");
        LOG_ERR("mcp.app", "msg_read failed: msg_id=%s", mid ? mid : "(null)");
    }
    res->body = out;
    return 0;
}

/* ── File market ────────────────────────────────────────────── */

DEFINE_PT(h_zcl_market_list,   "zmarket_list")
DEFINE_PT(h_zcl_market_status, "zmarket_status")

static int h_zcl_market_offer(const struct mcp_request *req, struct mcp_response *res)
{
    const char *fp = json_get_str(json_get(req->args, "filepath"));
    int64_t price  = json_get_int(json_get(req->args, "price_per_mb_zat"));
    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, fp);
    mcp_params_push_int(&p, price);
    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("zmarket_offer", params) : NULL;
    free(params);
    if (!out) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "RPC zmarket_offer failed: filepath=%s", fp ? fp : "(null)");
        LOG_ERR("mcp.app", "zmarket_offer failed: filepath=%s", fp ? fp : "(null)");
    }
    res->body = out;
    return 0;
}

static int h_zcl_market_buy(const struct mcp_request *req, struct mcp_response *res)
{
    const char *rh = json_get_str(json_get(req->args, "root_hash"));
    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, rh);
    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("zmarket_buy", params) : NULL;
    free(params);
    if (!out) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "RPC zmarket_buy failed: root_hash=%s", rh ? rh : "(null)");
        LOG_ERR("mcp.app", "zmarket_buy failed: root_hash=%s", rh ? rh : "(null)");
    }
    res->body = out;
    return 0;
}

/* ── Atomic swaps (ZSWP) ────────────────────────────────────── */

DEFINE_PT(h_zcl_swap_chains, "swap_chains")

static int h_zcl_swap_initiate(const struct mcp_request *req, struct mcp_response *res)
{
    const char *ma = json_get_str(json_get(req->args, "my_address"));
    const char *ca = json_get_str(json_get(req->args, "counter_address"));
    int64_t amount   = json_get_int(json_get(req->args, "amount"));
    int64_t locktime = json_get_int(json_get(req->args, "locktime_blocks"));
    const struct json_value *chain_v = json_get(req->args, "chain");
    const char *chain = chain_v ? json_get_str(chain_v) : NULL;
    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, ma);
    mcp_params_push_str(&p, ca);
    mcp_params_push_int(&p, amount);
    mcp_params_push_int(&p, locktime);
    if (chain) mcp_params_push_str(&p, chain);
    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("swap_initiate", params) : NULL;
    free(params);
    if (!out) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "RPC swap_initiate failed");
        LOG_ERR("mcp.app", "swap_initiate failed: amount=%lld", (long long)amount);
    }
    res->body = out;
    return 0;
}

static int h_zcl_swap_participate(const struct mcp_request *req, struct mcp_response *res)
{
    const char *ma = json_get_str(json_get(req->args, "my_address"));
    const char *ca = json_get_str(json_get(req->args, "counter_address"));
    int64_t amount   = json_get_int(json_get(req->args, "amount"));
    int64_t locktime = json_get_int(json_get(req->args, "locktime_blocks"));
    const char *sh = json_get_str(json_get(req->args, "secret_hash"));
    const struct json_value *chain_v = json_get(req->args, "chain");
    const char *chain = chain_v ? json_get_str(chain_v) : NULL;
    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, ma);
    mcp_params_push_str(&p, ca);
    mcp_params_push_int(&p, amount);
    mcp_params_push_int(&p, locktime);
    mcp_params_push_str(&p, sh);
    if (chain) mcp_params_push_str(&p, chain);
    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("swap_participate", params) : NULL;
    free(params);
    if (!out) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "RPC swap_participate failed");
        LOG_ERR("mcp.app", "swap_participate failed: amount=%lld", (long long)amount);
    }
    res->body = out;
    return 0;
}

static int h_zcl_swap_list(const struct mcp_request *req, struct mcp_response *res)
{
    const struct json_value *st = json_get(req->args, "state");
    char *out;
    if (st) {
        struct mcp_params p;
        mcp_params_init(&p);
        mcp_params_push_str(&p, json_get_str(st));
        char *params = mcp_params_to_json(&p);
        out = params ? mcp_node_rpc("swap_list", params) : NULL;
        free(params);
    } else {
        out = mcp_node_rpc("swap_list", NULL);
    }
    if (!out) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "RPC swap_list returned null");
        LOG_ERR("mcp.app", "swap_list returned null");
    }
    res->body = out;
    return 0;
}

/* ── Parameter specs ────────────────────────────────────────── */

static const struct mcp_param_spec p_name_resolve[] = {
    { "name", MCP_PARAM_STR, true, "Name to resolve",
      0, 0, 1, 63, NULL, NULL },
};
static const struct mcp_param_spec p_name_register[] = {
    { "name",  MCP_PARAM_STR, true, "Name (1-63 chars)",
      0, 0, 1, 63, NULL, NULL },
    { "type",  MCP_PARAM_STR, true, "Target type",
      0, 0, 0, 0, "onion,zaddr,taddr", NULL },
    { "value", MCP_PARAM_STR, true, "Target value",
      0, 0, 1, 256, NULL, NULL },
};
static const struct mcp_param_spec p_msg_send_named[] = {
    { "name",    MCP_PARAM_STR, true, "ZCL Name (e.g. alice)",
      0, 0, 1, 63, NULL, NULL },
    { "message", MCP_PARAM_STR, true, "Message text",
      0, 0, 1, 4000, NULL, NULL },
};
static const struct mcp_param_spec p_msg_send[] = {
    { "peer_id", MCP_PARAM_INT, true, "Connected peer ID",
      0, 1000000, 0, 0, NULL, NULL },
    { "message", MCP_PARAM_STR, true, "Message text",
      0, 0, 1, 4000, NULL, NULL },
};
static const struct mcp_param_spec p_msg_inbox[] = {
    { "unread_only", MCP_PARAM_BOOL, false, "Only unread",
      0, 0, 0, 0, NULL, "false" },
};
static const struct mcp_param_spec p_msg_read[] = {
    { "msg_id", MCP_PARAM_STR, true, "64-char hex message ID",
      0, 0, 64, 64, NULL, NULL },
};
static const struct mcp_param_spec p_market_offer[] = {
    { "filepath",         MCP_PARAM_STR, true, "Path to file to share",
      0, 0, 1, 1024, NULL, NULL },
    { "price_per_mb_zat", MCP_PARAM_INT, true, "Price per MB in zatoshis",
      0, 1000000000LL, 0, 0, NULL, NULL },
};
static const struct mcp_param_spec p_market_buy[] = {
    { "root_hash", MCP_PARAM_STR, true, "64-char hex SHA3 of offer",
      0, 0, 64, 64, NULL, NULL },
};
static const struct mcp_param_spec p_swap_initiate[] = {
    { "my_address",      MCP_PARAM_STR, true,  "Your address (refund path)",
      0, 0, 1, 128, NULL, NULL },
    { "counter_address", MCP_PARAM_STR, true,  "Counterparty address",
      0, 0, 1, 128, NULL, NULL },
    { "amount",          MCP_PARAM_INT, true,  "Amount in coins",
      1, 21000000LL, 0, 0, NULL, NULL },
    { "locktime_blocks", MCP_PARAM_INT, true,  "Lock duration in blocks",
      1, 1000000, 0, 0, NULL, NULL },
    { "chain",           MCP_PARAM_STR, false, "Chain",
      0, 0, 0, 0, "zcl,btc,ltc,doge", "\"zcl\"" },
};
static const struct mcp_param_spec p_swap_participate[] = {
    { "my_address",      MCP_PARAM_STR, true,  "Your address",
      0, 0, 1, 128, NULL, NULL },
    { "counter_address", MCP_PARAM_STR, true,  "Initiator address",
      0, 0, 1, 128, NULL, NULL },
    { "amount",          MCP_PARAM_INT, true,  "Amount",
      1, 21000000LL, 0, 0, NULL, NULL },
    { "locktime_blocks", MCP_PARAM_INT, true,  "Lock blocks (shorter than initiator)",
      1, 1000000, 0, 0, NULL, NULL },
    { "secret_hash",     MCP_PARAM_STR, true,  "64-char hex secret hash",
      0, 0, 64, 64, NULL, NULL },
    { "chain",           MCP_PARAM_STR, false, "Chain",
      0, 0, 0, 0, "zcl,btc,ltc,doge", "\"zcl\"" },
};
static const struct mcp_param_spec p_swap_list[] = {
    { "state", MCP_PARAM_STR, false, "Filter by state",
      0, 0, 0, 0, "pending,funded,redeemed,refunded", NULL },
};

static const struct mcp_tool_route k_routes[] = {
    /* Tokens */
    { "zcl_tokens", "app",
      "List all ZSLP tokens on the network.",
      NULL, 0, h_zcl_tokens },

    /* Names */
    { "zcl_name_resolve", "app",
      "Resolve a ZCL Name to its target (.onion, z-addr, or t-addr).",
      p_name_resolve, sizeof(p_name_resolve) / sizeof(p_name_resolve[0]),
      h_zcl_name_resolve },
    { "zcl_name_register", "app",
      "Build an OP_RETURN script to register a ZCL Name on-chain.",
      p_name_register, sizeof(p_name_register) / sizeof(p_name_register[0]),
      h_zcl_name_register },
    { "zcl_name_list", "app",
      "List all registered ZCL Names on the network.",
      NULL, 0, h_zcl_name_list },

    /* Messaging */
    { "zcl_msg_send_named", "app",
      "Send a message to a ZCL Name. Resolves the name first.",
      p_msg_send_named,
      sizeof(p_msg_send_named) / sizeof(p_msg_send_named[0]),
      h_zcl_msg_send_named },
    { "zcl_msg_send", "app",
      "Send a P2P message to a connected peer.",
      p_msg_send, sizeof(p_msg_send) / sizeof(p_msg_send[0]), h_zcl_msg_send },
    { "zcl_msg_inbox", "app",
      "List messages in the inbox. Newest first.",
      p_msg_inbox, sizeof(p_msg_inbox) / sizeof(p_msg_inbox[0]),
      h_zcl_msg_inbox },
    { "zcl_msg_read", "app",
      "Mark a message as read and return its content.",
      p_msg_read, sizeof(p_msg_read) / sizeof(p_msg_read[0]), h_zcl_msg_read },

    /* File market */
    { "zcl_market_list", "app",
      "List files available on the ZCL Market P2P file sharing network.",
      NULL, 0, h_zcl_market_list },
    { "zcl_market_offer", "app",
      "Announce a file for sale on the ZCL Market.",
      p_market_offer, sizeof(p_market_offer) / sizeof(p_market_offer[0]),
      h_zcl_market_offer },
    { "zcl_market_buy", "app",
      "Initiate purchase and download of a file from the ZCL Market.",
      p_market_buy, sizeof(p_market_buy) / sizeof(p_market_buy[0]),
      h_zcl_market_buy },
    { "zcl_market_status", "app",
      "ZCL Market status: cached offers, persisted offers, active downloads.",
      NULL, 0, h_zcl_market_status },

    /* Atomic swaps */
    { "zcl_swap_chains", "app",
      "List supported chains for atomic swaps: ZCL, BTC, LTC, DOGE.",
      NULL, 0, h_zcl_swap_chains },
    { "zcl_swap_initiate", "app",
      "Initiate an atomic swap. Generates secret, builds HTLC, returns P2SH.",
      p_swap_initiate, sizeof(p_swap_initiate) / sizeof(p_swap_initiate[0]),
      h_zcl_swap_initiate },
    { "zcl_swap_participate", "app",
      "Participate in an atomic swap (counter-HTLC with shorter locktime).",
      p_swap_participate,
      sizeof(p_swap_participate) / sizeof(p_swap_participate[0]),
      h_zcl_swap_participate },
    { "zcl_swap_list", "app",
      "List atomic swap contracts.",
      p_swap_list, sizeof(p_swap_list) / sizeof(p_swap_list[0]),
      h_zcl_swap_list },
};

void mcp_register_app(void)
{
    for (size_t i = 0; i < sizeof(k_routes) / sizeof(k_routes[0]); i++)
        mcp_router_register(&k_routes[i]);
}
