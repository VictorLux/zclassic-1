/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * REST API controller — fast JSON API for the block explorer.
 * Serves /api routes. All RPC calls happen in a background thread;
 * HTTPS handler threads only serve from cache (rpc_call crashes
 * when called from HTTPS handler threads). */

#include "controllers/api_controller.h"
#include "controllers/explorer_internal.h"
#include "controllers/explorer_factoids.h"
#include "views/format_helpers.h"
#include "event/event.h"
#include "net/download.h"
#include "validation/contextual_check_tx.h"
#include "keys/key_io.h"
#include "models/database.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <inttypes.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <sqlite3.h>

/* ── State ────────────────────────────────────────────────── */

static struct main_state *g_ms = NULL;
static struct tx_mempool *g_mp = NULL;
static struct coins_view_cache *g_coins_tip = NULL;
static struct node_db *g_ndb = NULL;
static const char *g_datadir = NULL;

static char g_rpc_user[128] = "zcluser";
static char g_rpc_pass[128] = "zclpass";
static int  g_rpc_port = 8023;

/* ── Background cache ─────────────────────────────────────── */

#define API_BLOCKS_CACHE_SIZE 131072   /* 128KB */
#define API_STATS_CACHE_SIZE  16384    /* 16KB  */
#define API_SUPPLY_CACHE_SIZE 4096     /* 4KB   */
#define API_HODL_CACHE_SIZE   8192     /* 8KB   */

static char   g_api_blocks_cache[API_BLOCKS_CACHE_SIZE];
static size_t g_api_blocks_cache_len = 0;

static char   g_api_stats_cache[API_STATS_CACHE_SIZE];
static size_t g_api_stats_cache_len = 0;

static char   g_api_supply_cache[API_SUPPLY_CACHE_SIZE];
static size_t g_api_supply_cache_len = 0;

static char   g_api_hodl_cache[API_HODL_CACHE_SIZE];
static size_t g_api_hodl_cache_len = 0;

#define API_DEEP_STATS_CACHE_SIZE 65536  /* 64KB */
static char   g_api_deep_stats_cache[API_DEEP_STATS_CACHE_SIZE];
static size_t g_api_deep_stats_cache_len = 0;

static volatile int g_api_cache_thread_running = 0;
static pthread_mutex_t g_api_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

void api_set_state(struct main_state *ms, struct tx_mempool *mp,
                    struct coins_view_cache *coins_tip,
                    struct node_db *ndb, const char *datadir)
{
    g_ms = ms;
    g_mp = mp;
    g_coins_tip = coins_tip;
    g_ndb = ndb;
    g_datadir = datadir;
}

void api_set_rpc_backend(const char *rpc_user, const char *rpc_pass,
                          int rpc_port)
{
    if (rpc_user) snprintf(g_rpc_user, sizeof(g_rpc_user), "%s", rpc_user);
    if (rpc_pass) snprintf(g_rpc_pass, sizeof(g_rpc_pass), "%s", rpc_pass);
    if (rpc_port > 0) g_rpc_port = rpc_port;
}

/* ── RPC call to local zclassicd ─────────────────────────── */
/* ONLY called from the background cache thread, never from
 * HTTPS handler threads (socket ops crash in that context). */

static int rpc_call(const char *method, const char *params_json,
                     char *out, size_t outmax)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)g_rpc_port);

    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    char body[4096];
    int blen = snprintf(body, sizeof(body),
        "{\"jsonrpc\":\"1.0\",\"id\":1,\"method\":\"%s\",\"params\":%s}",
        method, params_json);

    /* Base64 encode auth */
    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char auth_plain[256];
    snprintf(auth_plain, sizeof(auth_plain), "%s:%s", g_rpc_user, g_rpc_pass);
    char auth_b64[512];
    size_t alen = strlen(auth_plain), bo = 0;
    for (size_t i = 0; i < alen; i += 3) {
        uint32_t n = ((uint32_t)(uint8_t)auth_plain[i]) << 16;
        if (i + 1 < alen) n |= ((uint32_t)(uint8_t)auth_plain[i+1]) << 8;
        if (i + 2 < alen) n |= (uint32_t)(uint8_t)auth_plain[i+2];
        auth_b64[bo++] = b64[(n >> 18) & 63];
        auth_b64[bo++] = b64[(n >> 12) & 63];
        auth_b64[bo++] = (i + 1 < alen) ? b64[(n >> 6) & 63] : '=';
        auth_b64[bo++] = (i + 2 < alen) ? b64[n & 63] : '=';
    }
    auth_b64[bo] = '\0';

    char req[8192];
    int rlen = snprintf(req, sizeof(req),
        "POST / HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: Basic %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n%s",
        auth_b64, blen, body);

    if (write(fd, req, (size_t)rlen) != rlen) { close(fd); return -1; }

    size_t total = 0;
    while (total < outmax - 1) {
        ssize_t r = read(fd, out + total, outmax - 1 - total);
        if (r <= 0) break;
        total += (size_t)r;
    }
    out[total] = '\0';
    close(fd);

    /* Skip HTTP headers */
    char *body_start = strstr(out, "\r\n\r\n");
    if (body_start) {
        body_start += 4;
        size_t body_len = total - (size_t)(body_start - out);
        memmove(out, body_start, body_len);
        out[body_len] = '\0';
        return (int)body_len;
    }
    return (int)total;
}

/* JSON extraction and validation: delegated to shared format_helpers */
static bool json_extract_str(const char *json, const char *key,
                              char *out, size_t outmax)
{
    return zcl_json_extract_str(json, key, out, outmax);
}

static int64_t json_extract_int(const char *json, const char *key)
{
    int64_t v = -1;
    zcl_json_extract_int(json, key, &v);
    return v;
}

static double json_extract_real(const char *json, const char *key)
{
    double v = 0.0;
    zcl_json_extract_real(json, key, &v);
    return v;
}

static bool is_all_hex(const char *s, size_t len)
{
    return zcl_is_all_hex(s, len);
}

static bool is_all_digits(const char *s)
{
    return zcl_is_all_digits(s);
}

/* Validate address/param is safe to embed in JSON (alphanumeric only).
 * Prevents JSON injection via crafted params. */
static bool is_json_safe_param(const char *s, size_t maxlen)
{
    if (!s || !*s) return false;
    for (size_t i = 0; s[i] && i < maxlen; i++) {
        char c = s[i];
        if (!isalnum((unsigned char)c)) return false;
    }
    return true;
}

/* ── HTTP response helpers ───────────────────────────────── */

