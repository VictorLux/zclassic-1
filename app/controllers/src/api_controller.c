/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * REST API controller — fast JSON API for the block explorer.
 * Serves /api routes. All RPC calls happen in a background thread;
 * HTTPS handler threads only serve from cache (rpc_call crashes
 * when called from HTTPS handler threads). */

#include "controllers/api_controller.h"
#include "controllers/explorer_internal.h"
#include "controllers/explorer_factoids.h"
#include "controllers/file_controller.h"
#include "services/snapshot_sync_service.h"
#include "services/zslp_service.h"
#include "controllers/blockchain_controller.h"
#include "services/node_health_service.h"
#include "chain/mmb.h"
#include "config/boot.h"
#include "config/runtime.h"
#include "views/format_helpers.h"
#include "event/event.h"
#include "net/download.h"
#include "validation/contextual_check_tx.h"
#include "keys/key_io.h"
#include "models/database.h"
#include "models/block.h"
#include "models/file_service.h"
#include "models/onion_announcement.h"
#include "models/peer.h"
#include "models/zslp.h"
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <inttypes.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdatomic.h>
#include <unistd.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <sqlite3.h>

/* ── State ────────────────────────────────────────────────── */

struct api_context {
    struct main_state *main_state;
    struct tx_mempool *mempool;
    struct coins_view_cache *coins_tip;
    struct node_db *node_db;
    const char *datadir;
};

struct api_rpc_backend {
    char user[128];
    char pass[128];
    int port;
};

static struct api_context g_api_ctx = {0};
static struct api_rpc_backend g_api_rpc = {
    .user = "zcluser",
    .pass = "zclpass",
    .port = 8023,
};

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

static _Atomic int g_api_cache_thread_running = 0;
static pthread_mutex_t g_api_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

void api_set_state(struct main_state *ms, struct tx_mempool *mp,
                    struct coins_view_cache *coins_tip,
                    struct node_db *ndb, const char *datadir)
{
    g_api_ctx.main_state = ms;
    g_api_ctx.mempool = mp;
    g_api_ctx.coins_tip = coins_tip;
    g_api_ctx.node_db = ndb;
    g_api_ctx.datadir = datadir;
}

void api_set_rpc_backend(const char *rpc_user, const char *rpc_pass,
                          int rpc_port)
{
    if (rpc_user)
        snprintf(g_api_rpc.user, sizeof(g_api_rpc.user), "%s", rpc_user);
    if (rpc_pass)
        snprintf(g_api_rpc.pass, sizeof(g_api_rpc.pass), "%s", rpc_pass);
    if (rpc_port > 0)
        g_api_rpc.port = rpc_port;
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
    addr.sin_port = htons((uint16_t)g_api_rpc.port);

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
    snprintf(auth_plain, sizeof(auth_plain), "%s:%s",
             g_api_rpc.user, g_api_rpc.pass);
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

#define SECURITY_HEADERS \
    "X-Content-Type-Options: nosniff\r\n" \
    "X-Frame-Options: DENY\r\n" \
    "Strict-Transport-Security: max-age=31536000\r\n"

#define JSON_HEADERS \
    "HTTP/1.1 200 OK\r\n" \
    "Content-Type: application/json; charset=utf-8\r\n" \
    "Access-Control-Allow-Origin: *\r\n" \
    "Access-Control-Allow-Methods: GET, OPTIONS\r\n" \
    "Access-Control-Allow-Headers: Content-Type\r\n" \
    "Cache-Control: public, max-age=10\r\n" \
    SECURITY_HEADERS \
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

    double supply = (double)zcl_total_supply_zatoshi(height) / (double)ZATOSHI_PER_ZCL;

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

    double supply = (double)zcl_total_supply_zatoshi(height) / (double)ZATOSHI_PER_ZCL;

    /* Plain number -- CoinGecko expects just a number */
    return (size_t)snprintf((char *)r, max,
        "%s%.8f", JSON_HEADERS, supply);
}

