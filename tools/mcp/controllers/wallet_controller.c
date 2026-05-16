/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * MCP wallet controller: balance, addresses, sending, wallet diagnostics. */

#include "../controllers.h"
#include "../router.h"
#include "../rpc_client.h"
#include "../rpc_params.h"

#include "json/json.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Simple passthroughs (no params) ─────────────────────────── */

DEFINE_PT(h_zcl_balance,               "z_gettotalbalance", "mcp.wallet")
DEFINE_PT(h_zcl_getnewaddress,         "getnewaddress",     "mcp.wallet")
DEFINE_PT(h_zcl_z_getnewaddress,       "z_getnewaddress",   "mcp.wallet")
DEFINE_PT(h_zcl_getwalletinfo,         "getwalletinfo",     "mcp.wallet")
DEFINE_PT(h_zcl_z_listaddresses,       "z_listaddresses",   "mcp.wallet")
DEFINE_PT(h_zcl_walletaudit,           "walletaudit",       "mcp.wallet")

/* ── Parameterized handlers ──────────────────────────────────── */

static int h_zcl_send(const struct mcp_request *req, struct mcp_response *res)
{
    const char *from = json_get_str(json_get(req->args, "from"));
    const char *to   = json_get_str(json_get(req->args, "to"));
    const struct json_value *amt = json_get(req->args, "amount");
    double amount = (amt && amt->type == JSON_REAL) ? json_get_real(amt)
                                                    : (double)json_get_int(amt);

    /* Build [from, [{address: to, amount}]] via the JSON encoder — a
     * quote in `from` or `to` would otherwise rewrite the params array. */
    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, from);

    struct json_value recip, recip_arr;
    json_init(&recip);     json_set_object(&recip);
    json_push_kv_str (&recip, "address", to ? to : "");
    json_push_kv_real(&recip, "amount",  amount);
    json_init(&recip_arr); json_set_array(&recip_arr);
    json_push_back(&recip_arr, &recip);
    mcp_params_push_value(&p, &recip_arr);
    json_free(&recip);
    json_free(&recip_arr);

    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("z_sendmany", params) : NULL;
    free(params);
    if (!out) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "RPC z_sendmany failed: from=%s to=%s", from ? from : "(null)", to ? to : "(null)");
        LOG_ERR("mcp.wallet", "z_sendmany failed: from=%s to=%s amount=%.8f",
                from ? from : "(null)", to ? to : "(null)", amount);
    }
    res->body = out;
    return 0;
}

static int h_zcl_sendtoaddress(const struct mcp_request *req,
                                struct mcp_response *res)
{
    const char *addr = json_get_str(json_get(req->args, "address"));
    const struct json_value *amt = json_get(req->args, "amount");
    double amount = (amt && amt->type == JSON_REAL) ? json_get_real(amt)
                                                    : (double)json_get_int(amt);

    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str (&p, addr);
    mcp_params_push_real(&p, amount);
    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("sendtoaddress", params) : NULL;
    free(params);
    if (!out) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "RPC sendtoaddress failed: addr=%s", addr ? addr : "(null)");
        LOG_ERR("mcp.wallet", "sendtoaddress failed: addr=%s amount=%.8f",
                addr ? addr : "(null)", amount);
    }
    res->body = out;
    return 0;
}

static int h_zcl_listunspent(const struct mcp_request *req,
                              struct mcp_response *res)
{
    const struct json_value *mc = json_get(req->args, "minconf");
    const struct json_value *mx = json_get(req->args, "maxconf");
    char params[128];
    snprintf(params, sizeof(params), "[%lld,%lld]",
             mc ? (long long)json_get_int(mc) : 1LL,
             mx ? (long long)json_get_int(mx) : 9999999LL);
    char *out = mcp_node_rpc("listunspent", params);
    if (!out) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "RPC listunspent returned null");
        LOG_ERR("mcp.wallet", "listunspent returned null");
    }
    res->body = out;
    return 0;
}

static int h_zcl_listtransactions(const struct mcp_request *req,
                                    struct mcp_response *res)
{
    const struct json_value *cnt = json_get(req->args, "count");
    const struct json_value *sk  = json_get(req->args, "skip");
    char params[128];
    snprintf(params, sizeof(params), "[\"\",%lld,%lld]",
             cnt ? (long long)json_get_int(cnt) : 10LL,
             sk  ? (long long)json_get_int(sk)  : 0LL);
    char *out = mcp_node_rpc("listtransactions", params);
    if (!out) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "RPC listtransactions returned null");
        LOG_ERR("mcp.wallet", "listtransactions returned null");
    }
    res->body = out;
    return 0;
}