#define JSON_HEADERS \
    "HTTP/1.1 200 OK\r\n" \
    "Content-Type: application/json; charset=utf-8\r\n" \
    "Access-Control-Allow-Origin: *\r\n" \
    "Access-Control-Allow-Methods: GET, OPTIONS\r\n" \
    "Access-Control-Allow-Headers: Content-Type\r\n" \
    "Cache-Control: public, max-age=10\r\n" \
    "Connection: close\r\n\r\n"

#define JSON_404_HEADERS \
    "HTTP/1.1 404 Not Found\r\n" \
    "Content-Type: application/json; charset=utf-8\r\n" \
    "Access-Control-Allow-Origin: *\r\n" \
    "Connection: close\r\n\r\n"

#define JSON_500_HEADERS \
    "HTTP/1.1 500 Internal Server Error\r\n" \
    "Content-Type: application/json; charset=utf-8\r\n" \
    "Access-Control-Allow-Origin: *\r\n" \
    "Connection: close\r\n\r\n"

#define JSON_503_HEADERS \
    "HTTP/1.1 503 Service Unavailable\r\n" \
    "Content-Type: application/json; charset=utf-8\r\n" \
    "Access-Control-Allow-Origin: *\r\n" \
    "Retry-After: 10\r\n" \
    "Connection: close\r\n\r\n"

static size_t cors_preflight(uint8_t *r, size_t max)
{
    return (size_t)snprintf((char *)r, max,
        "HTTP/1.1 204 No Content\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Access-Control-Max-Age: 86400\r\n"
        "Connection: close\r\n\r\n");
}

static size_t json_error(uint8_t *r, size_t max, const char *headers,
                          const char *message)
{
    return (size_t)snprintf((char *)r, max,
        "%s{\"error\":\"%s\"}", headers, message);
}

/* ── Compute functions (called ONLY from background thread) ── */

/* Compute /api/blocks — latest 25 blocks */
static size_t compute_blocks(uint8_t *r, size_t max)
{
    char buf[65536];
    size_t off = 0;

    /* Get current height */
    if (rpc_call("getblockcount", "[]", buf, sizeof(buf)) <= 0)
        return json_error(r, max, JSON_500_HEADERS, "RPC unavailable");

    int64_t height = json_extract_int(buf, "result");
    if (height < 0)
        return json_error(r, max, JSON_500_HEADERS, "Cannot get block count");

    off += (size_t)snprintf((char *)r + off, max - off, "%s[", JSON_HEADERS);

    int count = 25;
    if (height < count) count = (int)height + 1;

    for (int i = 0; i < count && off + 512 < max; i++) {
        int64_t h = height - i;
        char params[64];
        snprintf(params, sizeof(params), "[%" PRId64 "]", h);
        if (rpc_call("getblockhash", params, buf, sizeof(buf)) <= 0)
            continue;

        char hash[65] = "";
        json_extract_str(buf, "result", hash, sizeof(hash));
        if (!hash[0]) continue;

        char params2[128];
        snprintf(params2, sizeof(params2), "[\"%s\", true]", hash);
        if (rpc_call("getblock", params2, buf, sizeof(buf)) <= 0)
            continue;

        int64_t blk_time = json_extract_int(buf, "time");
        double diff = json_extract_real(buf, "difficulty");

        /* Count transactions */
        int tx_count = 0;
        const char *txarr = strstr(buf, "\"tx\":[");
        if (txarr) {
            const char *end = strchr(txarr, ']');
            tx_count = 1;
            if (end) for (const char *p = txarr; p < end; p++)
                if (*p == ',') tx_count++;
        }

        if (i > 0) off += (size_t)snprintf((char *)r + off, max - off, ",");
        off += (size_t)snprintf((char *)r + off, max - off,
            "{\"height\":%" PRId64
            ",\"hash\":\"%s\""
            ",\"time\":%" PRId64
            ",\"num_tx\":%d"
            ",\"difficulty\":%.8f}",
            h, hash, blk_time, tx_count, diff);
    }

    off += (size_t)snprintf((char *)r + off, max - off, "]");
    return off;
}

/* Compute /api/stats — network stats */
static size_t compute_stats(uint8_t *r, size_t max)
{
    char buf[65536];

    /* Get blockchain info */
    if (rpc_call("getblockchaininfo", "[]", buf, sizeof(buf)) <= 0)
        return json_error(r, max, JSON_500_HEADERS, "RPC unavailable");

    int64_t height = json_extract_int(buf, "blocks");
    double diff = json_extract_real(buf, "difficulty");

    char chain[32] = "";
    json_extract_str(buf, "chain", chain, sizeof(chain));

    /* Get mining info for hashrate */
    char mbuf[8192];
    double hashrate = 0.0;
    if (rpc_call("getmininginfo", "[]", mbuf, sizeof(mbuf)) > 0)
        hashrate = json_extract_real(mbuf, "networkhashps");

    double supply = (double)zcl_total_supply_zatoshi(height) / 100000000.0;

    /* UTXO count via gettxoutsetinfo — expensive, skip if too slow */
    int64_t utxo_count = -1;
    char ubuf[8192];
    if (rpc_call("gettxoutsetinfo", "[]", ubuf, sizeof(ubuf)) > 0)
        utxo_count = json_extract_int(ubuf, "txouts");

    size_t off = 0;
    off += (size_t)snprintf((char *)r + off, max - off,
        "%s{"
        "\"height\":%" PRId64
        ",\"difficulty\":%.8f"
        ",\"networkhashps\":%.2f"
        ",\"supply\":%.8f"
        ",\"chain\":\"%s\"",
        JSON_HEADERS,
        height, diff, hashrate, supply, chain);

    if (utxo_count >= 0)
        off += (size_t)snprintf((char *)r + off, max - off,
            ",\"utxo_count\":%" PRId64, utxo_count);

    off += (size_t)snprintf((char *)r + off, max - off, "}");
    return off;
}

/* Compute /api/supply — circulating supply (CoinGecko format) */
static size_t compute_supply(uint8_t *r, size_t max)
{
    char buf[8192];

    if (rpc_call("getblockcount", "[]", buf, sizeof(buf)) <= 0)
        return json_error(r, max, JSON_500_HEADERS, "RPC unavailable");

    int64_t height = json_extract_int(buf, "result");
    if (height < 0)
        return json_error(r, max, JSON_500_HEADERS, "Cannot get height");

    double supply = (double)zcl_total_supply_zatoshi(height) / 100000000.0;

    /* Plain number -- CoinGecko expects just a number */
    return (size_t)snprintf((char *)r, max,
        "%s%.8f", JSON_HEADERS, supply);
}