/* Compute /api/hodl — HODL wave data via gethodlwave RPC */
static size_t compute_hodl(uint8_t *r, size_t max)
{
    char *buf = malloc(262144);
    if (!buf) return json_error(r, max, JSON_500_HEADERS, "Out of memory");

    if (rpc_call("gethodlwave", "[]", buf, 262144) <= 0) {
        free(buf);
        return json_error(r, max, JSON_500_HEADERS, "RPC unavailable");
    }

    /* Extract the "result" JSON object from the RPC response */
    const char *result_start = strstr(buf, "\"result\"");
    if (!result_start) {
        free(buf);
        return json_error(r, max, JSON_500_HEADERS, "No result");
    }
    result_start += 8;
    while (*result_start == ' ' || *result_start == ':') result_start++;

    /* Find the end of the result object (before ,"error") */
    const char *result_end = strstr(result_start, ",\"error\"");
    if (!result_end) result_end = result_start + strlen(result_start);

    size_t result_len = (size_t)(result_end - result_start);
    size_t hdr_len = strlen(JSON_HEADERS);

    if (hdr_len + result_len >= max) {
        free(buf);
        return json_error(r, max, JSON_500_HEADERS, "Response too large");
    }

    memcpy(r, JSON_HEADERS, hdr_len);
    memcpy(r + hdr_len, result_start, result_len);
    free(buf);
    return hdr_len + result_len;
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
            balance_sat, (double)balance_sat / (double)ZATOSHI_PER_ZCL);
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
                            (double)satoshis / (double)ZATOSHI_PER_ZCL, utxo_height);
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

static size_t compute_deep_stats(uint8_t *r, size_t max)
{
    if (!g_api_ctx.datadir)
        return json_error(r, max, JSON_500_HEADERS, "No datadir");

    char dbpath[1024];
    snprintf(dbpath, sizeof(dbpath), "%s/node.db", g_api_ctx.datadir);

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(dbpath, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return json_error(r, max, JSON_500_HEADERS, "Cannot open database");
    }
    sqlite3_busy_timeout(db, 30000);

    int64_t height = sql_query_i64(db, "SELECT MAX(height) FROM blocks");
    int64_t block_count = sql_query_i64(db, "SELECT count(*) FROM blocks");
    int64_t tx_count = sql_query_i64(db, "SELECT count(*) FROM transactions");
    struct explorer_utxo_stats utxo_stats = {0};
    explorer_query_utxo_stats(db, &utxo_stats);
    struct explorer_address_stats address_stats = {0};
    explorer_query_address_stats(db, &address_stats);

    /* Sprout stats */
    struct explorer_privacy_stats privacy_stats = {0};
    explorer_query_privacy_stats(db, &privacy_stats);
    int64_t js_first = sql_query_i64(db, "SELECT MIN(block_height) FROM joinsplits");

    /* Sapling stats */
    int64_t ss_first = sql_query_i64(db, "SELECT MIN(block_height) FROM sapling_spends");

    /* ZSLP stats */
    struct explorer_token_stats token_stats = {0};
    explorer_query_token_stats(db, &token_stats);

    int64_t supply_sat = zcl_total_supply_zatoshi(height);

    /* Integrity: checkpoint count and latest block hash */
    char latest_hash[128] = "";
    sql_query_text(db,
        "SELECT hex(hash) FROM blocks WHERE height = (SELECT MAX(height) FROM blocks)",
        latest_hash, sizeof(latest_hash));

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
        (double)supply_sat / (double)ZATOSHI_PER_ZCL,
        (double)privacy_stats.net_shielded_sat / (double)ZATOSHI_PER_ZCL,
        privacy_stats.joinsplits, js_first,
        privacy_stats.sapling_spends, privacy_stats.sapling_outputs, ss_first,
        utxo_stats.count, utxo_stats.dust_under_0001,
        address_stats.total, address_stats.nonzero,
        token_stats.token_count, token_stats.transfer_count,
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

static _Atomic enum lookup_type g_lookup_type = LOOKUP_NONE;
static char    g_lookup_param[512];
static uint8_t g_lookup_result[262144];
static size_t  g_lookup_result_len = 0;
static _Atomic int g_lookup_thread_running = 0;

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

static bool api_is_printable_ascii(const char *s)
{
    if (!s || !s[0])
        return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (*p < 32 || *p > 126)
            return false;
    }
    return true;
}

static struct node_db *api_node_db(void)
{
    return g_api_ctx.node_db ? g_api_ctx.node_db : app_runtime_node_db();
}