static int h_zcl_gettransaction(const struct mcp_request *req,
                                  struct mcp_response *res)
{
    const char *txid = json_get_str(json_get(req->args, "txid"));
    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, txid);
    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("gettransaction", params) : NULL;
    free(params);
    if (!out) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "RPC gettransaction failed: txid=%s", txid ? txid : "(null)");
        LOG_ERR("mcp.wallet", "gettransaction failed: txid=%s", txid ? txid : "(null)");
    }
    res->body = out;
    return 0;
}

static int h_zcl_listaddresses(const struct mcp_request *req,
                                struct mcp_response *res)
{
    (void)req;
    /* The node RPC `listwalletkeys` returns {transparent_keys:[{address,...}],
     * sapling_keys:[...]}.  Call it without private keys and project just
     * the addresses so the caller gets a clean list. */
    char *raw = mcp_node_rpc("listwalletkeys", "[false]");
    if (!raw) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "RPC listwalletkeys returned null");
        LOG_ERR("mcp.wallet", "listwalletkeys returned null");
    }

    struct json_value root;
    if (!json_read(&root, raw, strlen(raw))) { res->body = raw; return 0; }
    free(raw);

    size_t cap = 65536;
    char *out = zcl_malloc(cap, "listaddresses_body");
    if (!out) {
        json_free(&root);
        res->error = MCP_ERR_INTERNAL;
        snprintf(res->error_message, sizeof(res->error_message),
                 "malloc failed for listaddresses response");
        LOG_ERR("mcp.wallet", "malloc failed for listaddresses (%zu bytes)", cap);
    }
    size_t pos = 0;
    pos += (size_t)snprintf(out + pos, cap - pos, "{\"t_addresses\":[");

    const struct json_value *tk = json_get(&root, "transparent_keys");
    bool first = true;
    if (tk && tk->type == JSON_ARR) {
        for (size_t i = 0; i < tk->num_children; i++) {
            const struct json_value *k = &tk->children[i];
            const struct json_value *av = json_get(k, "address");
            const char *addr = av ? json_get_str(av) : NULL;
            if (!addr || !addr[0]) continue;
            if (pos + strlen(addr) + 8 >= cap) break;
            if (!first) out[pos++] = ',';
            first = false;
            out[pos++] = '"';
            for (const char *c = addr; *c && pos + 2 < cap; c++) out[pos++] = *c;
            out[pos++] = '"';
        }
    }
    pos += (size_t)snprintf(out + pos, cap - pos, "],\"z_addresses\":[");

    const struct json_value *sk = json_get(&root, "sapling_keys");
    first = true;
    if (sk && sk->type == JSON_ARR) {
        for (size_t i = 0; i < sk->num_children; i++) {
            const struct json_value *k = &sk->children[i];
            const struct json_value *av = json_get(k, "address");
            const char *addr = av ? json_get_str(av) : NULL;
            if (!addr || !addr[0]) continue;
            if (pos + strlen(addr) + 8 >= cap) break;
            if (!first) out[pos++] = ',';
            first = false;
            out[pos++] = '"';
            for (const char *c = addr; *c && pos + 2 < cap; c++) out[pos++] = *c;
            out[pos++] = '"';
        }
    }
    if (pos + 2 < cap) { out[pos++] = ']'; out[pos++] = '}'; out[pos] = 0; }

    json_free(&root);
    res->body = out;
    return 0;
}

static int h_zcl_dumpprivkey(const struct mcp_request *req,
                               struct mcp_response *res)
{
    const char *addr = json_get_str(json_get(req->args, "address"));
    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, addr);
    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("dumpprivkey", params) : NULL;
    free(params);
    if (!out) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "RPC dumpprivkey failed: address=%s", addr ? addr : "(null)");
        LOG_ERR("mcp.wallet", "dumpprivkey failed: address=%s", addr ? addr : "(null)");
    }
    res->body = out;
    return 0;
}