/* Compute /api/hodl — HODL wave data */
static size_t compute_hodl(uint8_t *r, size_t max)
{
    char buf[65536];

    /* Get current height and time */
    if (rpc_call("getblockcount", "[]", buf, sizeof(buf)) <= 0)
        return json_error(r, max, JSON_500_HEADERS, "RPC unavailable");

    int64_t height = json_extract_int(buf, "result");
    if (height < 0)
        return json_error(r, max, JSON_500_HEADERS, "Cannot get height");

    /* Get current block time */
    char params[64];
    snprintf(params, sizeof(params), "[%" PRId64 "]", height);
    if (rpc_call("getblockhash", params, buf, sizeof(buf)) <= 0)
        return json_error(r, max, JSON_500_HEADERS, "RPC unavailable");

    char hash[65] = "";
    json_extract_str(buf, "result", hash, sizeof(hash));

    char params2[128];
    snprintf(params2, sizeof(params2), "[\"%s\", true]", hash);
    if (rpc_call("getblock", params2, buf, sizeof(buf)) <= 0)
        return json_error(r, max, JSON_500_HEADERS, "RPC unavailable");

    int64_t tip_time = json_extract_int(buf, "time");

    size_t off = 0;
    off += (size_t)snprintf((char *)r + off, max - off,
        "%s{"
        "\"height\":%" PRId64
        ",\"time\":%" PRId64
        ",\"description\":\"HODL wave data — ratio of UTXOs by age band\""
        ",\"bands\":[\"<1d\",\"1d-1w\",\"1w-1m\",\"1m-3m\",\"3m-6m\",\"6m-1y\",\"1y+\"]"
        ",\"note\":\"Detailed HODL data available at /explorer/hodl\""
        "}",
        JSON_HEADERS, height, tip_time);

    return off;
}

/* Compute /api/block/:id — block detail (called from bg thread) */
static size_t compute_block(const char *param, uint8_t *r, size_t max)
{
    if (!param || !*param)
        return json_error(r, max, JSON_404_HEADERS, "Missing block identifier");

    char buf[262144];
    char hash[65] = "";

    /* Resolve height to hash if needed */
    if (is_all_digits(param)) {
        char params[64];
        snprintf(params, sizeof(params), "[%s]", param);
        if (rpc_call("getblockhash", params, buf, sizeof(buf)) <= 0)
            return json_error(r, max, JSON_500_HEADERS, "RPC unavailable");
        json_extract_str(buf, "result", hash, sizeof(hash));
    } else if (strlen(param) == 64 && is_all_hex(param, 64)) {
        snprintf(hash, sizeof(hash), "%s", param);
    }

    if (!hash[0])
        return json_error(r, max, JSON_404_HEADERS, "Block not found");

    /* Get full block */
    char params2[128];
    snprintf(params2, sizeof(params2), "[\"%s\", true]", hash);
    if (rpc_call("getblock", params2, buf, sizeof(buf)) <= 0)
        return json_error(r, max, JSON_500_HEADERS, "RPC unavailable");

    if (strstr(buf, "\"error\":null") == NULL)
        return json_error(r, max, JSON_404_HEADERS, "Block not found");

    int64_t height = json_extract_int(buf, "height");
    int64_t blk_time = json_extract_int(buf, "time");
    int64_t blk_size = json_extract_int(buf, "size");
    double diff = json_extract_real(buf, "difficulty");

    char merkle[65] = "", prev[65] = "", next_hash[65] = "", nonce[65] = "";
    json_extract_str(buf, "merkleroot", merkle, sizeof(merkle));
    json_extract_str(buf, "previousblockhash", prev, sizeof(prev));
    json_extract_str(buf, "nextblockhash", next_hash, sizeof(next_hash));
    json_extract_str(buf, "nonce", nonce, sizeof(nonce));

    int64_t confirmations = json_extract_int(buf, "confirmations");

    /* Count and collect transaction IDs */
    int tx_count = 0;
    const char *txarr = strstr(buf, "\"tx\":[");
    if (txarr) {
        const char *end = strchr(txarr, ']');
        tx_count = 1;
        if (end) for (const char *p = txarr; p < end; p++)
            if (*p == ',') tx_count++;
    }

    size_t off = 0;
    off += (size_t)snprintf((char *)r + off, max - off,
        "%s{"
        "\"hash\":\"%s\""
        ",\"height\":%" PRId64
        ",\"time\":%" PRId64
        ",\"size\":%" PRId64
        ",\"difficulty\":%.8f"
        ",\"confirmations\":%" PRId64
        ",\"num_tx\":%d"
        ",\"merkleroot\":\"%s\""
        ",\"nonce\":\"%s\"",
        JSON_HEADERS,
        hash, height, blk_time, blk_size, diff,
        confirmations, tx_count, merkle, nonce);

    if (prev[0])
        off += (size_t)snprintf((char *)r + off, max - off,
            ",\"previousblockhash\":\"%s\"", prev);
    if (next_hash[0])
        off += (size_t)snprintf((char *)r + off, max - off,
            ",\"nextblockhash\":\"%s\"", next_hash);

    /* Transaction ID array */
    off += (size_t)snprintf((char *)r + off, max - off, ",\"tx\":[");
    if (txarr) {
        const char *p = txarr + 6; /* skip "tx":[ */
        int idx = 0;
        while (p && idx < 200 && off + 128 < max) {
            if (*p == '"') {
                p++;
                const char *end = strchr(p, '"');
                if (!end) break;
                size_t tlen = (size_t)(end - p);
                if (tlen > 64) tlen = 64;
                if (idx > 0)
                    off += (size_t)snprintf((char *)r + off, max - off, ",");
                off += (size_t)snprintf((char *)r + off, max - off,
                    "\"%.*s\"", (int)tlen, p);
                idx++;
                p = end + 1;
            } else if (*p == ']') {
                break;
            } else {
                p++;
            }
        }
    }
    off += (size_t)snprintf((char *)r + off, max - off, "]}");
    return off;
}