static bool api_parse_zslp_limit(const char *path, size_t *limit_out)
{
    const char *q;
    const char *lp;
    char *end = NULL;
    long value;

    if (!limit_out)
        return false;
    *limit_out = 50;
    q = strchr(path, '?');
    if (!q)
        return true;
    lp = strstr(q, "limit=");
    if (!lp)
        return true;
    value = strtol(lp + 6, &end, 10);
    if (!end || (*end != '\0' && *end != '&') || value <= 0 || value > 64)
        return false;
    *limit_out = (size_t)value;
    return true;
}

static bool api_parse_collection_limit(const char *path,
                                       const char *query_key,
                                       size_t default_limit,
                                       size_t max_limit,
                                       size_t *limit_out)
{
    const char *q;
    const char *lp;
    char *end = NULL;
    long value;
    char needle[64];

    if (!path || !query_key || !limit_out || default_limit == 0 ||
        max_limit == 0 || default_limit > max_limit)
        return false;
    *limit_out = default_limit;
    q = strchr(path, '?');
    if (!q)
        return true;
    snprintf(needle, sizeof(needle), "%s=", query_key);
    lp = strstr(q, needle);
    if (!lp)
        return true;
    value = strtol(lp + strlen(needle), &end, 10);
    if (!end || (*end != '\0' && *end != '&') || value <= 0 ||
        (size_t)value > max_limit)
        return false;
    *limit_out = (size_t)value;
    return true;
}

static size_t api_serve_zslp_tokens(const char *path, uint8_t *response,
                                    size_t response_max)
{
    struct node_db *ndb = api_node_db();
    struct db_zslp_token_info tokens[64];
    size_t limit = 50;
    int count;
    size_t w = 0;

    if (!ndb || !ndb->db)
        return json_error(response, response_max, JSON_500_HEADERS, "No database");
    if (!api_parse_zslp_limit(path, &limit))
        return json_error(response, response_max, JSON_404_HEADERS,
                          "Invalid limit parameter");

    count = db_zslp_token_list(ndb, tokens, limit);
    w += (size_t)snprintf((char *)response + w, response_max - w,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n\r\n"
        "{\"tokens\":[");
    for (int i = 0; i < count && w + 256 < response_max; i++) {
        w += (size_t)snprintf((char *)response + w, response_max - w,
            "%s{\"token_id\":\"%s\",\"ticker\":\"%s\",\"name\":\"%s\","
            "\"decimals\":%d,\"genesis_height\":%d,\"total_minted\":%lld}",
            i > 0 ? "," : "",
            tokens[i].token_id,
            tokens[i].ticker,
            tokens[i].name,
            tokens[i].decimals,
            tokens[i].genesis_height,
            (long long)tokens[i].total_minted);
    }
    w += (size_t)snprintf((char *)response + w, response_max - w, "]}");
    return w < response_max ? w : response_max;
}

static size_t api_serve_zslp_token(const char *token_id, uint8_t *response,
                                   size_t response_max)
{
    struct node_db *ndb = api_node_db();
    struct db_zslp_token_info token;

    if (!ndb || !ndb->db)
        return json_error(response, response_max, JSON_500_HEADERS, "No database");
    if (!api_is_printable_ascii(token_id) ||
        !zslp_service_validate_token_key(token_id))
        return json_error(response, response_max, JSON_404_HEADERS,
                          "Invalid token id");
    if (!db_zslp_token_find(ndb, token_id, &token))
        return json_error(response, response_max, JSON_404_HEADERS,
                          "Token not found");

    return (size_t)snprintf((char *)response, response_max,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n\r\n"
        "{\"token_id\":\"%s\",\"ticker\":\"%s\",\"name\":\"%s\","
        "\"decimals\":%d,\"genesis_height\":%d,\"total_minted\":%lld}",
        token.token_id, token.ticker, token.name, token.decimals,
        token.genesis_height, (long long)token.total_minted);
}