static int h_zcl_importprivkey(const struct mcp_request *req,
                                 struct mcp_response *res)
{
    const char *wif   = json_get_str(json_get(req->args, "privkey"));
    const char *label = json_get_str(json_get(req->args, "label"));
    const struct json_value *rs = json_get(req->args, "rescan");
    bool rescan = rs ? json_get_bool(rs) : false;
    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str (&p, wif);
    mcp_params_push_str (&p, label);
    mcp_params_push_bool(&p, rescan);
    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("importprivkey", params) : NULL;
    free(params);
    if (!out) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "RPC importprivkey failed");
        LOG_ERR("mcp.wallet", "importprivkey failed: rescan=%s", rescan ? "true" : "false");
    }
    res->body = out;
    return 0;
}

static int h_zcl_importaddress(const struct mcp_request *req,
                                 struct mcp_response *res)
{
    const char *addr = json_get_str(json_get(req->args, "address"));
    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, addr);
    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("importaddress", params) : NULL;
    free(params);
    if (!out) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "RPC importaddress failed: address=%s", addr ? addr : "(null)");
        LOG_ERR("mcp.wallet", "importaddress failed: address=%s", addr ? addr : "(null)");
    }
    res->body = out;
    return 0;
}

static int h_zcl_z_listunspent(const struct mcp_request *req,
                                 struct mcp_response *res)
{
    const struct json_value *mc = json_get(req->args, "minconf");
    char params[64];
    snprintf(params, sizeof(params), "[%lld]",
             mc ? (long long)json_get_int(mc) : 1LL);
    char *out = mcp_node_rpc("z_listunspent", params);
    if (!out) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "RPC z_listunspent returned null");
        LOG_ERR("mcp.wallet", "z_listunspent returned null");
    }
    res->body = out;
    return 0;
}

static int h_zcl_z_getbalance(const struct mcp_request *req,
                                struct mcp_response *res)
{
    const char *addr = json_get_str(json_get(req->args, "address"));
    const struct json_value *mc = json_get(req->args, "minconf");
    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, addr);
    mcp_params_push_int(&p, mc ? json_get_int(mc) : 1LL);
    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("z_getbalance", params) : NULL;
    free(params);
    if (!out) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "RPC z_getbalance failed: address=%s", addr ? addr : "(null)");
        LOG_ERR("mcp.wallet", "z_getbalance failed: address=%s", addr ? addr : "(null)");
    }
    res->body = out;
    return 0;
}

static int h_zcl_rescanblockchain(const struct mcp_request *req,
                                    struct mcp_response *res)
{
    const struct json_value *s = json_get(req->args, "start_height");
    const struct json_value *e = json_get(req->args, "stop_height");
    char params[64];
    if (s && e)
        snprintf(params, sizeof(params), "[%lld,%lld]",
                 (long long)json_get_int(s), (long long)json_get_int(e));
    else if (s)
        snprintf(params, sizeof(params), "[%lld]",
                 (long long)json_get_int(s));
    else
        snprintf(params, sizeof(params), "[]");
    char *out = mcp_node_rpc("rescanblockchain", params);
    if (!out) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "RPC rescanblockchain returned null");
        LOG_ERR("mcp.wallet", "rescanblockchain returned null");
    }
    res->body = out;
    return 0;
}

static int h_zcl_listwalletkeys(const struct mcp_request *req,
                                  struct mcp_response *res)
{
    const struct json_value *ip = json_get(req->args, "include_privkeys");
    bool inc = ip ? json_get_bool(ip) : false;
    char params[32];
    snprintf(params, sizeof(params), "[%s]", inc ? "true" : "false");
    char *out = mcp_node_rpc("listwalletkeys", params);
    if (!out) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "RPC listwalletkeys returned null");
        LOG_ERR("mcp.wallet", "listwalletkeys returned null");
    }
    res->body = out;
    return 0;
}

static int h_zcl_replaywalletfromchain(const struct mcp_request *req,
                                         struct mcp_response *res)
{
    /* RPC requires "confirm" literal. Wrap the user's boolean for safety. */
    const struct json_value *cv = json_get(req->args, "confirm");
    bool confirm = cv ? json_get_bool(cv) : false;
    if (!confirm) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "replaywalletfromchain requires confirm=true "
                 "(destructive: wipes derived wallet state)");
        snprintf(res->error_param, sizeof(res->error_param), "confirm");
        LOG_ERR("mcp.wallet", "replaywalletfromchain called without confirm=true");
    }
    char *out = mcp_node_rpc("replaywalletfromchain", "[\"confirm\"]");
    if (!out) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "RPC replaywalletfromchain returned null");
        LOG_ERR("mcp.wallet", "replaywalletfromchain returned null");
    }
    res->body = out;
    return 0;
}