/* Compute /api/tx/:txid — transaction detail (called from bg thread) */
static size_t compute_tx(const char *param, uint8_t *r, size_t max)
{
    if (!param || strlen(param) != 64 || !is_all_hex(param, 64))
        return json_error(r, max, JSON_404_HEADERS, "Invalid transaction ID");

    char buf[262144];
    char params[128];
    snprintf(params, sizeof(params), "[\"%s\", 1]", param);
    int n = rpc_call("getrawtransaction", params, buf, sizeof(buf));
    if (n <= 0 || strstr(buf, "\"error\":null") == NULL)
        return json_error(r, max, JSON_404_HEADERS, "Transaction not found");

    /* Extract the result object */
    const char *result = strstr(buf, "\"result\":{");
    if (!result) result = buf;

    int64_t confirmations = json_extract_int(result, "confirmations");
    int64_t blk_height = json_extract_int(result, "height");
    int64_t tx_size = json_extract_int(result, "size");
    int64_t version = json_extract_int(result, "version");
    int64_t locktime = json_extract_int(result, "locktime");
    double value_balance = json_extract_real(result, "valuebalance");

    char blockhash[65] = "";
    json_extract_str(result, "blockhash", blockhash, sizeof(blockhash));

    size_t off = 0;
    off += (size_t)snprintf((char *)r + off, max - off,
        "%s{"
        "\"txid\":\"%s\""
        ",\"version\":%" PRId64
        ",\"size\":%" PRId64
        ",\"locktime\":%" PRId64
        ",\"confirmations\":%" PRId64
        ",\"blockhash\":\"%s\""
        ",\"blockheight\":%" PRId64
        ",\"valuebalance\":%.8f",
        JSON_HEADERS,
        param, version, tx_size, locktime,
        confirmations, blockhash, blk_height, value_balance);

    /* Parse vout array */
    off += (size_t)snprintf((char *)r + off, max - off, ",\"vout\":[");
    const char *vout = strstr(result, "\"vout\":[");
    if (vout) {
        const char *p = vout + 7;
        int brace = 0, idx = 0;
        while (*p && off + 512 < max) {
            if (*p == '{') {
                brace++;
                if (brace == 1) {
                    const char *entry = p;
                    int depth = 0;
                    const char *entry_end = NULL;
                    for (const char *q = p; *q; q++) {
                        if (*q == '{') depth++;
                        if (*q == '}') { depth--; if (depth == 0) { entry_end = q + 1; break; } }
                    }
                    if (!entry_end) break;

                    double val = json_extract_real(entry, "value");
                    int64_t vn = json_extract_int(entry, "n");

                    char addr[64] = "";
                    const char *addrs = strstr(entry, "\"addresses\":[\"");
                    if (addrs && addrs < entry_end) {
                        addrs += 14;
                        size_t ai = 0;
                        while (addrs[ai] && addrs[ai] != '"' && ai < sizeof(addr) - 1) {
                            addr[ai] = addrs[ai]; ai++;
                        }
                        addr[ai] = '\0';
                    }

                    if (idx > 0)
                        off += (size_t)snprintf((char *)r + off, max - off, ",");
                    off += (size_t)snprintf((char *)r + off, max - off,
                        "{\"n\":%" PRId64 ",\"value\":%.8f", vn, val);
                    if (addr[0])
                        off += (size_t)snprintf((char *)r + off, max - off,
                            ",\"address\":\"%s\"", addr);
                    off += (size_t)snprintf((char *)r + off, max - off, "}");
                    idx++;

                    p = entry_end;
                    brace = 0;
                    continue;
                }
            }
            if (*p == ']' && brace == 0) break;
            p++;
        }
    }
    off += (size_t)snprintf((char *)r + off, max - off, "]");

    /* Parse vin array */
    off += (size_t)snprintf((char *)r + off, max - off, ",\"vin\":[");
    const char *vin = strstr(result, "\"vin\":[");
    if (vin) {
        const char *p = vin + 6;
        int brace = 0, idx = 0;
        while (*p && off + 256 < max) {
            if (*p == '{') {
                brace++;
                if (brace == 1) {
                    const char *entry = p;
                    int depth = 0;
                    const char *entry_end = NULL;
                    for (const char *q = p; *q; q++) {
                        if (*q == '{') depth++;
                        if (*q == '}') { depth--; if (depth == 0) { entry_end = q + 1; break; } }
                    }
                    if (!entry_end) break;

                    char prev_txid[65] = "";
                    json_extract_str(entry, "txid", prev_txid, sizeof(prev_txid));
                    int64_t vout_n = json_extract_int(entry, "vout");

                    if (idx > 0)
                        off += (size_t)snprintf((char *)r + off, max - off, ",");

                    /* Check for coinbase */
                    if (strstr(entry, "\"coinbase\"") && (const char*)strstr(entry, "\"coinbase\"") < entry_end) {
                        off += (size_t)snprintf((char *)r + off, max - off,
                            "{\"coinbase\":true}");
                    } else {
                        off += (size_t)snprintf((char *)r + off, max - off,
                            "{\"txid\":\"%s\",\"vout\":%" PRId64 "}",
                            prev_txid, vout_n);
                    }
                    idx++;
                    p = entry_end;
                    brace = 0;
                    continue;
                }
            }
            if (*p == ']' && brace == 0) break;
            p++;
        }
    }
    off += (size_t)snprintf((char *)r + off, max - off, "]}");
    return off;
}

/* Compute /api/address/:addr — balance + UTXOs (called from bg thread) */
static size_t compute_address(const char *param, uint8_t *r, size_t max)
{
    if (!param || !*param)
        return json_error(r, max, JSON_404_HEADERS, "Missing address");

    size_t alen = strlen(param);
    if (alen < 25 || alen > 95)
        return json_error(r, max, JSON_404_HEADERS, "Invalid address");

    /* Ensure address is safe to embed in JSON/RPC params */
    if (!is_json_safe_param(param, alen))
        return json_error(r, max, JSON_404_HEADERS, "Invalid address characters");

    char buf[262144];
    size_t off = 0;

    /* Try getaddressbalance (addressindex RPC) */
    char params[256];
    snprintf(params, sizeof(params),
        "[{\"addresses\":[\"%s\"]}]", param);
    int n = rpc_call("getaddressbalance", params, buf, sizeof(buf));

    int64_t balance_sat = 0;
    bool got_balance = false;

    if (n > 0 && strstr(buf, "\"error\":null")) {
        balance_sat = json_extract_int(buf, "balance");
        got_balance = true;
    }

    /* Try getaddressutxos */
    char ubuf[262144];
    n = rpc_call("getaddressutxos", params, ubuf, sizeof(ubuf));

    off += (size_t)snprintf((char *)r + off, max - off,
        "%s{\"address\":\"%s\"", JSON_HEADERS, param);

    if (got_balance) {
        off += (size_t)snprintf((char *)r + off, max - off,
            ",\"balance_sat\":%" PRId64
            ",\"balance\":%.8f",
            balance_sat, (double)balance_sat / 100000000.0);
    }

    /* Parse UTXOs from result array */
    off += (size_t)snprintf((char *)r + off, max - off, ",\"utxos\":[");

    if (n > 0 && strstr(ubuf, "\"error\":null")) {
        const char *result = strstr(ubuf, "\"result\":[");
        if (result) {
            const char *p = result + 10;
            int brace = 0, idx = 0;
            while (*p && off + 512 < max) {
                if (*p == '{') {
                    brace++;
                    if (brace == 1) {
                        const char *entry = p;
                        int depth = 0;
                        const char *entry_end = NULL;
                        for (const char *q = p; *q; q++) {
                            if (*q == '{') depth++;
                            if (*q == '}') { depth--; if (depth == 0) { entry_end = q + 1; break; } }
                        }
                        if (!entry_end) break;

                        char txid[65] = "";
                        json_extract_str(entry, "txid", txid, sizeof(txid));
                        int64_t output_idx = json_extract_int(entry, "outputIndex");
                        int64_t satoshis = json_extract_int(entry, "satoshis");
                        int64_t utxo_height = json_extract_int(entry, "height");

                        if (idx > 0)
                            off += (size_t)snprintf((char *)r + off, max - off, ",");
                        off += (size_t)snprintf((char *)r + off, max - off,
                            "{\"txid\":\"%s\""
                            ",\"vout\":%" PRId64
                            ",\"satoshis\":%" PRId64
                            ",\"value\":%.8f"
                            ",\"height\":%" PRId64 "}",
                            txid, output_idx, satoshis,
                            (double)satoshis / 100000000.0, utxo_height);
                        idx++;

                        p = entry_end;
                        brace = 0;
                        continue;
                    }
                }
                if (*p == ']' && brace == 0) break;
                p++;
            }
        }
    }

    off += (size_t)snprintf((char *)r + off, max - off, "]}");
    return off;
}