static size_t api_serve_zslp_token_transfers(const char *path,
                                             const char *token_id,
                                             uint8_t *response,
                                             size_t response_max)
{
    struct node_db *ndb = api_node_db();
    struct db_zslp_transfer_info transfers[64];
    size_t limit = 50;
    int count;
    size_t w = 0;

    if (!ndb || !ndb->db)
        return json_error(response, response_max, JSON_500_HEADERS, "No database");
    if (!api_is_printable_ascii(token_id) ||
        !zslp_service_validate_token_key(token_id))
        return json_error(response, response_max, JSON_404_HEADERS,
                          "Invalid token id");
    if (!api_parse_zslp_limit(path, &limit))
        return json_error(response, response_max, JSON_404_HEADERS,
                          "Invalid limit parameter");

    count = db_zslp_transfer_list_by_token(ndb, token_id, transfers, limit);
    w += (size_t)snprintf((char *)response + w, response_max - w,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n\r\n"
        "{\"token_id\":\"%s\",\"transfers\":[",
        token_id);
    for (int i = 0; i < count && w + 256 < response_max; i++) {
        w += (size_t)snprintf((char *)response + w, response_max - w,
            "%s{\"txid\":\"%s\",\"token_id\":\"%s\",\"block_height\":%d,"
            "\"tx_type\":%d,\"amount\":%lld,\"vout\":%d%s%s%s}",
            i > 0 ? "," : "",
            transfers[i].txid,
            transfers[i].token_id,
            transfers[i].block_height,
            transfers[i].tx_type,
            (long long)transfers[i].amount,
            transfers[i].vout,
            transfers[i].to_addr_hex[0] ? ",\"to_addr_hex\":\"" : "",
            transfers[i].to_addr_hex,
            transfers[i].to_addr_hex[0] ? "\"" : "");
    }
    w += (size_t)snprintf((char *)response + w, response_max - w, "]}");
    return w < response_max ? w : response_max;
}

static size_t api_serve_onion_announcements(const char *path,
                                            uint8_t *response,
                                            size_t response_max)
{
    struct node_db *ndb = api_node_db();
    struct db_onion_announcement rows[32];
    size_t limit = 16;
    int count;
    size_t w = 0;

    if (!ndb || !ndb->db)
        return json_error(response, response_max, JSON_500_HEADERS, "No database");
    if (!api_parse_collection_limit(path, "limit", 16, 32, &limit))
        return json_error(response, response_max, JSON_404_HEADERS,
                          "Invalid limit parameter");
    memset(rows, 0, sizeof(rows));
    count = db_onion_announcement_recent(ndb, rows, limit);
    w += (size_t)snprintf((char *)response + w, response_max - w,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n\r\n"
        "{\"announcements\":[");
    for (int i = 0; i < count && w + 256 < response_max; i++) {
        w += (size_t)snprintf((char *)response + w, response_max - w,
            "%s{\"onion_address\":\"%s\",\"announced_at\":%" PRId64
            ",\"script_hex\":\"%s\"}",
            i > 0 ? "," : "",
            rows[i].onion_address,
            rows[i].announced_at,
            rows[i].script_hex);
    }
    w += (size_t)snprintf((char *)response + w, response_max - w, "]}");
    return w < response_max ? w : response_max;
}

static size_t api_serve_file_services(const char *path,
                                      uint8_t *response,
                                      size_t response_max)
{
    struct node_db *ndb = api_node_db();
    struct db_file_service rows[32];
    size_t limit = 16;
    int count;
    size_t w = 0;

    if (!ndb || !ndb->db)
        return json_error(response, response_max, JSON_500_HEADERS, "No database");
    if (!api_parse_collection_limit(path, "limit", 16, 32, &limit))
        return json_error(response, response_max, JSON_404_HEADERS,
                          "Invalid limit parameter");
    memset(rows, 0, sizeof(rows));
    count = db_file_service_recent(ndb, rows, limit);
    w += (size_t)snprintf((char *)response + w, response_max - w,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n\r\n"
        "{\"file_services\":[");
    for (int i = 0; i < count && w + 256 < response_max; i++) {
        char ip_hex[33];
        for (int j = 0; j < 16; j++)
            snprintf(ip_hex + j * 2, 3, "%02x", rows[i].ip[j]);
        w += (size_t)snprintf((char *)response + w, response_max - w,
            "%s{\"ip\":\"%s\",\"port\":%u,\"p2p_port\":%u,"
            "\"last_seen\":%" PRId64 ",\"is_zcl23\":%s}",
            i > 0 ? "," : "",
            ip_hex,
            (unsigned)rows[i].port,
            (unsigned)rows[i].p2p_port,
            rows[i].last_seen,
            rows[i].is_zcl23 ? "true" : "false");
    }
    w += (size_t)snprintf((char *)response + w, response_max - w, "]}");
    return w < response_max ? w : response_max;
}

