/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ZClassic23 MCP Server — Model Context Protocol for AI agents.
 * Built into zclassic23 binary. Speaks JSON-RPC over stdio.
 *
 * Install:  claude mcp add zcl23 -- zclassic23 -mcp
 * Usage:    Claude calls tools like zcl_status, zcl_getblock, zcl_peers
 *
 * Architecture:
 *   Claude Code <--stdio--> zclassic23 -mcp <--HTTP--> zclassic23 RPC
 */

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

/* ── Cookie auth ────────────────────────────────────���───────── */

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

/* Returns malloc'd JSON string or NULL. Caller frees. */
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

/* ── Tool definitions ───────────────────────────────────────── */

struct mcp_tool {
    const char *name;
    const char *description;
    const char *schema;  /* JSON inputSchema string */
};

static const struct mcp_tool tools[] = {
    {"zcl_status",
     "Node status: block height, peers, sync state, onion address, "
     "bg-validation progress, health checks. The single command to "
     "check if everything is working.",
     "{\"type\":\"object\",\"properties\":{}}"},

    {"zcl_getblockcount",
     "Current block height.",
     "{\"type\":\"object\",\"properties\":{}}"},

    {"zcl_getblock",
     "Get block by height or hash.",
     "{\"type\":\"object\",\"properties\":{"
     "\"block_id\":{\"type\":\"string\",\"description\":\"Height or hash\"},"
     "\"verbosity\":{\"type\":\"integer\",\"description\":\"0=hex 1=JSON 2=JSON+tx\",\"default\":1}"
     "},\"required\":[\"block_id\"]}"},

    {"zcl_getblockchaininfo",
     "Chain state: height, best block, difficulty, chain work, value pools.",
     "{\"type\":\"object\",\"properties\":{}}"},

    {"zcl_peers",
     "Connected peers with addresses, latency, services, heights.",
     "{\"type\":\"object\",\"properties\":{}}"},

    {"zcl_networkinfo",
     "Network info: version, connections, relay fee.",
     "{\"type\":\"object\",\"properties\":{}}"},

    {"zcl_addnode",
     "Add/remove peer. Actions: add, remove, onetry.",
     "{\"type\":\"object\",\"properties\":{"
     "\"addr\":{\"type\":\"string\",\"description\":\"IP:port\"},"
     "\"action\":{\"type\":\"string\",\"enum\":[\"add\",\"remove\",\"onetry\"]}"
     "},\"required\":[\"addr\"]}"},

    {"zcl_onion_status",
     "Tor onion service: .onion address, bootstrap state, "
     "directory peers, dynhost status.",
     "{\"type\":\"object\",\"properties\":{}}"},

    {"zcl_syncstate",
     "Sync state machine: phase, progress, header/block/UTXO status.",
     "{\"type\":\"object\",\"properties\":{}}"},

    {"zcl_validationstatus",
     "Background validation: verified height, sigs, proofs, blocks/sec.",
     "{\"type\":\"object\",\"properties\":{}}"},

    {"zcl_dataintegrity",
     "SHA3-256 hashes over all consensus tables. Database integrity check.",
     "{\"type\":\"object\",\"properties\":{}}"},

    {"zcl_mmb",
     "Merkle Mountain Belt root. FlyClient chain verification "
     "(50 samples, ≥150-bit security).",
     "{\"type\":\"object\",\"properties\":{}}"},

    {"zcl_utxocommitment",
     "SHA3-256 over entire UTXO set in canonical order.",
     "{\"type\":\"object\",\"properties\":{}}"},

    {"zcl_gametypes",
     "P2P game types: Ping (latency measurement), TicTacToe.",
     "{\"type\":\"object\",\"properties\":{}}"},

    {"zcl_pingpeer",
     "Measure round-trip latency to a connected peer.",
     "{\"type\":\"object\",\"properties\":{"
     "\"peer_id\":{\"type\":\"integer\",\"description\":\"Peer ID from zcl_peers\"}"
     "},\"required\":[\"peer_id\"]}"},

    {"zcl_peerlatency",
     "Latency for all peers: ping_ms, min_ping_ms, avg_latency_ms.",
     "{\"type\":\"object\",\"properties\":{}}"},

    {"zcl_balance",
     "Total wallet balance: transparent + shielded.",
     "{\"type\":\"object\",\"properties\":{}}"},

    {"zcl_getnewaddress",
     "Generate new transparent (t-addr) receiving address.",
     "{\"type\":\"object\",\"properties\":{}}"},

    {"zcl_z_getnewaddress",
     "Generate new shielded Sapling (z-addr) receiving address.",
     "{\"type\":\"object\",\"properties\":{}}"},

    {"zcl_send",
     "Send ZCL (transparent or shielded).",
     "{\"type\":\"object\",\"properties\":{"
     "\"from\":{\"type\":\"string\",\"description\":\"Source address\"},"
     "\"to\":{\"type\":\"string\",\"description\":\"Destination address\"},"
     "\"amount\":{\"type\":\"number\",\"description\":\"Amount in ZCL\"}"
     "},\"required\":[\"from\",\"to\",\"amount\"]}"},

    {"zcl_hodlwave",
     "UTXO age distribution: 10 buckets from 24h to 5y+.",
     "{\"type\":\"object\",\"properties\":{}}"},

    {"zcl_filemanifest",
     "File service status: chunks, SHA3 hashes, total size.",
     "{\"type\":\"object\",\"properties\":{}}"},

    {"zcl_tokens",
     "List all ZSLP tokens on the network.",
     "{\"type\":\"object\",\"properties\":{}}"},

    {"zcl_events",
     "Recent event log: sync events, peer connections, blocks.",
     "{\"type\":\"object\",\"properties\":{"
     "\"count\":{\"type\":\"integer\",\"description\":\"Number of events\",\"default\":20}"
     "}}"},

    {"zcl_health",
     "Health check: pass/fail, chain height, peers, sync, onion.",
     "{\"type\":\"object\",\"properties\":{}}"},

    {"zcl_swap_chains",
     "List supported chains for atomic swaps: ZCL, BTC, LTC, DOGE.",
     "{\"type\":\"object\",\"properties\":{}}"},

    {"zcl_swap_initiate",
     "Initiate an atomic swap. Generates secret, builds HTLC, returns P2SH address to fund.",
     "{\"type\":\"object\",\"properties\":{"
     "\"my_address\":{\"type\":\"string\",\"description\":\"Your address (refund path)\"},"
     "\"counter_address\":{\"type\":\"string\",\"description\":\"Counterparty address (claim path)\"},"
     "\"amount\":{\"type\":\"number\",\"description\":\"Amount in coins\"},"
     "\"locktime_blocks\":{\"type\":\"integer\",\"description\":\"Lock duration in blocks\"},"
     "\"chain\":{\"type\":\"string\",\"description\":\"Chain: zcl, btc, ltc, doge\",\"default\":\"zcl\"}"
     "},\"required\":[\"my_address\",\"counter_address\",\"amount\",\"locktime_blocks\"]}"},

    {"zcl_swap_participate",
     "Participate in an atomic swap (counter-HTLC with shorter locktime).",
     "{\"type\":\"object\",\"properties\":{"
     "\"my_address\":{\"type\":\"string\",\"description\":\"Your address\"},"
     "\"counter_address\":{\"type\":\"string\",\"description\":\"Initiator address\"},"
     "\"amount\":{\"type\":\"number\",\"description\":\"Amount\"},"
     "\"locktime_blocks\":{\"type\":\"integer\",\"description\":\"Lock blocks (shorter than initiator)\"},"
     "\"secret_hash\":{\"type\":\"string\",\"description\":\"64-char hex secret hash from initiator\"},"
     "\"chain\":{\"type\":\"string\",\"description\":\"Chain: zcl, btc, ltc, doge\",\"default\":\"zcl\"}"
     "},\"required\":[\"my_address\",\"counter_address\",\"amount\",\"locktime_blocks\",\"secret_hash\"]}"},

    {"zcl_swap_list",
     "List atomic swap contracts. Filter by state: pending, funded, redeemed, refunded.",
     "{\"type\":\"object\",\"properties\":{"
     "\"state\":{\"type\":\"string\",\"description\":\"Filter: pending, funded, redeemed, refunded\"}"
     "}}"},

    {"zcl_msg_send_named",
     "Send a message to a ZCL Name. Resolves the name, then delivers the message.",
     "{\"type\":\"object\",\"properties\":{"
     "\"name\":{\"type\":\"string\",\"description\":\"ZCL Name (e.g. alice)\"},"
     "\"message\":{\"type\":\"string\",\"description\":\"Message text\"}"
     "},\"required\":[\"name\",\"message\"]}"},

    {"zcl_msg_send",
     "Send a P2P message to a connected peer.",
     "{\"type\":\"object\",\"properties\":{"
     "\"peer_id\":{\"type\":\"integer\",\"description\":\"Connected peer ID\"},"
     "\"message\":{\"type\":\"string\",\"description\":\"Message text\"}"
     "},\"required\":[\"peer_id\",\"message\"]}"},

    {"zcl_msg_inbox",
     "List messages in the inbox. Returns recent messages, newest first.",
     "{\"type\":\"object\",\"properties\":{"
     "\"unread_only\":{\"type\":\"boolean\",\"description\":\"Only unread\",\"default\":false}"
     "}}"},

    {"zcl_msg_read",
     "Mark a message as read and return its content.",
     "{\"type\":\"object\",\"properties\":{"
     "\"msg_id\":{\"type\":\"string\",\"description\":\"64-char hex message ID\"}"
     "},\"required\":[\"msg_id\"]}"},

    {"zcl_name_resolve",
     "Resolve a ZCL Name to its target (.onion address, z-addr, or t-addr).",
     "{\"type\":\"object\",\"properties\":{"
     "\"name\":{\"type\":\"string\",\"description\":\"Name to resolve (e.g. alice)\"}"
     "},\"required\":[\"name\"]}"},

    {"zcl_name_register",
     "Build an OP_RETURN script to register a ZCL Name on-chain.",
     "{\"type\":\"object\",\"properties\":{"
     "\"name\":{\"type\":\"string\",\"description\":\"Name (1-63 chars, lowercase+hyphens)\"},"
     "\"type\":{\"type\":\"string\",\"description\":\"Target type: onion, zaddr, taddr\"},"
     "\"value\":{\"type\":\"string\",\"description\":\"Target value (.onion, z-addr, t-addr)\"}"
     "},\"required\":[\"name\",\"type\",\"value\"]}"},

    {"zcl_name_list",
     "List all registered ZCL Names on the network.",
     "{\"type\":\"object\",\"properties\":{}}"},

    {"zcl_market_list",
     "List files available on the ZCL Market P2P file sharing network.",
     "{\"type\":\"object\",\"properties\":{}}"},

    {"zcl_market_offer",
     "Announce a file for sale on the ZCL Market. Requires filepath and price.",
     "{\"type\":\"object\",\"properties\":{"
     "\"filepath\":{\"type\":\"string\",\"description\":\"Path to file to share\"},"
     "\"price_per_mb_zat\":{\"type\":\"integer\",\"description\":\"Price per MB in zatoshis\"}"
     "},\"required\":[\"filepath\",\"price_per_mb_zat\"]}"},

    {"zcl_market_buy",
     "Initiate purchase and download of a file from the ZCL Market.",
     "{\"type\":\"object\",\"properties\":{"
     "\"root_hash\":{\"type\":\"string\",\"description\":\"SHA3 hash of file offer (64-char hex)\"}"
     "},\"required\":[\"root_hash\"]}"},

    {"zcl_market_status",
     "ZCL Market status: cached offers, persisted offers, active downloads.",
     "{\"type\":\"object\",\"properties\":{}}"},

    {"zcl_rpc",
     "Call any RPC method directly. 85+ commands available including "
     "getblock, getrawtransaction, z_sendmany, getmininginfo, etc.",
     "{\"type\":\"object\",\"properties\":{"
     "\"method\":{\"type\":\"string\",\"description\":\"RPC method name\"},"
     "\"params\":{\"type\":\"string\",\"description\":\"JSON params array\",\"default\":\"[]\"}"
     "},\"required\":[\"method\"]}"},
};