/* ── Deep stats computation (SQLite-based) ───────────────── */

/* sql_query_i64() provided by controllers/explorer_internal.h */
#define dq_i64 sql_query_i64

static size_t compute_deep_stats(uint8_t *r, size_t max)
{
    if (!g_datadir) return json_error(r, max, JSON_500_HEADERS, "No datadir");

    char dbpath[1024];
    snprintf(dbpath, sizeof(dbpath), "%s/node.db", g_datadir);

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(dbpath, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return json_error(r, max, JSON_500_HEADERS, "Cannot open database");
    }
    sqlite3_busy_timeout(db, 30000);

    int64_t height = dq_i64(db, "SELECT MAX(height) FROM blocks");
    int64_t block_count = dq_i64(db, "SELECT count(*) FROM blocks");
    int64_t tx_count = dq_i64(db, "SELECT count(*) FROM transactions");
    int64_t utxo_count = dq_i64(db, "SELECT count(*) FROM utxos");
    int64_t dust_count = dq_i64(db, "SELECT count(*) FROM utxos WHERE value < 100000");
    int64_t addr_total = dq_i64(db, "SELECT count(*) FROM addresses");
    int64_t addr_nonzero = dq_i64(db, "SELECT count(*) FROM addresses WHERE balance > 0");

    /* Sprout stats */
    int64_t js_count = dq_i64(db, "SELECT count(*) FROM joinsplits");
    int64_t js_first = dq_i64(db, "SELECT MIN(block_height) FROM joinsplits");

    /* Sapling stats */
    int64_t ss_count = dq_i64(db, "SELECT count(*) FROM sapling_spends");
    int64_t so_count = dq_i64(db, "SELECT count(*) FROM sapling_outputs");
    int64_t ss_first = dq_i64(db, "SELECT MIN(block_height) FROM sapling_spends");

    /* ZSLP stats */
    int64_t token_count = dq_i64(db, "SELECT count(*) FROM zslp_tokens");
    int64_t transfer_count = dq_i64(db, "SELECT count(*) FROM zslp_transfers");

    int64_t supply_sat = zcl_total_supply_zatoshi(height);

    /* Shielded supply from blocks table */
    int64_t shielded_net = dq_i64(db, "SELECT COALESCE(SUM(sapling_value), 0) FROM blocks");

    /* Integrity: checkpoint count and latest block hash */
    char latest_hash[128] = "";
    {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db,
            "SELECT hex(hash) FROM blocks WHERE height = (SELECT MAX(height) FROM blocks)",
            -1, &s, NULL) == SQLITE_OK && s) {
            if (sqlite3_step(s) == SQLITE_ROW) {
                const char *h = (const char *)sqlite3_column_text(s, 0);
                if (h) snprintf(latest_hash, sizeof(latest_hash), "%s", h);
            }
            sqlite3_finalize(s);
        }
    }

    sqlite3_close(db);

    size_t off = 0;
    off += (size_t)snprintf((char *)r + off, max - off,
        "%s{"
        "\"height\":%" PRId64
        ",\"blocks\":%" PRId64
        ",\"transactions\":%" PRId64
        ",\"supply\":%.8f"
        ",\"shielded_net\":%.8f"
        ",\"sprout\":{\"joinsplits\":%" PRId64 ",\"first_height\":%" PRId64 "}"
        ",\"sapling\":{\"spends\":%" PRId64 ",\"outputs\":%" PRId64
            ",\"first_height\":%" PRId64 "}"
        ",\"utxo\":{\"count\":%" PRId64 ",\"dust_under_0001\":%" PRId64 "}"
        ",\"addresses\":{\"total\":%" PRId64 ",\"nonzero\":%" PRId64 "}"
        ",\"zslp\":{\"tokens\":%" PRId64 ",\"transfers\":%" PRId64 "}"
        ",\"integrity\":{\"indexed_blocks\":%" PRId64
            ",\"latest_hash\":\"%s\"}"
        "}",
        JSON_HEADERS,
        height, block_count, tx_count,
        (double)supply_sat / 100000000.0,
        (double)shielded_net / 100000000.0,
        js_count, js_first,
        ss_count, so_count, ss_first,
        utxo_count, dust_count,
        addr_total, addr_nonzero,
        token_count, transfer_count,
        block_count, latest_hash);

    return off;
}

/* ── Background cache refresh thread ─────────────────────── */
/* This thread periodically calls rpc_call (safe here -- normal
 * thread, not an HTTPS handler) and updates the static caches.
 * HTTPS handlers only read from cache, never call rpc_call. */

