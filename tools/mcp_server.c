/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ZClassic23 MCP Server — Model Context Protocol for AI agents.
 * Built into zclassic23 binary.  Speaks JSON-RPC over stdio.
 *
 * Install:  claude mcp add zcl23 -- zclassic23 -mcp
 * Usage:    Claude calls tools like zcl_status, zcl_getblock, zcl_peers
 *
 * Architecture:
 *   Claude Code <--stdio--> zclassic23 -mcp <--HTTP--> zclassic23 RPC
 *
 * This file is thin: it boots the router, registers every tool via the
 * routing table in tools/mcp/router.{h,c}, and runs the stdio loop.
 * All parameter validation, schema emission and error enveloping
 * happens inside the router — handlers here see pre-validated args. */

#include "mcp/router.h"

#include "json/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ── Config ─────────────────────────────────────────────────── */

static char g_cookie[256];
static int g_port = 18232;
static char g_datadir[512];

/* ── Base64 for HTTP auth ───────────────────────────────────── */

static const char b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64_encode(const char *in, size_t len, char *out)
{
    size_t i, j = 0;
    for (i = 0; i + 2 < len; i += 3) {
        uint8_t a = (uint8_t)in[i], b = (uint8_t)in[i+1], c = (uint8_t)in[i+2];
        out[j++] = b64[a >> 2];
        out[j++] = b64[((a & 3) << 4) | (b >> 4)];
        out[j++] = b64[((b & 0xf) << 2) | (c >> 6)];
        out[j++] = b64[c & 0x3f];
    }
    if (i < len) {
        uint8_t a = (uint8_t)in[i];
        out[j++] = b64[a >> 2];
        if (i + 1 < len) {
            uint8_t b2 = (uint8_t)in[i+1];
            out[j++] = b64[((a & 3) << 4) | (b2 >> 4)];
            out[j++] = b64[(b2 & 0xf) << 2];
        } else {
            out[j++] = b64[(a & 3) << 4];
            out[j++] = '=';
        }
        out[j++] = '=';
    }
    out[j] = 0;
}

/* ── Cookie auth ───────────────────────────────────────────── */

static bool read_cookie(void)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/.cookie", g_datadir);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    size_t n = fread(g_cookie, 1, sizeof(g_cookie) - 1, f);
    fclose(f);
    g_cookie[n] = 0;
    char *nl = strchr(g_cookie, '\n');
    if (nl) *nl = 0;
    return n > 0;
}

/* ── RPC call ───────────────────────────────────────────────── */

/* Returns malloc'd JSON string or NULL.  Caller frees. */
static char *node_rpc(const char *method, const char *params_json)
{
    read_cookie();

    char body[8192];
    int blen;
    if (params_json && params_json[0])
        blen = snprintf(body, sizeof(body),
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"%s\",\"params\":%s}",
            method, params_json);
    else
        blen = snprintf(body, sizeof(body),
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"%s\",\"params\":[]}",
            method);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return strdup("{\"error\":\"socket failed\"}");

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)g_port),
    };
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return strdup("{\"error\":\"cannot connect to node\"}");
    }

    char auth_b64[512];
    base64_encode(g_cookie, strlen(g_cookie), auth_b64);

    char header[1024];
    int hlen = snprintf(header, sizeof(header),
        "POST / HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: Basic %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n", auth_b64, blen);

    send(sock, header, (size_t)hlen, 0);
    send(sock, body, (size_t)blen, 0);

    size_t cap = 65536, len = 0;
    char *buf = malloc(cap);
    if (!buf) { close(sock); return NULL; }
    for (;;) {
        if (len + 4096 > cap) { cap *= 2; buf = realloc(buf, cap); }
        ssize_t n = recv(sock, buf + len, cap - len - 1, 0);
        if (n <= 0) break;
        len += (size_t)n;
    }
    close(sock);
    buf[len] = 0;

    /* Skip HTTP headers */
    char *body_start = strstr(buf, "\r\n\r\n");
    if (body_start) {
        body_start += 4;
        size_t bslen = len - (size_t)(body_start - buf);
        memmove(buf, body_start, bslen + 1);
    }

    /* Extract "result" from {"result":...,"error":null,"id":1} */
    struct json_value v;
    if (json_read(&v, buf, strlen(buf))) {
        const struct json_value *res = json_get(&v, "result");
        const struct json_value *err = json_get(&v, "error");
        if (err && err->type != JSON_NULL) {
            char *out = malloc(4096);
            json_write(err, out, 4096);
            json_free(&v);
            free(buf);
            return out;
        }
        if (res) {
            char *out = malloc(cap);
            json_write(res, out, cap);
            json_free(&v);
            free(buf);
            return out;
        }
        json_free(&v);
    }
    return buf;
}