#define NUM_TOOLS (sizeof(tools) / sizeof(tools[0]))

/* ── Tool dispatch ──────────────────────────────────────────── */

static char *dispatch_tool(const char *name, const struct json_value *args)
{
    if (strcmp(name, "zcl_status") == 0) {
        char *h = node_rpc("getblockcount", NULL);
        char *p = node_rpc("getpeerinfo", NULL);
        char *s = node_rpc("syncstate", NULL);
        char *v = node_rpc("validationstatus", NULL);
        char *hc = node_rpc("healthcheck", NULL);

        /* Count peers */
        int pc = 0;
        if (p) { for (char *c = p; *c; c++) if (*c == '{') pc++; }

        char *out = malloc(32768);
        snprintf(out, 32768,
            "{\"height\":%s,\"peers\":%d,\"sync\":%s,"
            "\"validation\":%s,\"health\":%s}",
            h ? h : "null", pc, s ? s : "null",
            v ? v : "null", hc ? hc : "null");
        free(h); free(p); free(s); free(v); free(hc);
        return out;
    }

    if (strcmp(name, "zcl_getblockcount") == 0)
        return node_rpc("getblockcount", NULL);

    if (strcmp(name, "zcl_getblock") == 0) {
        const struct json_value *bid = args ? json_get(args, "block_id") : NULL;
        const struct json_value *verb = args ? json_get(args, "verbosity") : NULL;
        if (!bid) return strdup("{\"error\":\"block_id required\"}");
        const char *id_str = json_get_str(bid);
        int verbosity = verb ? (int)json_get_int(verb) : 1;

        /* If numeric, get hash first */
        bool is_num = true;
        for (const char *c = id_str; *c; c++)
            if (*c < '0' || *c > '9') { is_num = false; break; }

        char params[256];
        if (is_num) {
            snprintf(params, sizeof(params), "[%s]", id_str);
            char *hash = node_rpc("getblockhash", params);
            if (!hash) return strdup("{\"error\":\"getblockhash failed\"}");
            /* hash may have quotes or not */
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
        return node_rpc("getblock", params);
    }

    if (strcmp(name, "zcl_getblockchaininfo") == 0)
        return node_rpc("getblockchaininfo", NULL);

    if (strcmp(name, "zcl_peers") == 0)
        return node_rpc("getpeerinfo", NULL);

    if (strcmp(name, "zcl_networkinfo") == 0)
        return node_rpc("getnetworkinfo", NULL);

    if (strcmp(name, "zcl_addnode") == 0) {
        const struct json_value *a = args ? json_get(args, "addr") : NULL;
        const struct json_value *act = args ? json_get(args, "action") : NULL;
        if (!a) return strdup("{\"error\":\"addr required\"}");
        char params[256];
        snprintf(params, sizeof(params), "[\"%s\",\"%s\"]",
                 json_get_str(a), act ? json_get_str(act) : "onetry");
        return node_rpc("addnode", params);
    }

    if (strcmp(name, "zcl_onion_status") == 0)
        return node_rpc("healthcheck", NULL);

    if (strcmp(name, "zcl_syncstate") == 0)
        return node_rpc("syncstate", NULL);

    if (strcmp(name, "zcl_validationstatus") == 0)
        return node_rpc("validationstatus", NULL);

    if (strcmp(name, "zcl_dataintegrity") == 0)
        return node_rpc("getdataintegrity", NULL);

    if (strcmp(name, "zcl_mmb") == 0)
        return node_rpc("getmmrroot", NULL);

    if (strcmp(name, "zcl_utxocommitment") == 0)
        return node_rpc("getutxocommitment", NULL);

    if (strcmp(name, "zcl_gametypes") == 0)
        return node_rpc("gametypes", NULL);

    if (strcmp(name, "zcl_pingpeer") == 0) {
        const struct json_value *pid = args ? json_get(args, "peer_id") : NULL;
        if (!pid) return strdup("{\"error\":\"peer_id required\"}");
        char params[64];
        snprintf(params, sizeof(params), "[%lld]", (long long)json_get_int(pid));
        return node_rpc("pingpeer", params);
    }

    if (strcmp(name, "zcl_peerlatency") == 0)
        return node_rpc("getpeerlatency", NULL);

    if (strcmp(name, "zcl_balance") == 0)
        return node_rpc("z_gettotalbalance", NULL);

    if (strcmp(name, "zcl_getnewaddress") == 0)
        return node_rpc("getnewaddress", NULL);

    if (strcmp(name, "zcl_z_getnewaddress") == 0)
        return node_rpc("z_getnewaddress", NULL);

    if (strcmp(name, "zcl_send") == 0) {
        const struct json_value *from = args ? json_get(args, "from") : NULL;
        const struct json_value *to = args ? json_get(args, "to") : NULL;
        const struct json_value *amt = args ? json_get(args, "amount") : NULL;
        if (!from || !to || !amt)
            return strdup("{\"error\":\"from, to, amount required\"}");
        char params[512];
        snprintf(params, sizeof(params),
            "[\"%s\",[{\"address\":\"%s\",\"amount\":%.8f}]]",
            json_get_str(from), json_get_str(to),
            amt->type == JSON_REAL ? json_get_real(amt) : (double)json_get_int(amt));
        return node_rpc("z_sendmany", params);
    }

    if (strcmp(name, "zcl_hodlwave") == 0)
        return node_rpc("gethodlwave", NULL);

    if (strcmp(name, "zcl_filemanifest") == 0)
        return node_rpc("getfilemanifeststatus", NULL);

    if (strcmp(name, "zcl_tokens") == 0)
        return node_rpc("zslp_listtokens", NULL);

    if (strcmp(name, "zcl_events") == 0) {
        const struct json_value *cnt = args ? json_get(args, "count") : NULL;
        char params[64];
        snprintf(params, sizeof(params), "[%lld]",
                 cnt ? (long long)json_get_int(cnt) : 20LL);
        return node_rpc("eventlog", params);
    }

    if (strcmp(name, "zcl_health") == 0)
        return node_rpc("healthcheck", NULL);

    if (strcmp(name, "zcl_swap_chains") == 0)
        return node_rpc("swap_chains", NULL);

    if (strcmp(name, "zcl_swap_initiate") == 0) {
        const struct json_value *ma = args ? json_get(args, "my_address") : NULL;
        const struct json_value *ca = args ? json_get(args, "counter_address") : NULL;
        const struct json_value *am = args ? json_get(args, "amount") : NULL;
        const struct json_value *lt = args ? json_get(args, "locktime_blocks") : NULL;
        const struct json_value *ch = args ? json_get(args, "chain") : NULL;
        if (!ma || !ca || !am || !lt)
            return strdup("{\"error\":\"my_address, counter_address, amount, locktime_blocks required\"}");
        char params[1024];
        if (ch)
            snprintf(params, sizeof(params), "[\"%s\",\"%s\",%lld,%lld,\"%s\"]",
                     json_get_str(ma), json_get_str(ca),
                     (long long)json_get_int(am), (long long)json_get_int(lt),
                     json_get_str(ch));
        else
            snprintf(params, sizeof(params), "[\"%s\",\"%s\",%lld,%lld]",
                     json_get_str(ma), json_get_str(ca),
                     (long long)json_get_int(am), (long long)json_get_int(lt));
        return node_rpc("swap_initiate", params);
    }

    if (strcmp(name, "zcl_swap_participate") == 0) {
        const struct json_value *ma = args ? json_get(args, "my_address") : NULL;
        const struct json_value *ca = args ? json_get(args, "counter_address") : NULL;
        const struct json_value *am = args ? json_get(args, "amount") : NULL;
        const struct json_value *lt = args ? json_get(args, "locktime_blocks") : NULL;
        const struct json_value *sh = args ? json_get(args, "secret_hash") : NULL;
        const struct json_value *ch = args ? json_get(args, "chain") : NULL;
        if (!ma || !ca || !am || !lt || !sh)
            return strdup("{\"error\":\"all fields required\"}");
        char params[1024];
        if (ch)
            snprintf(params, sizeof(params), "[\"%s\",\"%s\",%lld,%lld,\"%s\",\"%s\"]",
                     json_get_str(ma), json_get_str(ca),
                     (long long)json_get_int(am), (long long)json_get_int(lt),
                     json_get_str(sh), json_get_str(ch));
        else
            snprintf(params, sizeof(params), "[\"%s\",\"%s\",%lld,%lld,\"%s\"]",
                     json_get_str(ma), json_get_str(ca),
                     (long long)json_get_int(am), (long long)json_get_int(lt),
                     json_get_str(sh));
        return node_rpc("swap_participate", params);
    }

    if (strcmp(name, "zcl_swap_list") == 0) {
        const struct json_value *st = args ? json_get(args, "state") : NULL;
        if (st) {
            char params[64];
            snprintf(params, sizeof(params), "[\"%s\"]", json_get_str(st));
            return node_rpc("swap_list", params);
        }
        return node_rpc("swap_list", NULL);
    }

    if (strcmp(name, "zcl_msg_send_named") == 0) {
        const struct json_value *n = args ? json_get(args, "name") : NULL;
        const struct json_value *m = args ? json_get(args, "message") : NULL;
        if (!n || !m)
            return strdup("{\"error\":\"name and message required\"}");
        char params[4200];
        snprintf(params, sizeof(params), "[\"%s\", \"%s\"]",
                 json_get_str(n), json_get_str(m));
        return node_rpc("msg_send_named", params);
    }

    if (strcmp(name, "zcl_msg_send") == 0) {
        const struct json_value *pid = args ? json_get(args, "peer_id") : NULL;
        const struct json_value *msg = args ? json_get(args, "message") : NULL;
        if (!pid || !msg)
            return strdup("{\"error\":\"peer_id and message required\"}");
        char params[4200];
        snprintf(params, sizeof(params), "[%lld, \"%s\"]",
                 (long long)json_get_int(pid), json_get_str(msg));
        return node_rpc("msg_send", params);
    }

    if (strcmp(name, "zcl_msg_inbox") == 0) {
        const struct json_value *uo = args ? json_get(args, "unread_only") : NULL;
        if (uo && json_get_int(uo))
            return node_rpc("msg_inbox", "[true]");
        return node_rpc("msg_inbox", NULL);
    }

    if (strcmp(name, "zcl_msg_read") == 0) {
        const struct json_value *mid = args ? json_get(args, "msg_id") : NULL;
        if (!mid) return strdup("{\"error\":\"msg_id required\"}");
        char params[128];
        snprintf(params, sizeof(params), "[\"%s\"]", json_get_str(mid));
        return node_rpc("msg_read", params);
    }

    if (strcmp(name, "zcl_name_resolve") == 0) {
        const struct json_value *n = args ? json_get(args, "name") : NULL;
        if (!n) return strdup("{\"error\":\"name required\"}");
        char params[256];
        snprintf(params, sizeof(params), "[\"%s\"]", json_get_str(n));
        return node_rpc("name_resolve", params);
    }

    if (strcmp(name, "zcl_name_register") == 0) {
        const struct json_value *n = args ? json_get(args, "name") : NULL;
        const struct json_value *t = args ? json_get(args, "type") : NULL;
        const struct json_value *v = args ? json_get(args, "value") : NULL;
        if (!n || !t || !v)
            return strdup("{\"error\":\"name, type, and value required\"}");
        char params[1024];
        snprintf(params, sizeof(params), "[\"%s\", \"%s\", \"%s\"]",
                 json_get_str(n), json_get_str(t), json_get_str(v));
        return node_rpc("name_register", params);
    }

    if (strcmp(name, "zcl_name_list") == 0)
        return node_rpc("name_list", NULL);

    if (strcmp(name, "zcl_market_list") == 0)
        return node_rpc("zmarket_list", NULL);

    if (strcmp(name, "zcl_market_offer") == 0) {
        const struct json_value *fp = args ? json_get(args, "filepath") : NULL;
        const struct json_value *pr = args ? json_get(args, "price_per_mb_zat") : NULL;
        if (!fp || !pr)
            return strdup("{\"error\":\"filepath and price_per_mb_zat required\"}");
        char params[1024];
        snprintf(params, sizeof(params), "[\"%s\", %lld]",
                 json_get_str(fp), (long long)json_get_int(pr));
        return node_rpc("zmarket_offer", params);
    }

    if (strcmp(name, "zcl_market_buy") == 0) {
        const struct json_value *rh = args ? json_get(args, "root_hash") : NULL;
        if (!rh) return strdup("{\"error\":\"root_hash required\"}");
        char params[128];
        snprintf(params, sizeof(params), "[\"%s\"]", json_get_str(rh));
        return node_rpc("zmarket_buy", params);
    }

    if (strcmp(name, "zcl_market_status") == 0)
        return node_rpc("zmarket_status", NULL);

    if (strcmp(name, "zcl_rpc") == 0) {
        const struct json_value *m = args ? json_get(args, "method") : NULL;
        const struct json_value *p = args ? json_get(args, "params") : NULL;
        if (!m) return strdup("{\"error\":\"method required\"}");
        return node_rpc(json_get_str(m),
                        p ? json_get_str(p) : NULL);
    }

    char err[128];
    snprintf(err, sizeof(err), "{\"error\":\"unknown tool: %s\"}", name);
    return strdup(err);
}

/* ── MCP protocol ──────────────���────────────────────────────── */

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

    /* Build tools array JSON */
    size_t cap = 32768;
    char *buf = malloc(cap);
    int pos = snprintf(buf, cap,
        "{\"jsonrpc\":\"2.0\",\"id\":%lld,\"result\":{\"tools\":[",
        id ? (long long)json_get_int(id) : 0LL);

    for (size_t i = 0; i < NUM_TOOLS; i++) {
        if (i > 0) buf[pos++] = ',';
        pos += snprintf(buf + pos, cap - (size_t)pos,
            "{\"name\":\"%s\",\"description\":\"%s\",\"inputSchema\":%s}",
            tools[i].name, tools[i].description, tools[i].schema);
    }

    pos += snprintf(buf + pos, cap - (size_t)pos, "]}}");
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

    char *result = dispatch_tool(json_get_str(name_v), args);
    if (!result) result = strdup("null");

    /* Escape result for embedding in JSON string.
     * Since result is already JSON, we embed it directly as text content. */
    size_t rlen = strlen(result);
    size_t cap = rlen * 2 + 512;
    char *resp = malloc(cap);

    /* Escape the result string for JSON text field */
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

/* ── Main loop ─���───────────────────────────���────────────────── */

int mcp_server_main(const char *datadir, int rpc_port)
{
    snprintf(g_datadir, sizeof(g_datadir), "%s", datadir);
    g_port = rpc_port;

    char line[65536];
    while (fgets(line, sizeof(line), stdin)) {
        /* Strip trailing whitespace */
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