static void *api_cache_refresh_thread(void *arg)
{
    (void)arg;

    /* Wait for RPC server to start */
    sleep(5);

    printf("API cache: background refresh thread started\n");
    fflush(stdout);

    int iteration = 0;
    while (g_api_cache_thread_running) {
        /* Refresh /api/blocks every 30 seconds */
        if (iteration % 3 == 0) {
            uint8_t *tmp = malloc(API_BLOCKS_CACHE_SIZE);
            if (tmp) {
                size_t len = compute_blocks(tmp, API_BLOCKS_CACHE_SIZE);
                if (len > 0 && len < API_BLOCKS_CACHE_SIZE) {
                    pthread_mutex_lock(&g_api_cache_mutex);
                    memcpy(g_api_blocks_cache, tmp, len);
                    g_api_blocks_cache_len = len;
                    pthread_mutex_unlock(&g_api_cache_mutex);
                }
                free(tmp);
            }
        }

        /* Refresh /api/stats every 60 seconds */
        if (iteration % 6 == 0) {
            uint8_t *tmp = malloc(API_STATS_CACHE_SIZE);
            if (tmp) {
                size_t len = compute_stats(tmp, API_STATS_CACHE_SIZE);
                if (len > 0 && len < API_STATS_CACHE_SIZE) {
                    pthread_mutex_lock(&g_api_cache_mutex);
                    memcpy(g_api_stats_cache, tmp, len);
                    g_api_stats_cache_len = len;
                    pthread_mutex_unlock(&g_api_cache_mutex);
                }
                free(tmp);
            }
        }

        /* Refresh /api/supply every 60 seconds */
        if (iteration % 6 == 0) {
            uint8_t *tmp = malloc(API_SUPPLY_CACHE_SIZE);
            if (tmp) {
                size_t len = compute_supply(tmp, API_SUPPLY_CACHE_SIZE);
                if (len > 0 && len < API_SUPPLY_CACHE_SIZE) {
                    pthread_mutex_lock(&g_api_cache_mutex);
                    memcpy(g_api_supply_cache, tmp, len);
                    g_api_supply_cache_len = len;
                    pthread_mutex_unlock(&g_api_cache_mutex);
                }
                free(tmp);
            }
        }

        /* Refresh /api/hodl every 60 seconds */
        if (iteration % 6 == 0) {
            uint8_t *tmp = malloc(API_HODL_CACHE_SIZE);
            if (tmp) {
                size_t len = compute_hodl(tmp, API_HODL_CACHE_SIZE);
                if (len > 0 && len < API_HODL_CACHE_SIZE) {
                    pthread_mutex_lock(&g_api_cache_mutex);
                    memcpy(g_api_hodl_cache, tmp, len);
                    g_api_hodl_cache_len = len;
                    pthread_mutex_unlock(&g_api_cache_mutex);
                }
                free(tmp);
            }
        }

        /* Refresh /api/stats/deep every 300 seconds (30 iterations) */
        if (iteration % 30 == 0) {
            uint8_t *tmp = malloc(API_DEEP_STATS_CACHE_SIZE);
            if (tmp) {
                size_t len = compute_deep_stats(tmp, API_DEEP_STATS_CACHE_SIZE);
                if (len > 0 && len < API_DEEP_STATS_CACHE_SIZE) {
                    pthread_mutex_lock(&g_api_cache_mutex);
                    memcpy(g_api_deep_stats_cache, tmp, len);
                    g_api_deep_stats_cache_len = len;
                    pthread_mutex_unlock(&g_api_cache_mutex);
                }
                free(tmp);
            }
        }

        if (iteration == 0)
            printf("API cache: initial refresh complete (blocks=%zu stats=%zu supply=%zu hodl=%zu)\n",
                   g_api_blocks_cache_len, g_api_stats_cache_len,
                   g_api_supply_cache_len, g_api_hodl_cache_len);

        iteration++;
        /* Sleep 10 seconds between iterations; blocks refresh every 3rd (30s),
         * stats/supply/hodl every 6th (60s) */
        for (int s = 0; s < 10 && g_api_cache_thread_running; s++)
            sleep(1);
    }

    printf("API cache: background refresh thread stopped\n");
    fflush(stdout);
    return NULL;
}

static void ensure_cache_thread(void)
{
    if (g_api_cache_thread_running) return;
    g_api_cache_thread_running = 1;

    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 2 * 1024 * 1024);
    pthread_create(&t, &attr, api_cache_refresh_thread, NULL);
    pthread_attr_destroy(&attr);
}

void api_start_cache(void)
{
    ensure_cache_thread();
}

/* ── Serve from cache helpers ────────────────────────────── */

static size_t serve_from_cache(const char *cache, size_t cache_len,
                                uint8_t *r, size_t max)
{
    if (cache_len == 0)
        return json_error(r, max, JSON_503_HEADERS,
                          "Data loading, please retry in a few seconds");
    size_t copy = cache_len < max ? cache_len : max;
    pthread_mutex_lock(&g_api_cache_mutex);
    memcpy(r, cache, copy);
    pthread_mutex_unlock(&g_api_cache_mutex);
    return copy;
}

/* ── Parameterized endpoint request queue ────────────────── */
/* For /api/block/:id, /api/tx/:txid, /api/address/:addr we
 * submit the request to the background thread and serve from
 * a small per-request cache. Uses a simple single-slot queue. */

static pthread_mutex_t g_lookup_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_lookup_request_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  g_lookup_done_cond = PTHREAD_COND_INITIALIZER;

enum lookup_type { LOOKUP_NONE = 0, LOOKUP_BLOCK, LOOKUP_TX, LOOKUP_ADDRESS };

static volatile enum lookup_type g_lookup_type = LOOKUP_NONE;
static char    g_lookup_param[512];
static uint8_t g_lookup_result[262144];
static size_t  g_lookup_result_len = 0;
static volatile int g_lookup_thread_running = 0;

/* Background thread that processes lookup requests one at a time */
static void *api_lookup_thread(void *arg)
{
    (void)arg;
    printf("API lookup: background thread started\n");
    fflush(stdout);

    while (g_lookup_thread_running) {
        pthread_mutex_lock(&g_lookup_mutex);
        while (g_lookup_type == LOOKUP_NONE && g_lookup_thread_running) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&g_lookup_request_cond, &g_lookup_mutex, &ts);
        }
        if (!g_lookup_thread_running) {
            pthread_mutex_unlock(&g_lookup_mutex);
            break;
        }

        enum lookup_type type = g_lookup_type;
        char param[512];
        snprintf(param, sizeof(param), "%s", g_lookup_param);
        pthread_mutex_unlock(&g_lookup_mutex);

        size_t len = 0;
        switch (type) {
        case LOOKUP_BLOCK:
            len = compute_block(param, g_lookup_result, sizeof(g_lookup_result));
            break;
        case LOOKUP_TX:
            len = compute_tx(param, g_lookup_result, sizeof(g_lookup_result));
            break;
        case LOOKUP_ADDRESS:
            len = compute_address(param, g_lookup_result, sizeof(g_lookup_result));
            break;
        default:
            break;
        }

        pthread_mutex_lock(&g_lookup_mutex);
        g_lookup_result_len = len;
        g_lookup_type = LOOKUP_NONE;
        pthread_cond_broadcast(&g_lookup_done_cond);
        pthread_mutex_unlock(&g_lookup_mutex);
    }

    printf("API lookup: background thread stopped\n");
    fflush(stdout);
    return NULL;
}

