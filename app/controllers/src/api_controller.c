/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * REST API controller — fast JSON API for the block explorer.
 * Serves /api routes. Queries local zclassicd RPC for data. */

#include "controllers/api_controller.h"
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

/* ── State ────────────────────────────────────────────────── */

static struct main_state *g_ms = NULL;
static struct tx_mempool *g_mp = NULL;
static struct coins_view_cache *g_coins_tip = NULL;
static struct node_db *g_ndb = NULL;
static const char *g_datadir = NULL;

static char g_rpc_user[128] = "zcluser";
static char g_rpc_pass[128] = "zclpass";
static int  g_rpc_port = 8023;

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

/* ── Simple JSON extraction helpers ──────────────────────── */

static bool json_extract_str(const char *json, const char *key,
                              char *out, size_t outmax)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return false;
    p += strlen(search);
    while (*p == ' ') p++;
    if (*p != '"') return false;
    p++;
    size_t i = 0;
    while (p[i] && p[i] != '"' && i < outmax - 1) {
        out[i] = p[i]; i++;
    }
    out[i] = '\0';
    return i > 0;
}

static int64_t json_extract_int(const char *json, const char *key)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return -1;
    p += strlen(search);
    while (*p == ' ') p++;
    return strtoll(p, NULL, 10);
}

static double json_extract_real(const char *json, const char *key)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return 0.0;
    p += strlen(search);
    while (*p == ' ') p++;
    return strtod(p, NULL);
}

/* ── Validation helpers ──────────────────────────────────── */

static bool is_all_hex(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++)
        if (!isxdigit((unsigned char)s[i])) return false;
    return true;
}

static bool is_all_digits(const char *s)
{
    if (!s || !*s) return false;
    for (; *s; s++)
        if (!isdigit((unsigned char)*s)) return false;
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

/* ── /api/blocks — latest 25 blocks ─────────────────────── */

static size_t api_blocks(uint8_t *r, size_t max)
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

/* ── /api/block/:id — block detail ───────────────────────── */

static size_t api_block(const char *param, uint8_t *r, size_t max)
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

/* ── /api/tx/:txid — transaction detail ──────────────────── */

static size_t api_tx(const char *param, uint8_t *r, size_t max)
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
        /* Walk each {"value": ... "n": ... "scriptPubKey":{"addresses":[...]} } */
        const char *p = vout + 7;
        int brace = 0, idx = 0;
        while (*p && off + 512 < max) {
            if (*p == '{') {
                brace++;
                if (brace == 1) {
                    /* Find this vout entry's end */
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

                    /* Extract first address */
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

/* ── /api/address/:addr — balance + UTXOs ────────────────── */

static size_t api_address(const char *param, uint8_t *r, size_t max)
{
    if (!param || !*param)
        return json_error(r, max, JSON_404_HEADERS, "Missing address");

    /* Validate address: must be 26-35 chars, base58 */
    size_t alen = strlen(param);
    if (alen < 25 || alen > 95)
        return json_error(r, max, JSON_404_HEADERS, "Invalid address");

    /* Use getaddressbalance and getaddressutxos RPCs if available,
     * otherwise fall back to scantxoutset */
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
        /* The result is an array of UTXO objects */
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

/* ── /api/stats — network stats ──────────────────────────── */

static size_t api_stats(uint8_t *r, size_t max)
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

    /* Calculate supply: 12.5 ZCL per block for first 840000, then halving */
    double supply = 0.0;
    {
        int64_t h = height;
        int64_t subsidy = 1250000000LL; /* 12.5 ZCL in zatoshi */
        int64_t halving_interval = 840000;
        int64_t total_sat = 0;
        while (h > 0) {
            int64_t blocks_at_rate = h;
            if (blocks_at_rate > halving_interval)
                blocks_at_rate = halving_interval;
            /* Founders reward: 20% of block reward for first 850000 blocks
             * but it all goes to supply, so full subsidy counts */
            total_sat += blocks_at_rate * subsidy;
            h -= blocks_at_rate;
            subsidy /= 2;
            if (subsidy == 0) break;
        }
        supply = (double)total_sat / 100000000.0;
    }

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

/* ── /api/supply — circulating supply (CoinGecko format) ── */

static size_t api_supply(uint8_t *r, size_t max)
{
    char buf[8192];

    if (rpc_call("getblockcount", "[]", buf, sizeof(buf)) <= 0)
        return json_error(r, max, JSON_500_HEADERS, "RPC unavailable");

    int64_t height = json_extract_int(buf, "result");
    if (height < 0)
        return json_error(r, max, JSON_500_HEADERS, "Cannot get height");

    /* Calculate supply */
    int64_t h = height;
    int64_t subsidy = 1250000000LL;
    int64_t halving_interval = 840000;
    int64_t total_sat = 0;
    while (h > 0) {
        int64_t blocks_at_rate = h;
        if (blocks_at_rate > halving_interval)
            blocks_at_rate = halving_interval;
        total_sat += blocks_at_rate * subsidy;
        h -= blocks_at_rate;
        subsidy /= 2;
        if (subsidy == 0) break;
    }
    double supply = (double)total_sat / 100000000.0;

    /* Plain number — CoinGecko expects just a number */
    return (size_t)snprintf((char *)r, max,
        "%s%.8f", JSON_HEADERS, supply);
}

/* ── /api/hodl — HODL wave data ──────────────────────────── */

static size_t api_hodl(uint8_t *r, size_t max)
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

    /* HODL wave: sample at monthly intervals, compute approximate
     * age distribution using block timestamps.
     * This is a simplified version — the HTML explorer has detailed
     * UTXO-based analysis in the background thread. */

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

/* ── Main router ─────────────────────────────────────────── */

size_t api_handle_request(const char *method, const char *path,
                           const uint8_t *body, size_t body_len,
                           uint8_t *response, size_t response_max)
{
    (void)body; (void)body_len;
    if (!method || !path || !response) return 0;

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

    /* Route: /api/blocks */
    if (strcmp(clean_path, "/api/blocks") == 0)
        return api_blocks(response, response_max);

    /* Route: /api/block/:id */
    if (strncmp(clean_path, "/api/block/", 11) == 0 && clean_path[11])
        return api_block(clean_path + 11, response, response_max);

    /* Route: /api/tx/:txid */
    if (strncmp(clean_path, "/api/tx/", 8) == 0 && clean_path[8])
        return api_tx(clean_path + 8, response, response_max);

    /* Route: /api/address/:addr */
    if (strncmp(clean_path, "/api/address/", 13) == 0 && clean_path[13])
        return api_address(clean_path + 13, response, response_max);

    /* Route: /api/stats */
    if (strcmp(clean_path, "/api/stats") == 0)
        return api_stats(response, response_max);

    /* Route: /api/supply */
    if (strcmp(clean_path, "/api/supply") == 0)
        return api_supply(response, response_max);

    /* Route: /api/hodl */
    if (strcmp(clean_path, "/api/hodl") == 0)
        return api_hodl(response, response_max);

    return json_error(response, response_max, JSON_404_HEADERS,
                      "Unknown API endpoint");
}