/* ── Parameter specs ─────────────────────────────────────────── */

static const struct mcp_param_spec p_send[] = {
    { "from",   MCP_PARAM_STR,  true, "Source address",
      0, 0, 1, 128, NULL, NULL },
    { "to",     MCP_PARAM_STR,  true, "Destination address",
      0, 0, 1, 128, NULL, NULL },
    { "amount", MCP_PARAM_REAL, true, "Amount in ZCL",
      0, 0, 0, 0, NULL, NULL },
};

static const struct mcp_param_spec p_sendtoaddr[] = {
    { "address", MCP_PARAM_STR,  true, "Destination t-address",
      0, 0, 1, 128, NULL, NULL },
    { "amount",  MCP_PARAM_REAL, true, "Amount in ZCL",
      0, 0, 0, 0, NULL, NULL },
};

static const struct mcp_param_spec p_listunspent[] = {
    { "minconf", MCP_PARAM_INT, false, "Minimum confirmations",
      0, 9999999, 0, 0, NULL, "1" },
    { "maxconf", MCP_PARAM_INT, false, "Maximum confirmations",
      0, 9999999, 0, 0, NULL, "9999999" },
};

static const struct mcp_param_spec p_listtx[] = {
    { "count", MCP_PARAM_INT, false, "Number of transactions to return",
      1, 10000, 0, 0, NULL, "10" },
    { "skip",  MCP_PARAM_INT, false, "Number of most recent to skip",
      0, 10000000, 0, 0, NULL, "0" },
};

static const struct mcp_param_spec p_gettx[] = {
    { "txid", MCP_PARAM_STR, true, "Transaction id (hex)",
      0, 0, 1, 128, NULL, NULL },
};

static const struct mcp_param_spec p_addr[] = {
    { "address", MCP_PARAM_STR, true, "Address",
      0, 0, 1, 128, NULL, NULL },
};

static const struct mcp_param_spec p_importaddr[] = {
    { "address", MCP_PARAM_STR, true, "Transparent address to watch",
      0, 0, 1, 128, NULL, NULL },
};

static const struct mcp_param_spec p_importkey[] = {
    { "privkey", MCP_PARAM_STR,  true,  "WIF-encoded private key",
      0, 0, 1, 128, NULL, NULL },
    { "label",   MCP_PARAM_STR,  false, "Optional label",
      0, 0, 0, 128, NULL, "\"\"" },
    { "rescan",  MCP_PARAM_BOOL, false, "Rescan chain after import",
      0, 0, 0, 0, NULL, "false" },
};

static const struct mcp_param_spec p_zunspent[] = {
    { "minconf", MCP_PARAM_INT, false, "Minimum confirmations",
      0, 9999999, 0, 0, NULL, "1" },
};

static const struct mcp_param_spec p_zbalance[] = {
    { "address", MCP_PARAM_STR, true,  "Shielded z-address or t-address",
      0, 0, 1, 128, NULL, NULL },
    { "minconf", MCP_PARAM_INT, false, "Minimum confirmations",
      0, 9999999, 0, 0, NULL, "1" },
};

static const struct mcp_param_spec p_rescan[] = {
    { "start_height", MCP_PARAM_INT, false, "Start block height",
      0, 100000000, 0, 0, NULL, "0" },
    { "stop_height",  MCP_PARAM_INT, false, "Stop block height",
      0, 100000000, 0, 0, NULL, NULL },
};

static const struct mcp_param_spec p_listkeys[] = {
    { "include_privkeys", MCP_PARAM_BOOL, false,
      "Include WIF private keys in the response",
      0, 0, 0, 0, NULL, "false" },
};

static const struct mcp_param_spec p_confirm[] = {
    { "confirm", MCP_PARAM_BOOL, true,
      "Must be true — destructive: wipes derived wallet state",
      0, 0, 0, 0, NULL, NULL },
};

/* ── Route table ─────────────────────────────────────────────── */