static void ensure_lookup_thread(void)
{
    if (g_lookup_thread_running) return;
    g_lookup_thread_running = 1;

    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 2 * 1024 * 1024);
    pthread_create(&t, &attr, api_lookup_thread, NULL);
    pthread_attr_destroy(&attr);
}

/* Submit a lookup request and wait for result (with timeout) */
static size_t do_lookup(enum lookup_type type, const char *param,
                         uint8_t *response, size_t response_max)
{
    ensure_lookup_thread();

    pthread_mutex_lock(&g_lookup_mutex);

    /* If another request is pending, return 503 */
    if (g_lookup_type != LOOKUP_NONE) {
        pthread_mutex_unlock(&g_lookup_mutex);
        return json_error(response, response_max, JSON_503_HEADERS,
                          "Server busy, please retry");
    }

    snprintf(g_lookup_param, sizeof(g_lookup_param), "%s", param);
    g_lookup_result_len = 0;
    g_lookup_type = type;
    pthread_cond_signal(&g_lookup_request_cond);

    /* Wait up to 15 seconds for result */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 15;

    while (g_lookup_type != LOOKUP_NONE) {
        int rc = pthread_cond_timedwait(&g_lookup_done_cond, &g_lookup_mutex, &ts);
        if (rc != 0) {
            /* Timeout */
            pthread_mutex_unlock(&g_lookup_mutex);
            return json_error(response, response_max, JSON_503_HEADERS,
                              "Request timed out");
        }
    }

    size_t len = g_lookup_result_len;
    if (len > 0) {
        size_t copy = len < response_max ? len : response_max;
        memcpy(response, g_lookup_result, copy);
        pthread_mutex_unlock(&g_lookup_mutex);
        return copy;
    }

    pthread_mutex_unlock(&g_lookup_mutex);
    return json_error(response, response_max, JSON_500_HEADERS, "RPC unavailable");
}

/* ── Main router ─────────────────────────────────────────── */
/* IMPORTANT: This function is called from HTTPS handler threads.
 * It must NEVER call rpc_call directly. All data comes from
 * background caches or the lookup thread. */