/* ── Handler helpers ────────────────────────────────────────── */

#define H_SET_BODY(res, s) do { (res)->body = (s); } while (0)

/* Pass-through: "no args, single RPC method". */
#define DEFINE_PASSTHROUGH(fn_name, rpc_method)                                \
    static int fn_name(const struct mcp_request *req,                          \
                       struct mcp_response *res)                               \
    {                                                                          \
        (void)req;                                                             \
        char *out = node_rpc(rpc_method, NULL);                                \
        if (!out) return -1;                                                   \
        res->body = out;                                                       \
        return 0;                                                              \
    }

static int h_zcl_status(const struct mcp_request *req, struct mcp_response *res)
{
    (void)req;
    char *h  = node_rpc("getblockcount", NULL);
    char *p  = node_rpc("getpeerinfo", NULL);
    char *s  = node_rpc("syncstate", NULL);
    char *v  = node_rpc("validationstatus", NULL);
    char *hc = node_rpc("healthcheck", NULL);

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

DEFINE_PASSTHROUGH(h_zcl_getblockcount,     "getblockcount")
DEFINE_PASSTHROUGH(h_zcl_getblockchaininfo, "getblockchaininfo")
DEFINE_PASSTHROUGH(h_zcl_peers,             "getpeerinfo")
DEFINE_PASSTHROUGH(h_zcl_networkinfo,       "getnetworkinfo")
DEFINE_PASSTHROUGH(h_zcl_onion_status,      "healthcheck")
DEFINE_PASSTHROUGH(h_zcl_syncstate,         "syncstate")
DEFINE_PASSTHROUGH(h_zcl_validationstatus,  "validationstatus")
DEFINE_PASSTHROUGH(h_zcl_dataintegrity,     "getdataintegrity")
DEFINE_PASSTHROUGH(h_zcl_mmb,               "getmmrroot")
DEFINE_PASSTHROUGH(h_zcl_utxocommitment,    "getutxocommitment")
DEFINE_PASSTHROUGH(h_zcl_gametypes,         "gametypes")
DEFINE_PASSTHROUGH(h_zcl_peerlatency,       "getpeerlatency")
DEFINE_PASSTHROUGH(h_zcl_balance,           "z_gettotalbalance")
DEFINE_PASSTHROUGH(h_zcl_getnewaddress,     "getnewaddress")
DEFINE_PASSTHROUGH(h_zcl_z_getnewaddress,   "z_getnewaddress")
DEFINE_PASSTHROUGH(h_zcl_hodlwave,          "gethodlwave")
DEFINE_PASSTHROUGH(h_zcl_filemanifest,      "getfilemanifeststatus")
DEFINE_PASSTHROUGH(h_zcl_tokens,            "zslp_listtokens")
DEFINE_PASSTHROUGH(h_zcl_health,            "healthcheck")
DEFINE_PASSTHROUGH(h_zcl_swap_chains,       "swap_chains")
DEFINE_PASSTHROUGH(h_zcl_name_list,         "name_list")
DEFINE_PASSTHROUGH(h_zcl_market_list,       "zmarket_list")
DEFINE_PASSTHROUGH(h_zcl_market_status,     "zmarket_status")

static int h_zcl_getblock(const struct mcp_request *req, struct mcp_response *res)
{
    const char *id_str = json_get_str(json_get(req->args, "block_id"));
    const struct json_value *verb = json_get(req->args, "verbosity");
    int verbosity = verb ? (int)json_get_int(verb) : 1;

    bool is_num = id_str && id_str[0];
    for (const char *c = id_str; is_num && *c; c++)
        if (*c < '0' || *c > '9') is_num = false;

    char params[256];
    if (is_num) {
        snprintf(params, sizeof(params), "[%s]", id_str);
        char *hash = node_rpc("getblockhash", params);
        if (!hash) return -1;
        char clean[128];
        size_t ci = 0;
        for (size_t i = 0; hash[i] && ci < 127; i++)
            if (hash[i] != '"' && hash[i] != '\n') clean[ci++] = hash[i];
        clean[ci] = 0;
        free(hash);
        snprintf(params, sizeof(params), "[\"%s\",%d]", clean, verbosity);
    } else {
        snprintf(params, sizeof(params), "[\"%s\",%d]", id_str, verbosity);
    }
    char *out = node_rpc("getblock", params);
    if (!out) return -1;
    res->body = out;
    return 0;
}

static int h_zcl_addnode(const struct mcp_request *req, struct mcp_response *res)
{
    const char *addr = json_get_str(json_get(req->args, "addr"));
    const struct json_value *act = json_get(req->args, "action");
    char params[256];
    snprintf(params, sizeof(params), "[\"%s\",\"%s\"]",
             addr, act ? json_get_str(act) : "onetry");
    char *out = node_rpc("addnode", params);
    if (!out) return -1;
    res->body = out;
    return 0;
}

static int h_zcl_pingpeer(const struct mcp_request *req, struct mcp_response *res)
{
    int64_t peer_id = json_get_int(json_get(req->args, "peer_id"));
    char params[64];
    snprintf(params, sizeof(params), "[%lld]", (long long)peer_id);
    char *out = node_rpc("pingpeer", params);
    if (!out) return -1;
    res->body = out;
    return 0;
}

static int h_zcl_send(const struct mcp_request *req, struct mcp_response *res)
{
    const char *from = json_get_str(json_get(req->args, "from"));
    const char *to   = json_get_str(json_get(req->args, "to"));
    const struct json_value *amt = json_get(req->args, "amount");
    double amount = (amt && amt->type == JSON_REAL) ? json_get_real(amt)
                                                    : (double)json_get_int(amt);
    char params[512];
    snprintf(params, sizeof(params),
             "[\"%s\",[{\"address\":\"%s\",\"amount\":%.8f}]]",
             from, to, amount);
    char *out = node_rpc("z_sendmany", params);
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
    char *out = node_rpc("eventlog", params);
    if (!out) return -1;
    res->body = out;
    return 0;
}

static int h_zcl_swap_initiate(const struct mcp_request *req, struct mcp_response *res)
{
    const char *ma = json_get_str(json_get(req->args, "my_address"));
    const char *ca = json_get_str(json_get(req->args, "counter_address"));
    int64_t amount  = json_get_int(json_get(req->args, "amount"));
    int64_t locktime = json_get_int(json_get(req->args, "locktime_blocks"));
    const struct json_value *chain_v = json_get(req->args, "chain");
    const char *chain = chain_v ? json_get_str(chain_v) : NULL;
    char params[1024];
    if (chain)
        snprintf(params, sizeof(params), "[\"%s\",\"%s\",%lld,%lld,\"%s\"]",
                 ma, ca, (long long)amount, (long long)locktime, chain);
    else
        snprintf(params, sizeof(params), "[\"%s\",\"%s\",%lld,%lld]",
                 ma, ca, (long long)amount, (long long)locktime);
    char *out = node_rpc("swap_initiate", params);
    if (!out) return -1;
    res->body = out;
    return 0;
}

static int h_zcl_swap_participate(const struct mcp_request *req, struct mcp_response *res)
{
    const char *ma = json_get_str(json_get(req->args, "my_address"));
    const char *ca = json_get_str(json_get(req->args, "counter_address"));
    int64_t amount  = json_get_int(json_get(req->args, "amount"));
    int64_t locktime = json_get_int(json_get(req->args, "locktime_blocks"));
    const char *sh = json_get_str(json_get(req->args, "secret_hash"));
    const struct json_value *chain_v = json_get(req->args, "chain");
    const char *chain = chain_v ? json_get_str(chain_v) : NULL;
    char params[1024];
    if (chain)
        snprintf(params, sizeof(params),
                 "[\"%s\",\"%s\",%lld,%lld,\"%s\",\"%s\"]",
                 ma, ca, (long long)amount, (long long)locktime, sh, chain);
    else
        snprintf(params, sizeof(params),
                 "[\"%s\",\"%s\",%lld,%lld,\"%s\"]",
                 ma, ca, (long long)amount, (long long)locktime, sh);
    char *out = node_rpc("swap_participate", params);
    if (!out) return -1;
    res->body = out;
    return 0;
}

static int h_zcl_swap_list(const struct mcp_request *req, struct mcp_response *res)
{
    const struct json_value *st = json_get(req->args, "state");
    char *out;
    if (st) {
        char params[64];
        snprintf(params, sizeof(params), "[\"%s\"]", json_get_str(st));
        out = node_rpc("swap_list", params);
    } else {
        out = node_rpc("swap_list", NULL);
    }
    if (!out) return -1;
    res->body = out;
    return 0;
}

static int h_zcl_msg_send_named(const struct mcp_request *req, struct mcp_response *res)
{
    const char *n = json_get_str(json_get(req->args, "name"));
    const char *m = json_get_str(json_get(req->args, "message"));
    char params[4200];
    snprintf(params, sizeof(params), "[\"%s\", \"%s\"]", n, m);
    char *out = node_rpc("msg_send_named", params);
    if (!out) return -1;
    res->body = out;
    return 0;
}

static int h_zcl_msg_send(const struct mcp_request *req, struct mcp_response *res)
{
    int64_t pid = json_get_int(json_get(req->args, "peer_id"));
    const char *m = json_get_str(json_get(req->args, "message"));
    char params[4200];
    snprintf(params, sizeof(params), "[%lld, \"%s\"]", (long long)pid, m);
    char *out = node_rpc("msg_send", params);
    if (!out) return -1;
    res->body = out;
    return 0;
}

static int h_zcl_msg_inbox(const struct mcp_request *req, struct mcp_response *res)
{
    const struct json_value *uo = json_get(req->args, "unread_only");
    char *out = (uo && json_get_bool(uo))
                 ? node_rpc("msg_inbox", "[true]")
                 : node_rpc("msg_inbox", NULL);
    if (!out) return -1;
    res->body = out;
    return 0;
}

static int h_zcl_msg_read(const struct mcp_request *req, struct mcp_response *res)
{
    const char *mid = json_get_str(json_get(req->args, "msg_id"));
    char params[128];
    snprintf(params, sizeof(params), "[\"%s\"]", mid);
    char *out = node_rpc("msg_read", params);
    if (!out) return -1;
    res->body = out;
    return 0;
}

static int h_zcl_name_resolve(const struct mcp_request *req, struct mcp_response *res)
{
    const char *n = json_get_str(json_get(req->args, "name"));
    char params[256];
    snprintf(params, sizeof(params), "[\"%s\"]", n);
    char *out = node_rpc("name_resolve", params);
    if (!out) return -1;
    res->body = out;
    return 0;
}

static int h_zcl_name_register(const struct mcp_request *req, struct mcp_response *res)
{
    const char *n = json_get_str(json_get(req->args, "name"));
    const char *t = json_get_str(json_get(req->args, "type"));
    const char *v = json_get_str(json_get(req->args, "value"));
    char params[1024];
    snprintf(params, sizeof(params), "[\"%s\", \"%s\", \"%s\"]", n, t, v);
    char *out = node_rpc("name_register", params);
    if (!out) return -1;
    res->body = out;
    return 0;
}

static int h_zcl_market_offer(const struct mcp_request *req, struct mcp_response *res)
{
    const char *fp = json_get_str(json_get(req->args, "filepath"));
    int64_t price  = json_get_int(json_get(req->args, "price_per_mb_zat"));
    char params[1024];
    snprintf(params, sizeof(params), "[\"%s\", %lld]", fp, (long long)price);
    char *out = node_rpc("zmarket_offer", params);
    if (!out) return -1;
    res->body = out;
    return 0;
}

static int h_zcl_market_buy(const struct mcp_request *req, struct mcp_response *res)
{
    const char *rh = json_get_str(json_get(req->args, "root_hash"));
    char params[128];
    snprintf(params, sizeof(params), "[\"%s\"]", rh);
    char *out = node_rpc("zmarket_buy", params);
    if (!out) return -1;
    res->body = out;
    return 0;
}

static int h_zcl_rpc(const struct mcp_request *req, struct mcp_response *res)
{
    const char *m = json_get_str(json_get(req->args, "method"));
    const struct json_value *p = json_get(req->args, "params");
    char *out = node_rpc(m, p ? json_get_str(p) : NULL);
    if (!out) return -1;
    res->body = out;
    return 0;
}

/* ── Route table ─────────────────────────────────────────────── */

#define NO_PARAMS NULL, 0

static const struct mcp_param_spec p_getblock[] = {
    { "block_id", MCP_PARAM_STR, true, "Height or hash",
      0, 0, 1, 128, NULL, NULL },
    { "verbosity", MCP_PARAM_INT, false, "0=hex, 1=JSON, 2=JSON+tx",
      0, 2, 0, 0, NULL, "1" },
};
static const struct mcp_param_spec p_addnode[] = {
    { "addr", MCP_PARAM_STR, true, "IP:port",
      0, 0, 1, 128, NULL, NULL },
    { "action", MCP_PARAM_STR, false, "add | remove | onetry",
      0, 0, 0, 0, "add,remove,onetry", "\"onetry\"" },
};
static const struct mcp_param_spec p_pingpeer[] = {
    { "peer_id", MCP_PARAM_INT, true, "Peer ID from zcl_peers",
      0, 1000000, 0, 0, NULL, NULL },
};
static const struct mcp_param_spec p_send[] = {
    { "from",   MCP_PARAM_STR,  true, "Source address",
      0, 0, 1, 128, NULL, NULL },
    { "to",     MCP_PARAM_STR,  true, "Destination address",
      0, 0, 1, 128, NULL, NULL },
    { "amount", MCP_PARAM_REAL, true, "Amount in ZCL",
      0, 0, 0, 0, NULL, NULL },
};
static const struct mcp_param_spec p_events[] = {
    { "count", MCP_PARAM_INT, false, "Number of events",
      1, 1000, 0, 0, NULL, "20" },
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
static const struct mcp_param_spec p_market_offer[] = {
    { "filepath",          MCP_PARAM_STR, true, "Path to file to share",
      0, 0, 1, 1024, NULL, NULL },
    { "price_per_mb_zat",  MCP_PARAM_INT, true, "Price per MB in zatoshis",
      0, 1000000000LL, 0, 0, NULL, NULL },
};
static const struct mcp_param_spec p_market_buy[] = {
    { "root_hash", MCP_PARAM_STR, true, "64-char hex SHA3 of offer",
      0, 0, 64, 64, NULL, NULL },
};
static const struct mcp_param_spec p_rpc[] = {
    { "method", MCP_PARAM_STR, true,  "RPC method name",
      0, 0, 1, 128, NULL, NULL },
    { "params", MCP_PARAM_STR, false, "JSON params array",
      0, 0, 0, 0, NULL, "\"[]\"" },
};

#define ROUTE(nm, dom, desc, ps, handler) \
    { nm, dom, desc, ps, sizeof(ps)/sizeof(ps[0]), handler }
#define ROUTE0(nm, dom, desc, handler) \
    { nm, dom, desc, NULL, 0, handler }

static const struct mcp_tool_route k_routes[] = {
    /* ── ops ───────────────────────────────────────── */
    ROUTE0("zcl_status", "ops",
           "Node status: block height, peers, sync state, onion address, "
           "bg-validation progress, health checks. The single command to "
           "check if everything is working.",
           h_zcl_status),
    ROUTE0("zcl_health", "ops",
           "Health check: pass/fail, chain height, peers, sync, onion.",
           h_zcl_health),
    ROUTE0("zcl_filemanifest", "ops",
           "File service status: chunks, SHA3 hashes, total size.",
           h_zcl_filemanifest),
    ROUTE("zcl_events", "ops",
          "Recent event log: sync events, peer connections, blocks.",
          p_events, h_zcl_events),
    ROUTE("zcl_rpc", "ops",
          "Call any RPC method directly. 85+ commands available.",
          p_rpc, h_zcl_rpc),

    /* ── chain ─────────────────────────────────────── */
    ROUTE0("zcl_getblockcount", "chain",
           "Current block height.",
           h_zcl_getblockcount),
    ROUTE("zcl_getblock", "chain",
          "Get block by height or hash.",
          p_getblock, h_zcl_getblock),
    ROUTE0("zcl_getblockchaininfo", "chain",
           "Chain state: height, best block, difficulty, chain work, value pools.",
           h_zcl_getblockchaininfo),
    ROUTE0("zcl_syncstate", "chain",
           "Sync state machine: phase, progress, header/block/UTXO status.",
           h_zcl_syncstate),
    ROUTE0("zcl_validationstatus", "chain",
           "Background validation: verified height, sigs, proofs, blocks/sec.",
           h_zcl_validationstatus),
    ROUTE0("zcl_dataintegrity", "chain",
           "SHA3-256 hashes over all consensus tables.",
           h_zcl_dataintegrity),
    ROUTE0("zcl_mmb", "chain",
           "Merkle Mountain Belt root. FlyClient chain verification.",
           h_zcl_mmb),
    ROUTE0("zcl_utxocommitment", "chain",
           "SHA3-256 over entire UTXO set in canonical order.",
           h_zcl_utxocommitment),
    ROUTE0("zcl_hodlwave", "chain",
           "UTXO age distribution: 10 buckets from 24h to 5y+.",
           h_zcl_hodlwave),

    /* ── net ───────────────────────────────────────── */
    ROUTE0("zcl_peers", "net",
           "Connected peers with addresses, latency, services, heights.",
           h_zcl_peers),
    ROUTE0("zcl_networkinfo", "net",
           "Network info: version, connections, relay fee.",
           h_zcl_networkinfo),
    ROUTE("zcl_addnode", "net",
          "Add/remove peer. Actions: add, remove, onetry.",
          p_addnode, h_zcl_addnode),
    ROUTE0("zcl_onion_status", "net",
           "Tor onion service: .onion address, bootstrap state.",
           h_zcl_onion_status),
    ROUTE0("zcl_gametypes", "net",
           "P2P game types: Ping (latency measurement), TicTacToe.",
           h_zcl_gametypes),
    ROUTE("zcl_pingpeer", "net",
          "Measure round-trip latency to a connected peer.",
          p_pingpeer, h_zcl_pingpeer),
    ROUTE0("zcl_peerlatency", "net",
           "Latency for all peers: ping_ms, min_ping_ms, avg_latency_ms.",
           h_zcl_peerlatency),

    /* ── wallet ────────────────────────────────────── */
    ROUTE0("zcl_balance", "wallet",
           "Total wallet balance: transparent + shielded.",
           h_zcl_balance),
    ROUTE0("zcl_getnewaddress", "wallet",
           "Generate new transparent (t-addr) receiving address.",
           h_zcl_getnewaddress),
    ROUTE0("zcl_z_getnewaddress", "wallet",
           "Generate new shielded Sapling (z-addr) receiving address.",
           h_zcl_z_getnewaddress),
    ROUTE("zcl_send", "wallet",
          "Send ZCL (transparent or shielded).",
          p_send, h_zcl_send),

    /* ── app (names, messages, tokens, market, swaps) ── */
    ROUTE0("zcl_tokens", "app",
           "List all ZSLP tokens on the network.",
           h_zcl_tokens),
    ROUTE("zcl_name_resolve", "app",
          "Resolve a ZCL Name to its target (.onion, z-addr, or t-addr).",
          p_name_resolve, h_zcl_name_resolve),
    ROUTE("zcl_name_register", "app",
          "Build an OP_RETURN script to register a ZCL Name on-chain.",
          p_name_register, h_zcl_name_register),
    ROUTE0("zcl_name_list", "app",
           "List all registered ZCL Names on the network.",
           h_zcl_name_list),
    ROUTE("zcl_msg_send_named", "app",
          "Send a message to a ZCL Name. Resolves the name first.",
          p_msg_send_named, h_zcl_msg_send_named),
    ROUTE("zcl_msg_send", "app",
          "Send a P2P message to a connected peer.",
          p_msg_send, h_zcl_msg_send),
    ROUTE("zcl_msg_inbox", "app",
          "List messages in the inbox. Newest first.",
          p_msg_inbox, h_zcl_msg_inbox),
    ROUTE("zcl_msg_read", "app",
          "Mark a message as read and return its content.",
          p_msg_read, h_zcl_msg_read),
    ROUTE0("zcl_market_list", "app",
           "List files available on the ZCL Market P2P file sharing network.",
           h_zcl_market_list),
    ROUTE("zcl_market_offer", "app",
          "Announce a file for sale on the ZCL Market.",
          p_market_offer, h_zcl_market_offer),
    ROUTE("zcl_market_buy", "app",
          "Initiate purchase and download of a file from the ZCL Market.",
          p_market_buy, h_zcl_market_buy),
    ROUTE0("zcl_market_status", "app",
           "ZCL Market status: cached offers, persisted offers, active downloads.",
           h_zcl_market_status),
    ROUTE0("zcl_swap_chains", "app",
           "List supported chains for atomic swaps: ZCL, BTC, LTC, DOGE.",
           h_zcl_swap_chains),
    ROUTE("zcl_swap_initiate", "app",
          "Initiate an atomic swap. Generates secret, builds HTLC, returns P2SH.",
          p_swap_initiate, h_zcl_swap_initiate),
    ROUTE("zcl_swap_participate", "app",
          "Participate in an atomic swap (counter-HTLC with shorter locktime).",
          p_swap_participate, h_zcl_swap_participate),
    ROUTE("zcl_swap_list", "app",
          "List atomic swap contracts.",
          p_swap_list, h_zcl_swap_list),
};

#define NUM_ROUTES (sizeof(k_routes) / sizeof(k_routes[0]))

static void register_all_routes(void)
{
    mcp_router_reset();
    for (size_t i = 0; i < NUM_ROUTES; i++)
        mcp_router_register(&k_routes[i]);
}

/* ── MCP protocol ────────────────────────────────────────────── */

static void mcp_send(const char *json)
{
    fprintf(stdout, "%s\n", json);
    fflush(stdout);
}

static void handle_initialize(const struct json_value *req)
{
    const struct json_value *id = json_get(req, "id");
    char resp[512];
    snprintf(resp, sizeof(resp),
        "{\"jsonrpc\":\"2.0\",\"id\":%lld,\"result\":{"
        "\"protocolVersion\":\"2024-11-05\","
        "\"capabilities\":{\"tools\":{}},"
        "\"serverInfo\":{\"name\":\"zcl23\",\"version\":\"1.0.0\"}"
        "}}",
        id ? (long long)json_get_int(id) : 0LL);
    mcp_send(resp);
}

static void handle_tools_list(const struct json_value *req)
{
    const struct json_value *id = json_get(req, "id");
    size_t cap = 65536;
    char *buf = malloc(cap);
    if (!buf) return;

    int pos = snprintf(buf, cap,
        "{\"jsonrpc\":\"2.0\",\"id\":%lld,\"result\":{\"tools\":",
        id ? (long long)json_get_int(id) : 0LL);

    size_t written = mcp_router_tools_list_json(buf + pos, cap - (size_t)pos);
    pos += (int)written;
    pos += snprintf(buf + pos, cap - (size_t)pos, "}}");
    mcp_send(buf);
    free(buf);
}

static void handle_tools_call(const struct json_value *req)
{
    const struct json_value *id = json_get(req, "id");
    const struct json_value *params = json_get(req, "params");
    const struct json_value *name_v = params ? json_get(params, "name") : NULL;
    const struct json_value *args = params ? json_get(params, "arguments") : NULL;

    if (!name_v) {
        char resp[256];
        snprintf(resp, sizeof(resp),
            "{\"jsonrpc\":\"2.0\",\"id\":%lld,"
            "\"error\":{\"code\":-32602,\"message\":\"missing tool name\"}}",
            id ? (long long)json_get_int(id) : 0LL);
        mcp_send(resp);
        return;
    }

    char *result = mcp_router_dispatch(json_get_str(name_v), args);
    if (!result) result = strdup("null");

    /* Embed result as JSON text content. */
    size_t rlen = strlen(result);
    size_t cap = rlen * 2 + 512;
    char *resp = malloc(cap);
    char *escaped = malloc(rlen * 2 + 1);
    size_t ei = 0;
    for (size_t i = 0; i < rlen; i++) {
        char c = result[i];
        if (c == '"') { escaped[ei++] = '\\'; escaped[ei++] = '"'; }
        else if (c == '\\') { escaped[ei++] = '\\'; escaped[ei++] = '\\'; }
        else if (c == '\n') { escaped[ei++] = '\\'; escaped[ei++] = 'n'; }
        else if (c == '\r') { escaped[ei++] = '\\'; escaped[ei++] = 'r'; }
        else if (c == '\t') { escaped[ei++] = '\\'; escaped[ei++] = 't'; }
        else escaped[ei++] = c;
    }
    escaped[ei] = 0;

    snprintf(resp, cap,
        "{\"jsonrpc\":\"2.0\",\"id\":%lld,\"result\":{"
        "\"content\":[{\"type\":\"text\",\"text\":\"%s\"}]}}",
        id ? (long long)json_get_int(id) : 0LL, escaped);

    mcp_send(resp);
    free(escaped);
    free(resp);
    free(result);
}

/* ── Main loop ──────────────────────────────────────────────── */

int mcp_server_main(const char *datadir, int rpc_port)
{
    snprintf(g_datadir, sizeof(g_datadir), "%s", datadir);
    g_port = rpc_port;

    register_all_routes();

    char line[65536];
    while (fgets(line, sizeof(line), stdin)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = 0;
        if (len == 0) continue;

        struct json_value req;
        if (!json_read(&req, line, len)) {
            mcp_send("{\"jsonrpc\":\"2.0\",\"id\":null,"
                     "\"error\":{\"code\":-32700,\"message\":\"Parse error\"}}");
            continue;
        }

        const struct json_value *method = json_get(&req, "method");
        if (!method || method->type != JSON_STR) {
            json_free(&req);
            continue;
        }

        const char *m = json_get_str(method);

        if (strcmp(m, "initialize") == 0)
            handle_initialize(&req);
        else if (strcmp(m, "notifications/initialized") == 0)
            { /* no response */ }
        else if (strcmp(m, "tools/list") == 0)
            handle_tools_list(&req);
        else if (strcmp(m, "tools/call") == 0)
            handle_tools_call(&req);
        else {
            const struct json_value *id = json_get(&req, "id");
            char resp[256];
            snprintf(resp, sizeof(resp),
                "{\"jsonrpc\":\"2.0\",\"id\":%lld,"
                "\"error\":{\"code\":-32601,\"message\":\"Method not found\"}}",
                id ? (long long)json_get_int(id) : 0LL);
            mcp_send(resp);
        }

        json_free(&req);
    }

    return 0;
}