static const struct mcp_tool_route k_routes[] = {
    { "zcl_balance", "wallet",
      "Total wallet balance: transparent + shielded.",
      NULL, 0, h_zcl_balance },
    { "zcl_getnewaddress", "wallet",
      "Generate new transparent (t-addr) receiving address.",
      NULL, 0, h_zcl_getnewaddress },
    { "zcl_z_getnewaddress", "wallet",
      "Generate new shielded Sapling (z-addr) receiving address.",
      NULL, 0, h_zcl_z_getnewaddress },
    { "zcl_send", "wallet",
      "Send ZCL (transparent or shielded).",
      p_send, sizeof(p_send) / sizeof(p_send[0]), h_zcl_send },

    { "zcl_getwalletinfo", "wallet",
      "One-shot wallet health snapshot: balance, tx count, keys, status.",
      NULL, 0, h_zcl_getwalletinfo },
    { "zcl_listunspent", "wallet",
      "List transparent UTXOs available to spend.",
      p_listunspent, sizeof(p_listunspent) / sizeof(p_listunspent[0]),
      h_zcl_listunspent },
    { "zcl_listtransactions", "wallet",
      "Recent wallet transaction history.",
      p_listtx, sizeof(p_listtx) / sizeof(p_listtx[0]),
      h_zcl_listtransactions },
    { "zcl_gettransaction", "wallet",
      "Fetch a single wallet transaction by id.",
      p_gettx, sizeof(p_gettx) / sizeof(p_gettx[0]), h_zcl_gettransaction },
    { "zcl_sendtoaddress", "wallet",
      "Simple send to a single transparent address.",
      p_sendtoaddr, sizeof(p_sendtoaddr) / sizeof(p_sendtoaddr[0]),
      h_zcl_sendtoaddress },
    { "zcl_listaddresses", "wallet",
      "All transparent (t-addr) addresses in the wallet.",
      NULL, 0, h_zcl_listaddresses },
    { "zcl_dumpprivkey", "wallet",
      "Export the WIF private key for a transparent address.",
      p_addr, sizeof(p_addr) / sizeof(p_addr[0]), h_zcl_dumpprivkey },
    { "zcl_importprivkey", "wallet",
      "Import a WIF private key into the wallet.",
      p_importkey, sizeof(p_importkey) / sizeof(p_importkey[0]),
      h_zcl_importprivkey },
    { "zcl_importaddress", "wallet",
      "Watch a transparent address without private key. Tracks balance and "
      "transactions but cannot spend.",
      p_importaddr, sizeof(p_importaddr) / sizeof(p_importaddr[0]),
      h_zcl_importaddress },
    { "zcl_z_listaddresses", "wallet",
      "All shielded Sapling (z-addr) addresses in the wallet.",
      NULL, 0, h_zcl_z_listaddresses },
    { "zcl_z_listunspent", "wallet",
      "List shielded notes available to spend.",
      p_zunspent, sizeof(p_zunspent) / sizeof(p_zunspent[0]),
      h_zcl_z_listunspent },
    { "zcl_z_getbalance", "wallet",
      "Balance for a single t-address or z-address.",
      p_zbalance, sizeof(p_zbalance) / sizeof(p_zbalance[0]),
      h_zcl_z_getbalance },
    { "zcl_rescanblockchain", "wallet",
      "Manually trigger a wallet rescan over a height range.",
      p_rescan, sizeof(p_rescan) / sizeof(p_rescan[0]),
      h_zcl_rescanblockchain },
    { "zcl_walletaudit", "wallet",
      "Reconcile the wallet against the on-chain UTXO set.",
      NULL, 0, h_zcl_walletaudit },
    { "zcl_listwalletkeys", "wallet",
      "List all keys (metadata, and optionally WIFs).",
      p_listkeys, sizeof(p_listkeys) / sizeof(p_listkeys[0]),
      h_zcl_listwalletkeys },
    { "zcl_replaywalletfromchain", "wallet",
      "Rebuild the derived wallet state by replaying the chain. "
      "Destructive — requires confirm=true.",
      p_confirm, sizeof(p_confirm) / sizeof(p_confirm[0]),
      h_zcl_replaywalletfromchain },
};

void mcp_register_wallet(void)
{
    for (size_t i = 0; i < sizeof(k_routes) / sizeof(k_routes[0]); i++)
        mcp_router_register(&k_routes[i]);
}