size_t api_handle_request(const char *method, const char *path,
                           const uint8_t *body, size_t body_len,
                           uint8_t *response, size_t response_max)
{
    (void)body; (void)body_len;
    if (!method || !path || !response || response_max == 0) return 0;

    /* Start background cache thread on first request */
    ensure_cache_thread();

    /* Handle CORS preflight */
    if (strcmp(method, "OPTIONS") == 0)
        return cors_preflight(response, response_max);

    /* Only GET requests */
    if (strcmp(method, "GET") != 0)
        return json_error(response, response_max,
            "HTTP/1.1 405 Method Not Allowed\r\n"
            "Content-Type: application/json\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Connection: close\r\n\r\n",
            "Method not allowed");

    /* Strip trailing slash */
    char clean_path[512];
    snprintf(clean_path, sizeof(clean_path), "%s", path);
    size_t plen = strlen(clean_path);
    if (plen > 1 && clean_path[plen - 1] == '/')
        clean_path[plen - 1] = '\0';

    /* Route: /api/blocks — served from cache */
    if (strcmp(clean_path, "/api/blocks") == 0)
        return serve_from_cache(g_api_blocks_cache, g_api_blocks_cache_len,
                                response, response_max);

    /* Route: /api/block/:id — served via lookup thread */
    if (strncmp(clean_path, "/api/block/", 11) == 0 && clean_path[11])
        return do_lookup(LOOKUP_BLOCK, clean_path + 11, response, response_max);

    /* Route: /api/tx/:txid — served via lookup thread */
    if (strncmp(clean_path, "/api/tx/", 8) == 0 && clean_path[8])
        return do_lookup(LOOKUP_TX, clean_path + 8, response, response_max);

    /* Route: /api/address/:addr — served via lookup thread */
    if (strncmp(clean_path, "/api/address/", 13) == 0 && clean_path[13])
        return do_lookup(LOOKUP_ADDRESS, clean_path + 13, response, response_max);

    /* Route: /api/stats — served from cache */
    if (strcmp(clean_path, "/api/stats") == 0)
        return serve_from_cache(g_api_stats_cache, g_api_stats_cache_len,
                                response, response_max);

    /* Route: /api/stats/deep — deep stats served from cache */
    if (strcmp(clean_path, "/api/stats/deep") == 0)
        return serve_from_cache(g_api_deep_stats_cache, g_api_deep_stats_cache_len,
                                response, response_max);

    /* Route: /api/supply — served from cache */
    if (strcmp(clean_path, "/api/supply") == 0)
        return serve_from_cache(g_api_supply_cache, g_api_supply_cache_len,
                                response, response_max);

    /* Route: /api/hodl — served from cache */
    if (strcmp(clean_path, "/api/hodl") == 0)
        return serve_from_cache(g_api_hodl_cache, g_api_hodl_cache_len,
                                response, response_max);

    /* Route: /api/factoids — built from SQLite (read-only, safe from handler) */
    if (strcmp(clean_path, "/api/factoids") == 0) {
        if (!g_datadir)
            return json_error(response, response_max, JSON_500_HEADERS, "No datadir");
        return explorer_factoids_build_json(response, response_max, g_datadir);
    }

    /* Event log — lock-free atomic reads, safe from any handler thread */
    if (strncmp(clean_path, "/api/events", 11) == 0 &&
        (clean_path[11] == '\0' || clean_path[11] == '?')) {
        size_t count = 200;
        const char *q = strchr(path, '?');
        if (q) {
            const char *cp = strstr(q, "count=");
            if (cp) {
                long v = strtol(cp + 6, NULL, 10);
                if (v > 0 && v <= 65536) count = (size_t)v;
            }
        }
        /* Build JSON body: {"sync_state":"...","events":[...]} */
        char *buf = malloc(524288);
        if (!buf)
            return json_error(response, response_max, JSON_500_HEADERS,
                              "Out of memory");
        size_t w = 0;
        w += (size_t)snprintf(buf + w, 524288 - w,
            "{\"sync_state\":\"%s\",\"events\":",
            sync_state_name(sync_get_state()));
        /* Parse ?type= filter from query string */
        const char *type_filter = NULL;
        if (q) {
            const char *tp = strstr(q, "type=");
            if (tp) {
                static char type_buf[64];
                size_t tlen = 0;
                for (const char *c = tp + 5; *c && *c != '&' && tlen < 63; c++)
                    type_buf[tlen++] = *c;
                type_buf[tlen] = '\0';
                type_filter = type_buf;
            }
        }
        if (type_filter)
            w += event_dump_json_filtered(buf + w, 524288 - w, count, type_filter);
        else
            w += event_dump_json(buf + w, 524288 - w, count);
        if (w + 1 < 524288) buf[w++] = '}';

        size_t off = (size_t)snprintf((char *)response, response_max,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json; charset=utf-8\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n"
            "Content-Length: %zu\r\n\r\n", w);
        if (off + w <= response_max)
            memcpy(response + off, buf, w);
        else if (off < response_max)
            memcpy(response + off, buf, response_max - off);
        free(buf);
        return off + w < response_max ? off + w : response_max;
    }

    /* Sync state — minimal monitoring endpoint */
    if (strcmp(clean_path, "/api/syncstate") == 0) {
        return (size_t)snprintf((char *)response, response_max,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n\r\n"
            "{\"sync_state\":\"%s\"}",
            sync_state_name(sync_get_state()));
    }

    /* Download stats — IBD progress monitoring */
    if (strcmp(clean_path, "/api/downloadstats") == 0) {
        struct download_manager *dm = msg_get_download_mgr();
        uint64_t req = 0, recv = 0, tout = 0, inflight = 0, queued = 0;
        dl_get_stats(dm, &req, &recv, &tout, &inflight, &queued);
        return (size_t)snprintf((char *)response, response_max,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n\r\n"
            "{\"sync_state\":\"%s\","
            "\"requested\":%llu,\"received\":%llu,"
            "\"timed_out\":%llu,\"in_flight\":%llu,"
            "\"queued\":%llu,\"assume_valid_height\":%d}",
            sync_state_name(sync_get_state()),
            (unsigned long long)req, (unsigned long long)recv,
            (unsigned long long)tout, (unsigned long long)inflight,
            (unsigned long long)queued, g_assume_valid_height);
    }

    /* Health check — lightweight, machine-readable */
    if (strcmp(clean_path, "/api/health") == 0) {
        enum sync_state ss = sync_get_state();
        bool healthy = (ss == SYNC_AT_TIP);
        return (size_t)snprintf((char *)response, response_max,
            "HTTP/1.1 %s\r\n"
            "Content-Type: application/json\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n\r\n"
            "{\"healthy\":%s,\"sync_state\":\"%s\"}",
            healthy ? "200 OK" : "503 Service Unavailable",
            healthy ? "true" : "false",
            sync_state_name(ss));
    }

    /* Wallet data — balance, address, activity */
    if (strcmp(clean_path, "/api/wallet") == 0) {
        extern struct node_db *g_active_node_db;
        if (!g_active_node_db || !g_active_node_db->db) {
            return json_error(response, response_max, JSON_500_HEADERS,
                              "No database");
        }
        sqlite3 *db = g_active_node_db->db;
        sqlite3_stmt *s = NULL;

        /* Balance */
        int64_t transparent = 0;
        if (sqlite3_prepare_v2(db,
                "SELECT COALESCE(SUM(u.value),0) FROM utxos u"
                " INNER JOIN wallet_keys w ON u.address_hash=w.pubkey_hash",
                -1, &s, NULL) == SQLITE_OK && s) {
            if (sqlite3_step(s) == SQLITE_ROW)
                transparent = sqlite3_column_int64(s, 0);
            sqlite3_finalize(s);
        }

        int64_t shielded = 0;
        if (sqlite3_prepare_v2(db,
                "SELECT COALESCE(SUM(value),0) FROM wallet_sapling_notes"
                " WHERE spent_txid IS NULL", -1, &s, NULL) == SQLITE_OK && s) {
            if (sqlite3_step(s) == SQLITE_ROW)
                shielded = sqlite3_column_int64(s, 0);
            sqlite3_finalize(s);
        }

        /* Address */
        char address[128] = "";
        if (sqlite3_prepare_v2(db,
                "SELECT pubkey_hash FROM wallet_keys LIMIT 1",
                -1, &s, NULL) == SQLITE_OK && s) {
            if (sqlite3_step(s) == SQLITE_ROW) {
                const void *pkh = sqlite3_column_blob(s, 0);
                if (pkh && sqlite3_column_bytes(s, 0) == 20) {
                    struct tx_destination dest;
                    dest.type = DEST_KEY_ID;
                    memcpy(dest.id.key.id.data, pkh, 20);
                    const unsigned char pk[] = {0x1C, 0xB8};
                    const unsigned char sc[] = {0x1C, 0xBD};
                    encode_destination(&dest, pk, 2, sc, 2,
                                       address, sizeof(address));
                }
            }
            sqlite3_finalize(s);
        }

        /* Chain height + latest block time */
        int64_t height = 0, block_time = 0;
        if (sqlite3_prepare_v2(db,
                "SELECT COALESCE(MAX(height),0), COALESCE(MAX(time),0)"
                " FROM blocks WHERE status>=3", -1, &s, NULL) == SQLITE_OK && s) {
            if (sqlite3_step(s) == SQLITE_ROW) {
                height = sqlite3_column_int64(s, 0);
                block_time = sqlite3_column_int64(s, 1);
            }
            sqlite3_finalize(s);
        }

        /* Activity — last 20 wallet UTXOs with timestamps */
        size_t w = 0;
        char *buf = (char *)response;
        w += (size_t)snprintf(buf + w, response_max - w,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n\r\n"
            "{\"transparent\":%lld,\"shielded\":%lld,"
            "\"address\":\"%s\",\"height\":%lld,"
            "\"block_time\":%lld,\"now\":%lld,"
            "\"activity\":[",
            (long long)transparent, (long long)shielded,
            address, (long long)height,
            (long long)block_time, (long long)time(NULL));

        if (sqlite3_prepare_v2(db,
                "SELECT u.value, u.height, COALESCE(b.time,0)"
                " FROM utxos u INNER JOIN wallet_keys w"
                " ON u.address_hash=w.pubkey_hash"
                " LEFT JOIN blocks b ON b.height=u.height"
                " ORDER BY u.height DESC LIMIT 20",
                -1, &s, NULL) == SQLITE_OK && s) {
            bool first = true;
            while (sqlite3_step(s) == SQLITE_ROW && w + 100 < response_max) {
                if (!first) buf[w++] = ',';
                first = false;
                w += (size_t)snprintf(buf + w, response_max - w,
                    "{\"value\":%lld,\"height\":%d,\"time\":%lld}",
                    (long long)sqlite3_column_int64(s, 0),
                    sqlite3_column_int(s, 1),
                    (long long)sqlite3_column_int64(s, 2));
            }
            sqlite3_finalize(s);
        }

        w += (size_t)snprintf(buf + w, response_max - w, "]}");
        return w;
    }

    return json_error(response, response_max, JSON_404_HEADERS,
                      "Unknown API endpoint");
}