static size_t api_serve_peers(const char *path,
                              uint8_t *response,
                              size_t response_max)
{
    struct node_db *ndb = api_node_db();
    struct db_peer rows[32];
    size_t limit = 16;
    int count;
    size_t w = 0;

    if (!ndb || !ndb->db)
        return json_error(response, response_max, JSON_500_HEADERS, "No database");
    if (!api_parse_collection_limit(path, "limit", 16, 32, &limit))
        return json_error(response, response_max, JSON_404_HEADERS,
                          "Invalid limit parameter");
    memset(rows, 0, sizeof(rows));
    count = db_peer_recent(ndb, rows, limit);
    w += (size_t)snprintf((char *)response + w, response_max - w,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n\r\n"
        "{\"peers\":[");
    for (int i = 0; i < count && w + 320 < response_max; i++) {
        char ip_hex[33];
        char src_hex[33];
        for (int j = 0; j < 16; j++) {
            snprintf(ip_hex + j * 2, 3, "%02x", rows[i].ip[j]);
            snprintf(src_hex + j * 2, 3, "%02x", rows[i].source[j]);
        }
        w += (size_t)snprintf((char *)response + w, response_max - w,
            "%s{\"ip\":\"%s\",\"port\":%u,\"services\":%llu,"
            "\"last_seen\":%" PRId64 ",\"last_try\":%" PRId64
            ",\"attempts\":%d,\"bandwidth_score\":%u,"
            "\"is_zcl23\":%s%s%s%s}",
            i > 0 ? "," : "",
            ip_hex,
            (unsigned)rows[i].port,
            (unsigned long long)rows[i].services,
            rows[i].last_seen,
            rows[i].last_try,
            rows[i].attempts,
            (unsigned)rows[i].bandwidth_score,
            rows[i].is_zcl23 ? "true" : "false",
            rows[i].has_source ? ",\"source\":\"" : "",
            rows[i].has_source ? src_hex : "",
            rows[i].has_source ? "\"" : "");
    }
    w += (size_t)snprintf((char *)response + w, response_max - w, "]}");
    return w < response_max ? w : response_max;
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

    /* Route: /api/zslp/tokens — resource collection */
    if (strcmp(clean_path, "/api/zslp/tokens") == 0 ||
        strncmp(clean_path, "/api/zslp/tokens?", 17) == 0)
        return api_serve_zslp_tokens(path, response, response_max);

    /* Route: /api/zslp/tokens/:id/transfers — member subresource */
    if (strncmp(clean_path, "/api/zslp/tokens/", 17) == 0 && clean_path[17]) {
        const char *token_id = clean_path + 17;
        const char *suffix = strstr(token_id, "/transfers");
        if (suffix &&
            (strcmp(suffix, "/transfers") == 0 ||
             strncmp(suffix, "/transfers?", 11) == 0)) {
            char token_buf[ZSLP_TOKEN_KEY_MAX + 1];
            size_t token_len = (size_t)(suffix - token_id);
            if (token_len == 0 || token_len > ZSLP_TOKEN_KEY_MAX)
                return json_error(response, response_max, JSON_404_HEADERS,
                                  "Invalid token id");
            memcpy(token_buf, token_id, token_len);
            token_buf[token_len] = '\0';
            return api_serve_zslp_token_transfers(path, token_buf,
                                                  response, response_max);
        }
        return api_serve_zslp_token(token_id, response, response_max);
    }

    /* Route: /api/onion/announcements — resource collection */
    if (strcmp(clean_path, "/api/onion/announcements") == 0 ||
        strncmp(clean_path, "/api/onion/announcements?", 25) == 0)
        return api_serve_onion_announcements(path, response, response_max);

    /* Route: /api/file-services — resource collection */
    if (strcmp(clean_path, "/api/file-services") == 0 ||
        strncmp(clean_path, "/api/file-services?", 19) == 0)
        return api_serve_file_services(path, response, response_max);

    /* Route: /api/peers — resource collection */
    if (strcmp(clean_path, "/api/peers") == 0 ||
        strncmp(clean_path, "/api/peers?", 11) == 0)
        return api_serve_peers(path, response, response_max);

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
        if (!g_api_ctx.datadir)
            return json_error(response, response_max, JSON_500_HEADERS, "No datadir");
        return explorer_factoids_build_json(response, response_max, g_api_ctx.datadir);
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
        if (w > 524288) w = 524288;  /* cap to buf size */
        if (off + w <= response_max)
            memcpy(response + off, buf, w);
        else if (off < response_max) {
            size_t avail = response_max - off;
            if (avail > w) avail = w;
            memcpy(response + off, buf, avail);
        }
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
            "{\"sync_state\":\"%s\","
            "\"utxo_replay_active\":%s,"
            "\"utxo_replay_height\":%d}",
            sync_state_name(sync_get_state()),
            atomic_load(&g_utxo_replay_active) ? "true" : "false",
            atomic_load(&g_utxo_replay_height));
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
        struct node_health_snapshot health;
        node_health_collect(&health, g_api_ctx.node_db ?
            g_api_ctx.node_db : app_runtime_node_db(),
            g_api_ctx.main_state);

        char body[4096];
        snprintf(body, sizeof(body),
            "{"
            "\"healthy\":%s,"
            "\"sync_state\":\"%s\","
            "\"chain\":{"
              "\"tip_height\":%d,"
              "\"header_height\":%d,"
              "\"peer_best_height\":%d,"
              "\"tip_lag\":%d"
            "},"
            "\"database\":{"
              "\"wal_size_bytes\":%lld,"
              "\"utxo_count\":%lld"
            "},"
            "\"network\":{"
              "\"peer_count\":%zu,"
              "\"has_peers\":%s,"
              "\"tip_stale\":%s,"
              "\"tip_stale_seconds\":%lld"
            "},"
            "\"services\":{"
              "\"tor_ready\":%s,"
              "\"onion_service_ready\":%s,"
              "\"onion_address\":%s%s%s"
            "},"
            "\"download\":{"
              "\"requested\":%llu,"
              "\"received\":%llu,"
              "\"timed_out\":%llu,"
              "\"in_flight\":%llu,"
              "\"queued\":%llu,"
              "\"queue_backed_up\":%s"
            "},"
            "\"boot\":{\"uptime_seconds\":%lld},"
            "\"errors\":{"
              "\"total\":%d,"
              "\"last\":%s%s%s"
            "},"
            "\"status\":{"
              "\"degraded_reason\":%s%s%s"
            "}"
            "}",
            health.healthy ? "true" : "false",
            sync_state_name(health.sync_state),
            health.tip_height,
            health.header_height,
            health.peer_best_height,
            health.tip_lag,
            (long long)health.wal_size_bytes,
            (long long)health.utxo_count,
            health.peer_count,
            health.has_peers ? "true" : "false",
            health.tip_stale ? "true" : "false",
            (long long)health.tip_stale_seconds,
            health.tor_ready ? "true" : "false",
            health.onion_service_ready ? "true" : "false",
            health.onion_address[0] ? "\"" : "null",
            health.onion_address,
            health.onion_address[0] ? "\"" : "",
            (unsigned long long)health.blocks_requested,
            (unsigned long long)health.blocks_received,
            (unsigned long long)health.blocks_timed_out,
            (unsigned long long)health.in_flight,
            (unsigned long long)health.queued,
            health.queue_backed_up ? "true" : "false",
            (long long)health.uptime_seconds,
            health.error_total,
            health.last_error[0] ? "\"" : "null",
            health.last_error,
            health.last_error[0] ? "\"" : "",
            health.degraded_reason[0] ? "\"" : "null",
            health.degraded_reason,
            health.degraded_reason[0] ? "\"" : "");

        return (size_t)snprintf((char *)response, response_max,
            "HTTP/1.1 %s\r\n"
            "Content-Type: application/json\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n\r\n"
            "%s",
            health.healthy ? "200 OK" : "503 Service Unavailable",
            body);
    }

    /* Route: /api/node/snapshot — snapshot sync service status */
    if (strcmp(clean_path, "/api/node/snapshot") == 0) {
        struct snapshot_sync_service *svc = snapsync_global();
        bool init = snapsync_global_initialized();
        uint64_t received = 0, total = 0;
        double rate = 0;
        if (init) snapsync_get_progress(svc, &received, &total, &rate);
        return (size_t)snprintf((char *)response, response_max,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n\r\n"
            "{\"state\":\"%s\","
            "\"received\":%llu,\"total\":%llu,"
            "\"rate_per_sec\":%.0f,"
            "\"percent\":%.1f,"
            "\"serving_peer\":%u,"
            "\"offered_height\":%d,"
            "\"turbo_active\":%s}",
            init ? snapsync_state_name(svc->state) : "not_initialized",
            (unsigned long long)received, (unsigned long long)total,
            rate,
            total > 0 ? 100.0 * (double)received / (double)total : 0,
            init ? svc->serving_peer_id : 0,
            init ? svc->offered_height : 0,
            (init && svc->turbo_active) ? "true" : "false");
    }

    /* Route: /api/node/mmb — Merkle Mountain Belt status */
    if (strcmp(clean_path, "/api/node/mmb") == 0) {
        struct mmb *mb = rpc_blockchain_get_mmb();
        uint8_t root[32] = {0};
        if (mb && mb->num_leaves > 0) mmb_root(mb, root);
        char hex[65];
        for (int i = 0; i < 32; i++) sprintf(hex + i*2, "%02x", root[i]);
        return (size_t)snprintf((char *)response, response_max,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n\r\n"
            "{\"mmb_root\":\"%s\","
            "\"num_leaves\":%llu,"
            "\"num_peaks\":%u}",
            hex,
            (unsigned long long)(mb ? mb->num_leaves : 0),
            mb ? mb->num_mountains : 0);
    }

    /* Route: /api/node/status — comprehensive diagnostics */
    if (strcmp(clean_path, "/api/node/status") == 0) {
        enum sync_state ss = sync_get_state();
        struct snapshot_sync_service *svc = snapsync_global();
        bool snap_init = snapsync_global_initialized();
        struct mmb *mb = rpc_blockchain_get_mmb();
        struct error_ring *er = error_ring_global();

        char body[2048];
        snprintf(body, sizeof(body),
            "{"
            "\"sync\":{\"state\":\"%s\","
              "\"replay_active\":%s,\"replay_height\":%d},"
            "\"snapshot\":{\"state\":\"%s\","
              "\"received\":%llu,\"total\":%llu},"
            "\"mmb\":{\"leaves\":%llu,\"peaks\":%u},"
            "\"errors\":{\"total\":%d}"
            "}",
            sync_state_name(ss),
            atomic_load(&g_utxo_replay_active) ? "true" : "false",
            atomic_load(&g_utxo_replay_height),
            snap_init ? snapsync_state_name(svc->state) : "not_initialized",
            (unsigned long long)(snap_init ? svc->received_utxos : 0),
            (unsigned long long)(snap_init ? svc->offered_count : 0),
            (unsigned long long)(mb ? mb->num_leaves : 0),
            mb ? mb->num_mountains : 0,
            error_ring_total(er));

        return (size_t)snprintf((char *)response, response_max,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n\r\n%s", body);
    }

    /* Wallet data — balance, address, activity */
    if (strcmp(clean_path, "/api/wallet") == 0) {
        struct node_db *ndb = g_api_ctx.node_db ?
            g_api_ctx.node_db : app_runtime_node_db();
        if (!ndb || !ndb->db) {
            return json_error(response, response_max, JSON_500_HEADERS,
                              "No database");
        }
        sqlite3 *db = ndb->db;
        sqlite3_stmt *s = NULL;

        /* Balance — use wallet_utxos (correct spent tracking) */
        int64_t transparent = 0;
        if (sqlite3_prepare_v2(db,
                "SELECT COALESCE(SUM(value),0) FROM wallet_utxos"
                " WHERE spent_txid IS NULL",
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
                "SELECT wu.value, wu.height, COALESCE(b.time,0)"
                " FROM wallet_utxos wu"
                " LEFT JOIN blocks b ON b.height=wu.height"
                " WHERE wu.spent_txid IS NULL"
                " ORDER BY wu.height DESC LIMIT 20",
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

    /* ── File Transfer Service — SHA3-verified chunks ──────────── */

    /* GET /api/files/manifest — JSON manifest of all chunks */
    if (strcmp(clean_path, "/api/files/manifest") == 0) {
        const struct file_manifest *fm = file_controller_get_manifest();
        if (!fm) {
            /* Try building on demand */
            extern void file_controller_init(const char *);
            if (g_api_ctx.datadir) file_controller_init(g_api_ctx.datadir);
            fm = file_controller_get_manifest();
        }
        if (!fm)
            return json_error(response, response_max, JSON_500_HEADERS,
                              "No block files for manifest");

        /* Build JSON response */
        char *buf = malloc(131072);
        if (!buf)
            return json_error(response, response_max, JSON_500_HEADERS, "OOM");
        size_t w = 0;
        char root_hex[65];
        for (int i = 0; i < 32; i++)
            snprintf(root_hex + i * 2, 3, "%02x", fm->root_hash[i]);

        w += (size_t)snprintf(buf + w, 131072 - w,
            "{\"root_hash\":\"%s\","
            "\"num_chunks\":%u,"
            "\"total_bytes\":%llu,"
            "\"chunks\":[",
            root_hex, fm->num_chunks,
            (unsigned long long)fm->total_bytes);
        for (uint32_t i = 0; i < fm->num_chunks && w + 256 < 131072; i++) {
            char hex[65];
            for (int j = 0; j < 32; j++)
                snprintf(hex + j * 2, 3, "%02x", fm->chunks[i].sha3[j]);
            w += (size_t)snprintf(buf + w, 131072 - w,
                "%s{\"sha3\":\"%s\",\"size\":%u,\"file\":%d,\"offset\":%llu}",
                i > 0 ? "," : "", hex, fm->chunks[i].size,
                fm->chunks[i].file_index,
                (unsigned long long)fm->chunks[i].offset);
        }
        w += (size_t)snprintf(buf + w, 131072 - w, "]}");

        size_t off = (size_t)snprintf((char *)response, response_max,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Cache-Control: public, max-age=300\r\n"
            "Connection: close\r\n"
            "Content-Length: %zu\r\n\r\n", w);
        if (off + w <= response_max)
            memcpy(response + off, buf, w);
        free(buf);
        return off + w < response_max ? off + w : response_max;
    }

    /* GET /api/files/:sha3hash — raw chunk bytes by SHA3 hash */
    if (strncmp(clean_path, "/api/files/", 11) == 0 &&
        strlen(clean_path + 11) == 64) {
        const char *hex = clean_path + 11;
        uint8_t sha3[32];
        for (int i = 0; i < 32; i++) {
            unsigned int byte;
            if (sscanf(hex + i * 2, "%2x", &byte) != 1)
                return json_error(response, response_max,
                    JSON_404_HEADERS, "Invalid SHA3 hash");
            sha3[i] = (uint8_t)byte;
        }
        const struct file_manifest *fm = file_controller_get_manifest();
        if (!fm)
            return json_error(response, response_max,
                JSON_404_HEADERS, "No manifest");
        const struct file_chunk *chunk = file_manifest_find(fm, sha3);
        if (!chunk)
            return json_error(response, response_max,
                JSON_404_HEADERS, "Chunk not found");
        uint8_t *data = NULL;
        uint32_t data_size = 0;
        if (!file_chunk_read(chunk, g_api_ctx.datadir, &data, &data_size))
            return json_error(response, response_max,
                JSON_500_HEADERS, "Failed to read chunk");

        size_t off = (size_t)snprintf((char *)response, response_max,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/octet-stream\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Cache-Control: public, max-age=31536000, immutable\r\n"
            "X-SHA3-256: %s\r\n"
            "Connection: close\r\n"
            "Content-Length: %u\r\n\r\n", hex, data_size);
        if (off + data_size <= response_max)
            memcpy(response + off, data, data_size);
        free(data);
        return off + data_size < response_max ? off + data_size : response_max;
    }

    return json_error(response, response_max, JSON_404_HEADERS,
                      "Unknown API endpoint");
}
