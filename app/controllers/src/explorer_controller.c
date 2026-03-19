/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Block explorer controller — comprehensive blockchain explorer served
 * over Tor .onion. Supports blocks, transactions (transparent + shielded),
 * ZSLP tokens, and address lookups. */

#include "controllers/explorer_controller.h"
#include "chain/chain.h"
#include "chain/chainparams.h"
#include "chain/subsidy.h"
#include "coins/coins.h"
#include "coins/coins_view.h"
#include "core/uint256.h"
#include "core/serialize.h"
#include "keys/key_io.h"
#include "models/database.h"
#include "models/tx_index.h"
#include "models/utxo.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "sapling/slp.h"
#include "script/standard.h"
#include "storage/disk_block_io.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include "validation/txmempool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <inttypes.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>
#include <math.h>

#include "views/explorer_css.h"

static struct main_state *g_ms = NULL;
static struct tx_mempool *g_mp = NULL;
static struct coins_view_cache *g_coins_tip = NULL;
static struct node_db *g_ndb = NULL;
static const char *g_datadir = NULL;

/* RPC proxy to local zclassicd when our chain is empty */
static char g_rpc_user[128] = "zcluser";
static char g_rpc_pass[128] = "zclpass";
static int g_rpc_proxy_port = 8023;

/* ── Template system ───────────────────────────────────────── */

static char g_explorer_dir[1024] = "";
static char g_css_cache[8192] = "";
static size_t g_css_len = 0;

static void ensure_explorer_dir(void)
{
    if (!g_datadir) return;
    snprintf(g_explorer_dir, sizeof(g_explorer_dir), "%s/explorer", g_datadir);
    mkdir(g_explorer_dir, 0755);
}

static void write_default_file(const char *filename, const char *content)
{
    char path[1200];
    snprintf(path, sizeof(path), "%s/%s", g_explorer_dir, filename);
    /* Only write if file doesn't exist — don't overwrite customizations */
    FILE *f = fopen(path, "r");
    if (f) { fclose(f); return; }
    f = fopen(path, "w");
    if (f) {
        fputs(content, f);
        fclose(f);
        printf("Explorer: wrote default %s\n", path);
    }
}

static void load_css(void)
{
    char path[1200];
    snprintf(path, sizeof(path), "%s/style.css", g_explorer_dir);
    FILE *f = fopen(path, "r");
    if (f) {
        g_css_len = fread(g_css_cache, 1, sizeof(g_css_cache) - 1, f);
        g_css_cache[g_css_len] = '\0';
        fclose(f);
    } else {
        /* Fallback to compiled-in CSS */
        g_css_len = strlen(explorer_css);
        if (g_css_len >= sizeof(g_css_cache)) g_css_len = sizeof(g_css_cache) - 1;
        memcpy(g_css_cache, explorer_css, g_css_len);
        g_css_cache[g_css_len] = '\0';
    }
}

static void init_default_templates(void)
{
    ensure_explorer_dir();
    write_default_file("style.css", explorer_css);
    load_css();
}

/* Forward declarations for cache warming */
static void *stats_compute_thread(void *arg);
static void *hodl_compute_thread(void *arg);
static volatile int g_stats_computing;
static volatile int g_hodl_computing;

static void prewarm_caches(void)
{
    /* Delay 5 seconds to let RPC server start */
    sleep(5);

    printf("Explorer: pre-warming stats cache...\n");
    fflush(stdout);
    if (!g_stats_computing) {
        g_stats_computing = 1;
        pthread_t t;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attr, 2 * 1024 * 1024);
        pthread_create(&t, &attr, stats_compute_thread, NULL);
        pthread_attr_destroy(&attr);
    }

    printf("Explorer: pre-warming HODL wave cache...\n");
    fflush(stdout);
    if (!g_hodl_computing) {
        g_hodl_computing = 1;
        pthread_t t;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attr, 2 * 1024 * 1024);
        pthread_create(&t, &attr, hodl_compute_thread, NULL);
        pthread_attr_destroy(&attr);
    }
}

static void *prewarm_thread(void *arg)
{
    (void)arg;
    prewarm_caches();
    return NULL;
}

void explorer_set_state(struct main_state *ms, struct tx_mempool *mp,
                         struct coins_view_cache *coins_tip,
                         struct node_db *ndb, const char *datadir)
{
    g_ms = ms;
    g_mp = mp;
    g_coins_tip = coins_tip;
    g_ndb = ndb;
    g_datadir = datadir;
    init_default_templates();

    /* Pre-warm caches in background after startup */
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &attr, prewarm_thread, NULL);
    pthread_attr_destroy(&attr);
}

/* ── RPC proxy to local zclassicd ─────────────────────────── */

static int rpc_call(const char *method, const char *params_json,
                     char *out, size_t outmax)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)g_rpc_proxy_port);

    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
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

    /* Base64 encode auth (simple inline for user:pass) */
    char auth_plain[256];
    snprintf(auth_plain, sizeof(auth_plain), "%s:%s", g_rpc_user, g_rpc_pass);
    /* Simple base64 */
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
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

    /* Read response */
    size_t total = 0;
    while (total < outmax - 1) {
        ssize_t r = read(fd, out + total, outmax - 1 - total);
        if (r <= 0) break;
        total += (size_t)r;
    }
    out[total] = '\0';
    close(fd);

    /* Skip HTTP headers — find \r\n\r\n */
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

/* Extract a JSON string value for a key (simple parser) */
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

static int native_chain_height(void)
{
    if (!g_ms) return -1;
    return active_chain_height(&g_ms->chain_active);
}

static bool use_rpc_proxy(void)
{
    return native_chain_height() < 1;
}

/* ── Helpers ──────────────────────────────────────────────── */

static size_t html_escape(char *dst, size_t max, const char *src)
{
    size_t w = 0;
    for (size_t i = 0; src[i] && w + 6 < max; i++) {
        switch (src[i]) {
        case '<':  w += (size_t)snprintf(dst + w, max - w, "&lt;"); break;
        case '>':  w += (size_t)snprintf(dst + w, max - w, "&gt;"); break;
        case '&':  w += (size_t)snprintf(dst + w, max - w, "&amp;"); break;
        case '"':  w += (size_t)snprintf(dst + w, max - w, "&quot;"); break;
        case '\'': w += (size_t)snprintf(dst + w, max - w, "&#39;"); break;
        default:   dst[w++] = src[i]; break;
        }
    }
    dst[w] = '\0';
    return w;
}

static double get_difficulty(const struct block_index *bi)
{
    if (!bi) return 1.0;
    int shift = (int)((bi->nBits >> 24) & 0xff) - 29;
    double diff = (double)0x0000ffff / (double)(bi->nBits & 0x00ffffff);
    while (shift < 0) { diff *= 256.0; shift++; }
    while (shift > 0) { diff /= 256.0; shift--; }
    return diff;
}

static void format_time(char *buf, size_t max, uint32_t t)
{
    time_t ts = (time_t)t;
    struct tm tm;
    gmtime_r(&ts, &tm);
    strftime(buf, max, "%Y-%m-%d %H:%M:%S UTC", &tm);
}

static void format_time_ago(char *buf, size_t max, uint32_t t)
{
    time_t now = time(NULL);
    int64_t diff = (int64_t)now - (int64_t)t;
    if (diff < 0) diff = 0;
    if (diff < 60)
        snprintf(buf, max, "%"PRId64"s ago", diff);
    else if (diff < 3600)
        snprintf(buf, max, "%"PRId64"m ago", diff / 60);
    else if (diff < 86400)
        snprintf(buf, max, "%"PRId64"h ago", diff / 3600);
    else
        snprintf(buf, max, "%"PRId64"d ago", diff / 86400);
}

static void format_zcl(char *buf, size_t max, int64_t zatoshi)
{
    int64_t whole, frac;
    if (zatoshi < 0) {
        whole = (-zatoshi) / 100000000LL;
        frac = (-zatoshi) % 100000000LL;
        snprintf(buf, max, "-%" PRId64 ".%08" PRId64, whole, frac);
    } else {
        whole = zatoshi / 100000000LL;
        frac = zatoshi % 100000000LL;
        snprintf(buf, max, "%" PRId64 ".%08" PRId64, whole, frac);
    }
}

static bool is_all_hex(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++)
        if (!isxdigit((unsigned char)s[i])) return false;
    return true;
}

static bool is_all_digits(const char *s)
{
    if (!s[0]) return false;
    for (size_t i = 0; s[i]; i++)
        if (!isdigit((unsigned char)s[i])) return false;
    return true;
}

/* Encode a tx_destination to a t-address string */
static bool addr_encode(char *out, size_t outmax,
                         const struct tx_destination *dest)
{
    const struct chain_params *cp = chain_params_get();
    if (!cp) return false;
    size_t pk_len = 0, sh_len = 0;
    const unsigned char *pk_pfx = chain_params_base58_prefix(cp, B58_PUBKEY_ADDRESS, &pk_len);
    const unsigned char *sh_pfx = chain_params_base58_prefix(cp, B58_SCRIPT_ADDRESS, &sh_len);
    return encode_destination(dest, pk_pfx, pk_len, sh_pfx, sh_len, out, outmax);
}

static bool addr_decode(const char *str, struct tx_destination *dest)
{
    const struct chain_params *cp = chain_params_get();
    if (!cp) return false;
    size_t pk_len = 0, sh_len = 0;
    const unsigned char *pk_pfx = chain_params_base58_prefix(cp, B58_PUBKEY_ADDRESS, &pk_len);
    const unsigned char *sh_pfx = chain_params_base58_prefix(cp, B58_SCRIPT_ADDRESS, &sh_len);
    return decode_destination(str, pk_pfx, pk_len, sh_pfx, sh_len, dest);
}

/* ── CSS ──────────────────────────────────────────────────── */

#define EXPLORER_HEADER(title) \
    "HTTP/1.1 200 OK\r\n" \
    "Content-Type: text/html; charset=utf-8\r\n" \
    "Connection: close\r\n\r\n" \
    "<!DOCTYPE html><html><head><meta charset='utf-8'>" \
    "<meta name='viewport' content='width=device-width,initial-scale=1'>" \
    "<title>" title "</title>" \
    "<link rel='icon' type='image/png' href='/explorer/favicon.png'>" \
    "<link rel='stylesheet' href='/explorer/style.css'>" \
    "</head><body>"

#define EXPLORER_NAV \
    "<div class='nav'>" \
    "<a href='/explorer'>Blocks</a>" \
    "<a href='/explorer/stats'>Stats</a>" \
    "<a href='/explorer/hodl'>HODL Wave</a>" \
    "<a href='/explorer/tokens'>Tokens</a>" \
    "<div class='search'>" \
    "<form action='/explorer/search' method='get'>" \
    "<input name='q' placeholder='Search block, tx, or address...'>" \
    "</form></div></div>"

#define EXPLORER_FOOTER \
    "<footer>ZClassic23 Block Explorer &mdash; Pure C23 &mdash; zclnet.net</footer>" \
    "</body></html>"

/* Append helper: returns new offset, checks bounds */
#define APPEND(off, buf, max, ...) do { \
    int _n = snprintf((char *)(buf) + (off), (max) - (off), __VA_ARGS__); \
    if (_n > 0) (off) += (size_t)_n; \
} while(0)

/* ── Dashboard (RPC proxy mode) ───────────────────────────── */

static size_t serve_dashboard_rpc(uint8_t *r, size_t max)
{
    size_t off = 0;
    char buf[65536];

    /* Get blockchain info */
    rpc_call("getblockchaininfo", "[]", buf, sizeof(buf));
    int tip = (int)json_extract_int(buf, "blocks");
    double diff = json_extract_real(buf, "difficulty");

    /* Get mempool info */
    rpc_call("getmempoolinfo", "[]", buf, sizeof(buf));
    int64_t mp_count = json_extract_int(buf, "size");
    int64_t mp_bytes = json_extract_int(buf, "bytes");

    APPEND(off, r, max, EXPLORER_HEADER("Dashboard") EXPLORER_NAV);

    APPEND(off, r, max,
        "<div class='stats-row'>"
        "<div class='stat'><div class='num'>%d</div><div class='lbl'>Block Height</div></div>"
        "<div class='stat'><div class='num'>%.2f</div><div class='lbl'>Difficulty</div></div>"
        "<div class='stat'><div class='num'>%" PRId64 "</div><div class='lbl'>Mempool Txs</div></div>"
        "<div class='stat'><div class='num'>%.1f KB</div><div class='lbl'>Mempool Size</div></div>"
        "</div>",
        tip, diff, mp_count, (double)mp_bytes / 1024.0);

    /* Latest blocks */
    APPEND(off, r, max,
        "<h2>Latest Blocks</h2>"
        "<table><tr><th>Height</th><th>Hash</th><th>Time</th>"
        "<th>Txs</th><th>Difficulty</th></tr>");

    int show = 25;
    for (int h = tip; h > tip - show && h >= 0 && off + 600 < max; h--) {
        char params[64];
        snprintf(params, sizeof(params), "[%d]", h);
        rpc_call("getblockhash", params, buf, sizeof(buf));

        /* Extract hash from {"result":"<hash>",...} */
        char hash[65] = "";
        json_extract_str(buf, "result", hash, sizeof(hash));
        if (!hash[0]) continue;

        /* Get block details */
        char params2[128];
        snprintf(params2, sizeof(params2), "[\"%s\"]", hash);
        rpc_call("getblock", params2, buf, sizeof(buf));

        int64_t blk_time = json_extract_int(buf, "time");
        int64_t ntx = json_extract_int(buf, "tx");  /* this is actually array, use size */
        double blk_diff = json_extract_real(buf, "difficulty");

        /* Count txs by counting "tx":[ array elements */
        const char *txarr = strstr(buf, "\"tx\":[");
        int tx_count = 0;
        if (txarr) {
            const char *p = txarr;
            while ((p = strstr(p + 1, "\"")) != NULL && *p) {
                /* Count quoted strings in the tx array */
                tx_count++;
                p = strchr(p + 1, '"');
                if (!p) break;
                if (*(p + 1) == ']' || *(p + 1) == ',') continue;
                break;
            }
            tx_count /= 1; /* each tx has open+close quote */
        }
        /* Simpler: just count commas + 1 */
        if (txarr) {
            const char *end = strchr(txarr, ']');
            tx_count = 1;
            for (const char *p = txarr; p && p < end; p++)
                if (*p == ',') tx_count++;
        }
        (void)ntx;

        char ts[32];
        format_time(ts, sizeof(ts), (uint32_t)blk_time);

        char short_hash[18];
        snprintf(short_hash, sizeof(short_hash), "%.8s...%.4s", hash, hash + 60);

        char ago[32];
        format_time_ago(ago, sizeof(ago), (uint32_t)blk_time);

        APPEND(off, r, max,
            "<tr><td><a href='/explorer/block/%d'><b>%d</b></a></td>"
            "<td class='hash'><a href='/explorer/block/%s'>%s</a></td>"
            "<td>%s<br><small style='color:#666'>%s</small></td>"
            "<td>%d</td><td>%.2f</td></tr>",
            h, h, hash, short_hash, ago, ts, tx_count, blk_diff);
    }

    APPEND(off, r, max, "</table>" EXPLORER_FOOTER);
    return off;
}

/* ── Dashboard (native chain mode) ───────────────────────── */

static size_t serve_dashboard_native(uint8_t *r, size_t max)
{
    size_t off = 0;

    int tip = active_chain_height(&g_ms->chain_active);
    const struct block_index *tip_bi = active_chain_tip(&g_ms->chain_active);

    APPEND(off, r, max, EXPLORER_HEADER("Dashboard") EXPLORER_NAV);

    size_t mp_count = g_mp ? tx_mempool_size(g_mp) : 0;
    uint64_t mp_bytes = g_mp ? tx_mempool_total_size(g_mp) : 0;

    APPEND(off, r, max,
        "<div class='stats-row'>"
        "<div class='stat'><div class='num'>%d</div><div class='lbl'>Block Height</div></div>"
        "<div class='stat'><div class='num'>%.2f</div><div class='lbl'>Difficulty</div></div>"
        "<div class='stat'><div class='num'>%zu</div><div class='lbl'>Mempool Txs</div></div>"
        "<div class='stat'><div class='num'>%.1f KB</div><div class='lbl'>Mempool Size</div></div>"
        "</div>",
        tip, get_difficulty(tip_bi), mp_count, (double)mp_bytes / 1024.0);

    APPEND(off, r, max,
        "<h2>Latest Blocks</h2>"
        "<table><tr><th>Height</th><th>Hash</th><th>Time</th>"
        "<th>Txs</th><th>Difficulty</th><th>Shielded</th></tr>");

    int show = 25;
    if (show > tip + 1) show = tip + 1;
    for (int h = tip; h > tip - show && h >= 0; h--) {
        const struct block_index *bi = active_chain_at(&g_ms->chain_active, h);
        if (!bi) continue;

        char hash[65] = "";
        if (bi->phashBlock) uint256_get_hex(bi->phashBlock, hash);
        char ts[32];
        format_time(ts, sizeof(ts), bi->nTime);
        char short_hash[18];
        snprintf(short_hash, sizeof(short_hash), "%.8s...%.4s", hash, hash + 60);
        char sap_val[32] = "";
        if (bi->nSaplingValue != 0)
            format_zcl(sap_val, sizeof(sap_val), bi->nSaplingValue);

        APPEND(off, r, max,
            "<tr><td><a href='/explorer/block/%d'>%d</a></td>"
            "<td class='hash'><a href='/explorer/block/%s'>%s</a></td>"
            "<td>%s</td><td>%u</td><td>%.4f</td><td class='amount'>%s</td></tr>",
            h, h, hash, short_hash, ts, bi->nTx, get_difficulty(bi), sap_val);

        if (off + 512 >= max) break;
    }

    APPEND(off, r, max, "</table>" EXPLORER_FOOTER);
    return off;
}

static size_t serve_dashboard(uint8_t *r, size_t max)
{
    if (use_rpc_proxy())
        return serve_dashboard_rpc(r, max);
    return serve_dashboard_native(r, max);
}

/* ── Block Detail (RPC proxy) ─────────────────────────────── */

static size_t serve_block_rpc(const char *param, uint8_t *r, size_t max)
{
    if (!param || !param[0]) return 0;
    size_t off = 0;
    char buf[262144]; /* 256KB for block JSON */

    /* Get block hash */
    char hash[65] = "";
    if (is_all_digits(param)) {
        char params[64];
        snprintf(params, sizeof(params), "[%s]", param);
        rpc_call("getblockhash", params, buf, sizeof(buf));
        json_extract_str(buf, "result", hash, sizeof(hash));
    } else if (strlen(param) == 64 && is_all_hex(param, 64)) {
        snprintf(hash, sizeof(hash), "%s", param);
    }

    if (!hash[0]) {
        return (size_t)snprintf((char *)r, max,
            "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
            "<!DOCTYPE html><html><head><link rel='stylesheet' href='/explorer/style.css'></head><body>"
            EXPLORER_NAV "<h2>Block Not Found</h2>" EXPLORER_FOOTER);
    }

    /* Get full block */
    char params2[128];
    snprintf(params2, sizeof(params2), "[\"%s\", true]", hash);
    rpc_call("getblock", params2, buf, sizeof(buf));

    int height = (int)json_extract_int(buf, "height");
    int64_t blk_time = json_extract_int(buf, "time");
    double blk_diff = json_extract_real(buf, "difficulty");
    (void)json_extract_int(buf, "size");

    char merkle[65] = "", prev[65] = "", next_hash[65] = "";
    json_extract_str(buf, "merkleroot", merkle, sizeof(merkle));
    json_extract_str(buf, "previousblockhash", prev, sizeof(prev));
    json_extract_str(buf, "nextblockhash", next_hash, sizeof(next_hash));

    char ts[32];
    format_time(ts, sizeof(ts), (uint32_t)blk_time);

    /* Count txs */
    int tx_count = 0;
    const char *txarr = strstr(buf, "\"tx\":[");
    if (txarr) {
        const char *end = strchr(txarr, ']');
        tx_count = 1;
        if (end) for (const char *p = txarr; p < end; p++)
            if (*p == ',') tx_count++;
    }

    APPEND(off, r, max, EXPLORER_HEADER("Block") EXPLORER_NAV);

    /* Pager */
    APPEND(off, r, max, "<div class='pager'>");
    if (height > 0)
        APPEND(off, r, max, "<a href='/explorer/block/%d'>&laquo; Block %d</a>", height - 1, height - 1);
    if (next_hash[0])
        APPEND(off, r, max, "<a href='/explorer/block/%d'>Block %d &raquo;</a>", height + 1, height + 1);
    APPEND(off, r, max, "</div>");

    APPEND(off, r, max,
        "<h2>Block %d</h2>"
        "<div class='card'><div class='grid'>"
        "<div class='label'>Hash</div><div class='val hash'>%s</div>"
        "<div class='label'>Height</div><div class='val'>%d</div>"
        "<div class='label'>Time</div><div class='val'>%s</div>"
        "<div class='label'>Transactions</div><div class='val'>%d</div>"
        "<div class='label'>Difficulty</div><div class='val'>%.6f</div>"
        "<div class='label'>Merkle Root</div><div class='val mono'>%s</div>",
        height, hash, height, ts, tx_count, blk_diff, merkle);
    if (prev[0])
        APPEND(off, r, max,
            "<div class='label'>Prev Block</div><div class='val hash'>"
            "<a href='/explorer/block/%s'>%s</a></div>", prev, prev);
    APPEND(off, r, max, "</div></div>");

    /* Transaction list */
    if (txarr) {
        APPEND(off, r, max,
            "<h2>Transactions (%d)</h2>"
            "<table><tr><th>#</th><th>TxID</th></tr>", tx_count);

        const char *p = txarr + 6; /* skip "tx":[ */
        int idx = 0;
        while (p && idx < 100 && off + 256 < max) {
            if (*p == '"') {
                p++;
                const char *end = strchr(p, '"');
                if (!end) break;
                char txid[65];
                size_t tlen = (size_t)(end - p);
                if (tlen > 64) tlen = 64;
                memcpy(txid, p, tlen);
                txid[tlen] = '\0';

                char short_txid[18];
                if (tlen >= 64)
                    snprintf(short_txid, sizeof(short_txid), "%.8s...%.4s", txid, txid + 60);
                else
                    snprintf(short_txid, sizeof(short_txid), "%s", txid);

                APPEND(off, r, max,
                    "<tr><td>%d</td><td class='hash'><a href='/explorer/tx/%s'>%s</a></td></tr>",
                    idx, txid, short_txid);
                idx++;
                p = end + 1;
            } else if (*p == ']') {
                break;
            } else {
                p++;
            }
        }
        if (tx_count > 100)
            APPEND(off, r, max,
                "<tr><td colspan='2' style='color:#666;text-align:center'>"
                "...and %d more transactions</td></tr>", tx_count - 100);
        APPEND(off, r, max, "</table>");
    }

    APPEND(off, r, max, EXPLORER_FOOTER);
    return off;
}

/* ── Block Detail (native) ────────────────────────────────── */

static size_t serve_block(const char *param, uint8_t *r, size_t max)
{
    if (use_rpc_proxy())
        return serve_block_rpc(param, r, max);
    if (!g_ms || !param || !param[0]) return 0;
    size_t off = 0;

    const struct block_index *bi = NULL;

    if (is_all_digits(param)) {
        int h = atoi(param);
        int tip = active_chain_height(&g_ms->chain_active);
        if (h >= 0 && h <= tip)
            bi = active_chain_at(&g_ms->chain_active, h);
    } else if (strlen(param) == 64 && is_all_hex(param, 64)) {
        struct uint256 hash;
        uint256_set_hex(&hash, param);
        bi = (const struct block_index *)block_map_find(
            &g_ms->map_block_index, &hash);
    }

    if (!bi) {
        return (size_t)snprintf((char *)r, max,
            "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
            "<!DOCTYPE html><html><head><link rel='stylesheet' href='/explorer/style.css'></head><body>"
            EXPLORER_NAV "<h2>Block Not Found</h2>"
            "<p>No block found for: <code>%s</code></p>"
            EXPLORER_FOOTER, param);
    }

    int height = bi->nHeight;
    int tip = active_chain_height(&g_ms->chain_active);

    char hash[65] = "";
    if (bi->phashBlock) uint256_get_hex(bi->phashBlock, hash);
    char merkle[65], sapling_root[65], nonce[65];
    uint256_get_hex(&bi->hashMerkleRoot, merkle);
    uint256_get_hex(&bi->hashFinalSaplingRoot, sapling_root);
    uint256_get_hex(&bi->nNonce, nonce);

    char ts[32];
    format_time(ts, sizeof(ts), bi->nTime);
    char sap_val[32] = "0";
    format_zcl(sap_val, sizeof(sap_val), bi->nSaplingValue);
    char sprout_val[32] = "0";
    format_zcl(sprout_val, sizeof(sprout_val), bi->nSproutValue);

    APPEND(off, r, max, EXPLORER_HEADER("Block") EXPLORER_NAV);

    /* Navigation */
    APPEND(off, r, max, "<div class='pager'>");
    if (height > 0)
        APPEND(off, r, max, "<a href='/explorer/block/%d'>&laquo; Block %d</a>", height - 1, height - 1);
    if (height < tip)
        APPEND(off, r, max, "<a href='/explorer/block/%d'>Block %d &raquo;</a>", height + 1, height + 1);
    APPEND(off, r, max, "</div>");

    APPEND(off, r, max,
        "<h2>Block %d</h2>"
        "<div class='card'><div class='grid'>"
        "<div class='label'>Hash</div><div class='val hash'>%s</div>"
        "<div class='label'>Height</div><div class='val'>%d</div>"
        "<div class='label'>Confirmations</div><div class='val'>%d</div>"
        "<div class='label'>Time</div><div class='val'>%s</div>"
        "<div class='label'>Transactions</div><div class='val'>%u</div>"
        "<div class='label'>Difficulty</div><div class='val'>%.6f</div>"
        "<div class='label'>Merkle Root</div><div class='val mono'>%s</div>"
        "<div class='label'>Sapling Root</div><div class='val mono'>%s</div>"
        "<div class='label'>Nonce</div><div class='val mono'>%s</div>"
        "<div class='label'>Bits</div><div class='val'>0x%08x</div>"
        "<div class='label'>Sapling &Delta;</div><div class='val amount'>%s ZCL</div>"
        "<div class='label'>Sprout &Delta;</div><div class='val amount'>%s ZCL</div>"
        "</div></div>",
        height, hash, height, tip - height + 1, ts, bi->nTx,
        get_difficulty(bi), merkle, sapling_root, nonce,
        bi->nBits, sap_val, sprout_val);

    /* Load block from disk to show transactions */
    struct block blk;
    block_init(&blk);
    bool loaded = g_datadir && read_block_from_disk_index(&blk, bi, g_datadir);

    if (loaded && blk.num_vtx > 0) {
        APPEND(off, r, max,
            "<h2>Transactions (%zu)</h2>"
            "<table><tr><th>#</th><th>TxID</th><th>Type</th>"
            "<th>Inputs</th><th>Outputs</th><th>Value Out</th></tr>",
            blk.num_vtx);

        size_t show_max = blk.num_vtx > 100 ? 100 : blk.num_vtx;
        for (size_t i = 0; i < show_max && off + 512 < max; i++) {
            const struct transaction *tx = &blk.vtx[i];
            char txid[65];
            uint256_get_hex(&tx->hash, txid);
            char short_txid[18];
            snprintf(short_txid, sizeof(short_txid), "%.8s...%.4s", txid, txid + 60);

            char val[32];
            format_zcl(val, sizeof(val), transaction_get_value_out(tx));

            bool is_cb = transaction_is_coinbase(tx);
            bool has_shielded = (tx->num_shielded_spend > 0 || tx->num_shielded_output > 0 ||
                                 tx->num_joinsplit > 0);

            /* Check for ZSLP */
            bool is_slp = false;
            struct slp_message slp;
            if (tx->num_vout > 0 && tx->vout[0].script_pub_key.size > 0)
                is_slp = slp_parse(tx->vout[0].script_pub_key.data,
                                   tx->vout[0].script_pub_key.size, &slp);

            const char *type_tags = "";
            char tags_buf[256] = "";
            if (is_cb) snprintf(tags_buf, sizeof(tags_buf), "<span class='tag tag-cb'>Coinbase</span> ");
            if (has_shielded) {
                size_t tl = strlen(tags_buf);
                snprintf(tags_buf + tl, sizeof(tags_buf) - tl,
                    "<span class='tag tag-shielded'>Shielded</span> ");
            }
            if (is_slp) {
                size_t tl = strlen(tags_buf);
                snprintf(tags_buf + tl, sizeof(tags_buf) - tl,
                    "<span class='tag tag-slp'>ZSLP: %s</span> ", slp.ticker);
            }
            type_tags = tags_buf;

            APPEND(off, r, max,
                "<tr><td>%zu</td>"
                "<td class='hash'><a href='/explorer/tx/%s'>%s</a></td>"
                "<td>%s</td>"
                "<td>%zu</td><td>%zu</td>"
                "<td class='amount'>%s</td></tr>",
                i, txid, short_txid, type_tags,
                tx->num_vin, tx->num_vout, val);
        }

        if (blk.num_vtx > 100)
            APPEND(off, r, max,
                "<tr><td colspan='6' style='color:#666;text-align:center'>"
                "...and %zu more transactions</td></tr>",
                blk.num_vtx - 100);

        APPEND(off, r, max, "</table>");
    } else if (!loaded) {
        APPEND(off, r, max,
            "<div class='card' style='border-left-color:#ff4444'>"
            "Block data not available on disk.</div>");
    }

    block_free(&blk);
    APPEND(off, r, max, EXPLORER_FOOTER);
    return off;
}

/* ── Transaction Detail (RPC proxy) ───────────────────────── */

static size_t serve_tx_rpc(const char *param, uint8_t *r, size_t max)
{
    if (!param || strlen(param) != 64 || !is_all_hex(param, 64))
        return 0;

    size_t off = 0;
    char buf[262144];

    char params[128];
    snprintf(params, sizeof(params), "[\"%s\", 1]", param);
    int n = rpc_call("getrawtransaction", params, buf, sizeof(buf));
    if (n <= 0 || strstr(buf, "\"error\":null") == NULL) {
        return (size_t)snprintf((char *)r, max,
            "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
            "<!DOCTYPE html><html><head><link rel='stylesheet' href='/explorer/style.css'></head><body>"
            EXPLORER_NAV "<h2>Transaction Not Found</h2>"
            "<p>TxID: <code>%s</code></p>" EXPLORER_FOOTER, param);
    }

    /* Extract the result object — find "result":{ */
    const char *result = strstr(buf, "\"result\":{");
    if (!result) result = buf;

    int64_t confirmations = json_extract_int(result, "confirmations");
    int64_t blk_height = json_extract_int(result, "height");
    int64_t tx_size = json_extract_int(result, "size");
    int64_t version = json_extract_int(result, "version");
    int64_t locktime = json_extract_int(result, "locktime");
    int64_t expiry = json_extract_int(result, "expiryheight");
    double value_balance = json_extract_real(result, "valuebalance");

    char blockhash[65] = "";
    json_extract_str(result, "blockhash", blockhash, sizeof(blockhash));

    APPEND(off, r, max, EXPLORER_HEADER("Transaction") EXPLORER_NAV);

    APPEND(off, r, max,
        "<h2>Transaction</h2>"
        "<div class='card'><div class='grid'>"
        "<div class='label'>TxID</div><div class='val hash'>%s</div>"
        "<div class='label'>Confirmations</div><div class='val'>%" PRId64 "</div>"
        "<div class='label'>Size</div><div class='val'>%" PRId64 " bytes</div>"
        "<div class='label'>Version</div><div class='val'>%" PRId64 "</div>"
        "<div class='label'>Lock Time</div><div class='val'>%" PRId64 "</div>",
        param, confirmations, tx_size, version, locktime);

    if (blockhash[0])
        APPEND(off, r, max,
            "<div class='label'>Block</div><div class='val hash'>"
            "<a href='/explorer/block/%s'>%.16s...</a> (height %" PRId64 ")</div>",
            blockhash, blockhash, blk_height);
    if (expiry > 0)
        APPEND(off, r, max,
            "<div class='label'>Expiry Height</div><div class='val'>%" PRId64 "</div>", expiry);
    if (value_balance != 0.0) {
        char vb[32];
        format_zcl(vb, sizeof(vb), (int64_t)(value_balance * 100000000.0));
        APPEND(off, r, max,
            "<div class='label'>Value Balance</div><div class='val amount'>%s ZCL</div>", vb);
    }

    APPEND(off, r, max, "</div></div>");

    /* Parse vout array for outputs */
    const char *vout = strstr(result, "\"vout\":[");
    if (vout) {
        APPEND(off, r, max, "<h2>Outputs</h2><div class='io-box'>");

        /* Walk through vout entries — look for "n": and "value": and "addresses": */
        const char *p = vout;
        const char *vout_end = NULL;
        int brace_depth = 0;
        for (const char *q = vout + 7; *q; q++) {
            if (*q == '[') brace_depth++;
            if (*q == ']') { brace_depth--; if (brace_depth <= 0) { vout_end = q; break; } }
        }
        if (!vout_end) vout_end = buf + n;

        /* Find each {"value": entry */
        p = vout;
        int out_idx = 0;
        while (p < vout_end && off + 512 < max) {
            const char *val_str = strstr(p, "\"value\":");
            if (!val_str || val_str >= vout_end) break;

            double val = strtod(val_str + 8, NULL);
            char val_fmt[32];
            format_zcl(val_fmt, sizeof(val_fmt), (int64_t)(val * 100000000.0));

            /* Try to find address */
            char addr[64] = "";
            const char *addr_start = strstr(val_str, "\"addresses\":[\"");
            if (addr_start && addr_start < vout_end && addr_start - val_str < 500) {
                addr_start += 14;
                const char *addr_end = strchr(addr_start, '"');
                if (addr_end && (size_t)(addr_end - addr_start) < sizeof(addr)) {
                    memcpy(addr, addr_start, (size_t)(addr_end - addr_start));
                    addr[(size_t)(addr_end - addr_start)] = '\0';
                }
            }

            /* Check for OP_RETURN */
            bool is_opreturn = (strstr(val_str, "\"type\":\"nulldata\"") != NULL &&
                                strstr(val_str, "\"type\":\"nulldata\"") < vout_end &&
                                strstr(val_str, "\"type\":\"nulldata\"") - val_str < 500);

            if (is_opreturn) {
                APPEND(off, r, max,
                    "<div class='io-row'><div class='io-idx'>%d</div>"
                    "<div class='io-addr' style='color:#888'>OP_RETURN</div>"
                    "<div class='io-val'>%s ZCL</div></div>",
                    out_idx, val_fmt);
            } else if (addr[0]) {
                APPEND(off, r, max,
                    "<div class='io-row'><div class='io-idx'>%d</div>"
                    "<div class='io-addr'><a href='/explorer/address/%s'>%s</a></div>"
                    "<div class='io-val'>%s ZCL</div></div>",
                    out_idx, addr, addr, val_fmt);
            } else {
                APPEND(off, r, max,
                    "<div class='io-row'><div class='io-idx'>%d</div>"
                    "<div class='io-addr' style='color:#666'>Unknown</div>"
                    "<div class='io-val'>%s ZCL</div></div>",
                    out_idx, val_fmt);
            }

            out_idx++;
            p = val_str + 8;
        }
        APPEND(off, r, max, "</div>");
    }

    /* Shielded data */
    int64_t vShieldedSpend = 0, vShieldedOutput = 0, vJoinSplit = 0;
    const char *ss = strstr(result, "\"vShieldedSpend\":[");
    if (ss) { for (const char *q = ss; *q && *q != ']'; q++) if (*q == '{') vShieldedSpend++; }
    const char *so = strstr(result, "\"vShieldedOutput\":[");
    if (so) { for (const char *q = so; *q && *q != ']'; q++) if (*q == '{') vShieldedOutput++; }
    const char *js = strstr(result, "\"vjoinsplit\":[");
    if (js) { for (const char *q = js; *q && *q != ']'; q++) if (*q == '{') vJoinSplit++; }

    if (vShieldedSpend > 0 || vShieldedOutput > 0 || vJoinSplit > 0) {
        APPEND(off, r, max, "<h2>Shielded Data</h2><div class='card'><div class='grid'>");
        if (vShieldedSpend > 0)
            APPEND(off, r, max,
                "<div class='label'>Sapling Spends</div><div class='val'>%" PRId64 "</div>",
                vShieldedSpend);
        if (vShieldedOutput > 0)
            APPEND(off, r, max,
                "<div class='label'>Sapling Outputs</div><div class='val'>%" PRId64 "</div>",
                vShieldedOutput);
        if (vJoinSplit > 0)
            APPEND(off, r, max,
                "<div class='label'>JoinSplits</div><div class='val'>%" PRId64 "</div>",
                vJoinSplit);
        APPEND(off, r, max, "</div></div>");
    }

    APPEND(off, r, max, EXPLORER_FOOTER);
    return off;
}

/* ── Transaction Detail (native) ──────────────────────────── */

static size_t serve_tx(const char *param, uint8_t *r, size_t max)
{
    if (use_rpc_proxy())
        return serve_tx_rpc(param, r, max);
    if (!g_ms || !param || strlen(param) != 64 || !is_all_hex(param, 64))
        return (size_t)snprintf((char *)r, max,
            "HTTP/1.1 400 Bad Request\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
            "<!DOCTYPE html><html><head><link rel='stylesheet' href='/explorer/style.css'></head><body>"
            EXPLORER_NAV "<h2>Invalid Transaction ID</h2>"
            "<p>Expected 64 hex characters.</p>" EXPLORER_FOOTER);

    struct uint256 txhash;
    uint256_set_hex(&txhash, param);

    /* Try mempool */
    struct transaction tx;
    memset(&tx, 0, sizeof(tx));
    bool in_mempool = g_mp && tx_mempool_lookup(g_mp, &txhash, &tx);

    /* Try tx index */
    int block_height = -1;
    char block_hash_hex[65] = "";
    struct block blk;
    block_init(&blk);
    bool from_block = false;

    if (!in_mempool && g_ndb) {
        struct db_tx_index txi;
        if (db_tx_find(g_ndb, txhash.data, &txi)) {
            block_height = txi.block_height;

            /* Load block from disk */
            const struct block_index *bi = active_chain_at(&g_ms->chain_active, block_height);
            if (bi && g_datadir && read_block_from_disk_index(&blk, bi, g_datadir)) {
                /* Find the tx in the block */
                for (size_t i = 0; i < blk.num_vtx; i++) {
                    if (uint256_eq(&blk.vtx[i].hash, &txhash)) {
                        transaction_copy(&tx, &blk.vtx[i]);
                        from_block = true;
                        if (bi->phashBlock)
                            uint256_get_hex(bi->phashBlock, block_hash_hex);
                        break;
                    }
                }
            }
        }
    }

    if (!in_mempool && !from_block) {
        block_free(&blk);
        return (size_t)snprintf((char *)r, max,
            "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
            "<!DOCTYPE html><html><head><link rel='stylesheet' href='/explorer/style.css'></head><body>"
            EXPLORER_NAV "<h2>Transaction Not Found</h2>"
            "<p>TxID: <code>%s</code></p>"
            "<p style='color:#666'>Not in mempool or tx index.</p>" EXPLORER_FOOTER, param);
    }

    size_t off = 0;
    int tip = active_chain_height(&g_ms->chain_active);
    int confirmations = in_mempool ? 0 : (block_height >= 0 ? tip - block_height + 1 : 0);

    APPEND(off, r, max, EXPLORER_HEADER("Transaction") EXPLORER_NAV);

    /* Header info */
    char txid_hex[65];
    uint256_get_hex(&tx.hash, txid_hex);

    /* Compute serialized size */
    struct byte_stream bs;
    stream_init(&bs, 512);
    transaction_serialize(&tx, &bs);
    size_t tx_size = bs.size;
    stream_free(&bs);

    APPEND(off, r, max,
        "<h2>Transaction</h2>"
        "<div class='card'><div class='grid'>"
        "<div class='label'>TxID</div><div class='val hash'>%s</div>"
        "<div class='label'>Status</div><div class='val'>%s</div>"
        "<div class='label'>Confirmations</div><div class='val'>%d</div>",
        txid_hex,
        in_mempool ? "<span class='tag tag-mempool'>Mempool</span>" : "Confirmed",
        confirmations);

    if (block_height >= 0)
        APPEND(off, r, max,
            "<div class='label'>Block</div><div class='val'>"
            "<a href='/explorer/block/%d'>%d</a></div>",
            block_height, block_height);

    APPEND(off, r, max,
        "<div class='label'>Version</div><div class='val'>%d%s</div>"
        "<div class='label'>Size</div><div class='val'>%zu bytes</div>"
        "<div class='label'>Lock Time</div><div class='val'>%u</div>",
        tx.version, tx.overwintered ? " (Overwinter)" : "",
        tx_size, tx.lock_time);

    if (tx.overwintered && tx.expiry_height > 0)
        APPEND(off, r, max,
            "<div class='label'>Expiry Height</div><div class='val'>%u</div>",
            tx.expiry_height);

    /* Value balance for Sapling */
    if (tx.overwintered && tx.version >= 4) {
        char vb[32];
        format_zcl(vb, sizeof(vb), tx.value_balance);
        APPEND(off, r, max,
            "<div class='label'>Value Balance</div><div class='val amount'>%s ZCL</div>",
            vb);
    }

    APPEND(off, r, max, "</div></div>");

    /* Inputs */
    APPEND(off, r, max, "<h2>Inputs (%zu)</h2><div class='io-box'>", tx.num_vin);
    for (size_t i = 0; i < tx.num_vin && off + 512 < max; i++) {
        if (transaction_is_coinbase(&tx) && i == 0) {
            char subsidy[32];
            int64_t reward = block_height >= 0 ? get_block_subsidy(block_height, NULL) : 0;
            format_zcl(subsidy, sizeof(subsidy), reward);
            APPEND(off, r, max,
                "<div class='io-row'>"
                "<div class='io-idx'>%zu</div>"
                "<div class='io-addr'><span class='tag tag-cb'>Coinbase</span> "
                "Block reward</div>"
                "<div class='io-val'>%s ZCL</div></div>",
                i, subsidy);
        } else {
            char prev_hash[65];
            uint256_get_hex(&tx.vin[i].prevout.hash, prev_hash);
            char prev_short[18];
            snprintf(prev_short, sizeof(prev_short), "%.8s...%.4s",
                     prev_hash, prev_hash + 60);
            APPEND(off, r, max,
                "<div class='io-row'>"
                "<div class='io-idx'>%zu</div>"
                "<div class='io-addr'><a href='/explorer/tx/%s'>%s</a>:%u</div>"
                "<div class='io-val'></div></div>",
                i, prev_hash, prev_short, tx.vin[i].prevout.n);
        }
    }
    APPEND(off, r, max, "</div>");

    /* Outputs */
    int64_t total_out = 0;
    APPEND(off, r, max, "<h2>Outputs (%zu)</h2><div class='io-box'>", tx.num_vout);
    for (size_t i = 0; i < tx.num_vout && off + 512 < max; i++) {
        char val[32];
        format_zcl(val, sizeof(val), tx.vout[i].value);
        total_out += tx.vout[i].value;

        /* Try to extract destination address */
        char addr_str[64] = "";
        struct tx_destination dest;
        memset(&dest, 0, sizeof(dest));
        if (script_extract_destination(&tx.vout[i].script_pub_key, &dest)) {
            addr_encode(addr_str, sizeof(addr_str), &dest);
        }

        /* Check for OP_RETURN */
        bool is_op_return = (tx.vout[i].script_pub_key.size > 0 &&
                             tx.vout[i].script_pub_key.data[0] == 0x6a); /* OP_RETURN */

        if (is_op_return) {
            APPEND(off, r, max,
                "<div class='io-row'>"
                "<div class='io-idx'>%zu</div>"
                "<div class='io-addr' style='color:#888'>OP_RETURN (%zu bytes)</div>"
                "<div class='io-val'>%s ZCL</div></div>",
                i, tx.vout[i].script_pub_key.size, val);
        } else if (addr_str[0]) {
            APPEND(off, r, max,
                "<div class='io-row'>"
                "<div class='io-idx'>%zu</div>"
                "<div class='io-addr'><a href='/explorer/address/%s'>%s</a></div>"
                "<div class='io-val'>%s ZCL</div></div>",
                i, addr_str, addr_str, val);
        } else {
            APPEND(off, r, max,
                "<div class='io-row'>"
                "<div class='io-idx'>%zu</div>"
                "<div class='io-addr' style='color:#666'>Non-standard script (%zu bytes)</div>"
                "<div class='io-val'>%s ZCL</div></div>",
                i, tx.vout[i].script_pub_key.size, val);
        }
    }
    {
        char tot[32];
        format_zcl(tot, sizeof(tot), total_out);
        APPEND(off, r, max,
            "<div class='io-row' style='font-weight:bold;border-top:1px solid #333'>"
            "<div class='io-idx'></div><div class='io-addr'>Total</div>"
            "<div class='io-val'>%s ZCL</div></div>", tot);
    }
    APPEND(off, r, max, "</div>");

    /* Shielded data */
    if (tx.num_shielded_spend > 0 || tx.num_shielded_output > 0 || tx.num_joinsplit > 0) {
        APPEND(off, r, max, "<h2>Shielded Data</h2><div class='card'><div class='grid'>");
        if (tx.num_shielded_spend > 0)
            APPEND(off, r, max,
                "<div class='label'>Sapling Spends</div><div class='val'>%zu</div>",
                tx.num_shielded_spend);
        if (tx.num_shielded_output > 0)
            APPEND(off, r, max,
                "<div class='label'>Sapling Outputs</div><div class='val'>%zu</div>",
                tx.num_shielded_output);
        if (tx.num_joinsplit > 0) {
            int64_t js_in = 0, js_out = 0;
            for (size_t j = 0; j < tx.num_joinsplit; j++) {
                js_in += tx.v_joinsplit[j].vpub_old;
                js_out += tx.v_joinsplit[j].vpub_new;
            }
            char jsi[32], jso[32];
            format_zcl(jsi, sizeof(jsi), js_in);
            format_zcl(jso, sizeof(jso), js_out);
            APPEND(off, r, max,
                "<div class='label'>JoinSplits</div><div class='val'>%zu</div>"
                "<div class='label'>vpub_old (t&rarr;z)</div><div class='val amount'>%s ZCL</div>"
                "<div class='label'>vpub_new (z&rarr;t)</div><div class='val amount'>%s ZCL</div>",
                tx.num_joinsplit, jsi, jso);
        }
        APPEND(off, r, max, "</div></div>");
    }

    /* ZSLP token data */
    if (tx.num_vout > 0) {
        struct slp_message slp;
        if (slp_parse(tx.vout[0].script_pub_key.data,
                      tx.vout[0].script_pub_key.size, &slp)) {
            APPEND(off, r, max,
                "<h2><span class='tag tag-slp'>ZSLP Token</span></h2>"
                "<div class='card'><div class='grid'>");

            if (slp.type == SLP_TX_GENESIS) {
                char qty[32];
                snprintf(qty, sizeof(qty), "%" PRIu64, slp.initial_quantity);
                char safe_ticker[128], safe_name[256];
                html_escape(safe_ticker, sizeof(safe_ticker), slp.ticker);
                html_escape(safe_name, sizeof(safe_name), slp.name);
                APPEND(off, r, max,
                    "<div class='label'>Type</div><div class='val'>GENESIS</div>"
                    "<div class='label'>Ticker</div><div class='val' style='color:#ff88ff'>%s</div>"
                    "<div class='label'>Name</div><div class='val'>%s</div>"
                    "<div class='label'>Decimals</div><div class='val'>%u</div>"
                    "<div class='label'>Initial Supply</div><div class='val'>%s</div>",
                    safe_ticker, safe_name, slp.decimals, qty);
                if (slp.document_url[0]) {
                    char safe_url[512];
                    html_escape(safe_url, sizeof(safe_url), slp.document_url);
                    APPEND(off, r, max,
                        "<div class='label'>Document URL</div><div class='val'>%s</div>",
                        safe_url);
                }
            } else if (slp.type == SLP_TX_SEND) {
                char token_id_hex[65];
                uint256_get_hex(&slp.token_id, token_id_hex);
                APPEND(off, r, max,
                    "<div class='label'>Type</div><div class='val'>SEND</div>"
                    "<div class='label'>Token ID</div><div class='val hash'>"
                    "<a href='/explorer/tx/%s'>%s</a></div>",
                    token_id_hex, token_id_hex);
                for (int q = 0; q < slp.num_outputs; q++) {
                    char qlbl[32];
                    snprintf(qlbl, sizeof(qlbl), "Output %d", q + 1);
                    APPEND(off, r, max,
                        "<div class='label'>%s</div><div class='val'>%" PRIu64 "</div>",
                        qlbl, slp.output_quantities[q]);
                }
            } else if (slp.type == SLP_TX_MINT) {
                char token_id_hex[65];
                uint256_get_hex(&slp.token_id, token_id_hex);
                char qty[32];
                snprintf(qty, sizeof(qty), "%" PRIu64, slp.additional_quantity);
                APPEND(off, r, max,
                    "<div class='label'>Type</div><div class='val'>MINT</div>"
                    "<div class='label'>Token ID</div><div class='val hash'>"
                    "<a href='/explorer/tx/%s'>%s</a></div>"
                    "<div class='label'>Quantity</div><div class='val'>%s</div>",
                    token_id_hex, token_id_hex, qty);
            }

            APPEND(off, r, max, "</div></div>");
        }
    }

    transaction_free(&tx);
    block_free(&blk);
    APPEND(off, r, max, EXPLORER_FOOTER);
    return off;
}

/* ── Address Page ─────────────────────────────────────────── */

static size_t serve_address(const char *param, uint8_t *r, size_t max)
{
    if (!g_ms || !param || !param[0])
        return 0;

    size_t off = 0;
    char safe_addr[128];
    html_escape(safe_addr, sizeof(safe_addr), param);

    struct tx_destination dest;
    memset(&dest, 0, sizeof(dest));
    if (!addr_decode(param, &dest)) {
        return (size_t)snprintf((char *)r, max,
            "HTTP/1.1 400 Bad Request\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
            "<!DOCTYPE html><html><head><link rel='stylesheet' href='/explorer/style.css'></head><body>"
            EXPLORER_NAV "<h2>Invalid Address</h2>"
            "<p><code>%s</code> is not a valid ZClassic address.</p>"
            EXPLORER_FOOTER, safe_addr);
    }

    /* Get the 20-byte hash */
    const uint8_t *addr_hash = NULL;
    if (dest.type == DEST_KEY_ID)
        addr_hash = dest.id.key.id.data;
    else if (dest.type == DEST_SCRIPT_ID)
        addr_hash = dest.id.script.hash.data;

    APPEND(off, r, max, EXPLORER_HEADER("Address") EXPLORER_NAV);

    APPEND(off, r, max,
        "<h2>Address</h2>"
        "<div class='card'><div class='grid'>"
        "<div class='label'>Address</div><div class='val hash'>%s</div>"
        "<div class='label'>Type</div><div class='val'>%s</div>",
        safe_addr,
        dest.type == DEST_KEY_ID ? "P2PKH (Pay-to-PubKey-Hash)" : "P2SH (Pay-to-Script-Hash)");

    if (g_ndb && addr_hash) {
        int64_t balance = db_utxo_balance_for_address(g_ndb, addr_hash);
        char bal[32];
        format_zcl(bal, sizeof(bal), balance);
        APPEND(off, r, max,
            "<div class='label'>Balance</div><div class='val amount'>%s ZCL</div>",
            bal);
    }
    APPEND(off, r, max, "</div></div>");

    /* UTXO list */
    if (g_ndb && addr_hash) {
        struct db_utxo utxos[100];
        int count = db_utxo_list_for_address(g_ndb, addr_hash, utxos, 100);

        APPEND(off, r, max,
            "<h2>Unspent Outputs (%d)</h2>"
            "<table><tr><th>TxID</th><th>Vout</th><th>Value</th>"
            "<th>Height</th><th>Type</th></tr>", count);

        for (int i = 0; i < count && off + 512 < max; i++) {
            char txid_hex[65];
            struct uint256 utxo_txid;
            memcpy(utxo_txid.data, utxos[i].txid, 32);
            uint256_get_hex(&utxo_txid, txid_hex);
            char short_txid[18];
            snprintf(short_txid, sizeof(short_txid), "%.8s...%.4s",
                     txid_hex, txid_hex + 60);

            char val[32];
            format_zcl(val, sizeof(val), utxos[i].value);

            APPEND(off, r, max,
                "<tr><td class='hash'><a href='/explorer/tx/%s'>%s</a></td>"
                "<td>%u</td><td class='amount'>%s ZCL</td>"
                "<td>%d</td><td>%s</td></tr>",
                txid_hex, short_txid, utxos[i].vout, val,
                utxos[i].height,
                utxos[i].is_coinbase ? "<span class='tag tag-cb'>CB</span>" : "");

            db_utxo_free(&utxos[i]);
        }

        APPEND(off, r, max, "</table>");
        if (count == 0) {
            APPEND(off, r, max,
                "<p style='color:#666'>No unspent outputs found for this address.</p>");
        }
    } else {
        APPEND(off, r, max,
            "<p style='color:#666'>UTXO index not available.</p>");
    }

    APPEND(off, r, max, EXPLORER_FOOTER);
    return off;
}

/* ── Search ───────────────────────────────────────────────── */

static size_t serve_search(const char *query, uint8_t *r, size_t max)
{
    if (!g_ms || !query) return 0;

    /* Strip leading/trailing whitespace */
    while (*query == ' ') query++;
    size_t qlen = strlen(query);
    char q[256];
    if (qlen >= sizeof(q)) qlen = sizeof(q) - 1;
    memcpy(q, query, qlen);
    q[qlen] = '\0';
    while (qlen > 0 && q[qlen - 1] == ' ') q[--qlen] = '\0';

    if (!qlen) return serve_dashboard(r, max);

    int tip = active_chain_height(&g_ms->chain_active);

    /* Block height? */
    if (is_all_digits(q)) {
        int h = atoi(q);
        if (h >= 0 && h <= tip)
            return serve_block(q, r, max);
    }

    /* 64-hex: block hash or txid? */
    if (qlen == 64 && is_all_hex(q, 64)) {
        /* Try block hash */
        struct uint256 hash;
        uint256_set_hex(&hash, q);
        const struct block_index *bi = (const struct block_index *)block_map_find(
            &g_ms->map_block_index, &hash);
        if (bi)
            return serve_block(q, r, max);

        /* Try txid */
        if (g_ndb) {
            struct db_tx_index txi;
            if (db_tx_find(g_ndb, hash.data, &txi))
                return serve_tx(q, r, max);
        }
        if (g_mp && tx_mempool_exists(g_mp, &hash))
            return serve_tx(q, r, max);
    }

    /* Address? (starts with t1, t3, etc.) */
    if (qlen > 20 && (q[0] == 't' || q[0] == 'T')) {
        struct tx_destination dest;
        if (addr_decode(q, &dest))
            return serve_address(q, r, max);
    }

    /* Not found */
    char safe[512];
    html_escape(safe, sizeof(safe), q);
    size_t off = 0;
    APPEND(off, r, max,
        EXPLORER_HEADER("Search") EXPLORER_NAV
        "<h2>Search Results</h2>"
        "<div class='card'>"
        "<p>No results for: <code>%s</code></p>"
        "<p style='color:#666'>Try a block height, block hash, transaction ID, or address.</p>"
        "</div>" EXPLORER_FOOTER, safe);
    return off;
}

/* ── Stats Page with SVG Charts ────────────────────────────── */

/* Smart Y-axis label formatter */
static void format_y_label(char *buf, size_t max, double val)
{
    double av = val < 0 ? -val : val;
    if (av >= 1e9)       snprintf(buf, max, "%.1fG", val / 1e9);
    else if (av >= 1e6)  snprintf(buf, max, "%.1fM", val / 1e6);
    else if (av >= 1e4)  snprintf(buf, max, "%.0fK", val / 1e3);
    else if (av >= 1e3)  snprintf(buf, max, "%.1fK", val / 1e3);
    else if (av >= 100)  snprintf(buf, max, "%.0f", val);
    else if (av >= 10)   snprintf(buf, max, "%.1f", val);
    else if (av >= 1)    snprintf(buf, max, "%.2f", val);
    else if (av >= 0.01) snprintf(buf, max, "%.3f", val);
    else                 snprintf(buf, max, "%.1e", val);
}

static void svg_line_chart(char *out, size_t max, size_t *off,
                            const char *title, const char *color,
                            double *values, const char labels[][20],
                            int count, const char *y_label)
{
    if (count < 2) return;

    double min_v = values[0], max_v = values[0];
    for (int i = 1; i < count; i++) {
        if (values[i] < min_v) min_v = values[i];
        if (values[i] > max_v) max_v = values[i];
    }
    if (max_v == min_v) max_v = min_v + 1;

    /* Auto-detect if log scale is needed (range spans >100x) */
    double pos_min = min_v > 0 ? min_v : 0.01;
    double pos_max = max_v > 0 ? max_v : 1;
    bool use_log = (pos_max / pos_min > 100);

    double range = max_v - min_v;
    double log_min = 0, log_range = 1;
    if (use_log) {
        log_min = log10(pos_min > 0 ? pos_min : 0.01);
        double log_max = log10(pos_max);
        log_range = log_max - log_min;
        if (log_range < 0.1) log_range = 0.1;
    }

    int w = 800, h = 300, pad_l = 90, pad_r = 20, pad_t = 40, pad_b = 60;
    int plot_w = w - pad_l - pad_r;
    int plot_h = h - pad_t - pad_b;

    APPEND(*off, out, max,
        "<svg viewBox='0 0 %d %d' style='width:100%%;max-width:%dpx;height:auto;"
        "background:#0c0c0c;border-radius:8px;margin:4px 0'>",
        w, h, w);

    if (title && title[0])
        APPEND(*off, out, max,
            "<text x='%d' y='25' fill='#33ff99' font-size='16' font-weight='600'>%s%s</text>",
            pad_l, title, use_log ? " (log scale)" : "");

    /* Grid lines + Y labels */
    for (int i = 0; i <= 4; i++) {
        int y = pad_t + plot_h - (plot_h * i / 4);
        double val;
        if (use_log) {
            double log_val = log_min + log_range * i / 4.0;
            val = pow(10.0, log_val);
        } else {
            val = min_v + range * i / 4.0;
        }
        char lbl[32];
        format_y_label(lbl, sizeof(lbl), val);
        APPEND(*off, out, max,
            "<line x1='%d' y1='%d' x2='%d' y2='%d' stroke='#1a1a1a' stroke-width='1'/>"
            "<text x='%d' y='%d' fill='#666' font-size='13' text-anchor='end'>%s</text>",
            pad_l, y, w - pad_r, y,
            pad_l - 10, y + 5, lbl);
    }

    /* Y axis label */
    APPEND(*off, out, max,
        "<text x='14' y='%d' fill='#888' font-size='12' "
        "transform='rotate(-90,14,%d)' text-anchor='middle'>%s</text>",
        pad_t + plot_h / 2, pad_t + plot_h / 2, y_label);

    /* Map value to Y coordinate */
    #define VAL_TO_Y(v) (use_log \
        ? (pad_t + plot_h - (int)(((log10((v) > 0 ? (v) : 0.01)) - log_min) / log_range * plot_h)) \
        : (pad_t + plot_h - (int)(((v) - min_v) / range * plot_h)))

    /* Data line */
    APPEND(*off, out, max, "<polyline fill='none' stroke='%s' stroke-width='2.5' "
        "stroke-linejoin='round' points='", color);

    for (int i = 0; i < count; i++) {
        int x = pad_l + plot_w * i / (count - 1);
        int y = VAL_TO_Y(values[i]);
        APPEND(*off, out, max, "%d,%d ", x, y);
    }
    APPEND(*off, out, max, "'/>");

    /* Fill area under line */
    APPEND(*off, out, max,
        "<polyline fill='%s' fill-opacity='0.1' stroke='none' points='%d,%d ",
        color, pad_l, pad_t + plot_h);
    for (int i = 0; i < count; i++) {
        int x = pad_l + plot_w * i / (count - 1);
        int y = VAL_TO_Y(values[i]);
        APPEND(*off, out, max, "%d,%d ", x, y);
    }
    APPEND(*off, out, max, "%d,%d '/>", w - pad_r, pad_t + plot_h);

    #undef VAL_TO_Y

    /* X labels */
    int label_step = count > 10 ? count / 6 : 1;
    for (int i = 0; i < count; i += label_step) {
        int x = pad_l + plot_w * i / (count - 1);
        APPEND(*off, out, max,
            "<text x='%d' y='%d' fill='#666' font-size='11' text-anchor='middle'>%s</text>",
            x, h - 10, labels[i]);
    }

    APPEND(*off, out, max, "</svg>");
}

__attribute__((unused))
static void svg_stacked_area(char *out, size_t max, size_t *off,
                              const char *title,
                              double bands[][50], int num_bands,
                              const char band_labels[][32],
                              const char band_colors[][10],
                              const char x_labels[][20], int count)
{
    if (count < 2) return;

    int w = 800, h = 350, pad_l = 60, pad_r = 20, pad_t = 40, pad_b = 80;
    int plot_w = w - pad_l - pad_r;
    int plot_h = h - pad_t - pad_b;

    APPEND(*off, out, max,
        "<div class='card'>"
        "<h3 style='color:#33ff99;margin:0 0 8px;font-size:20px'>%s</h3>"
        "<svg viewBox='0 0 %d %d' style='width:100%%;max-width:%dpx;height:auto;"
        "background:#0c0c0c;border-radius:8px'>",
        title, w, h, w);

    /* Draw stacked areas from bottom to top */
    for (int b = num_bands - 1; b >= 0; b--) {
        APPEND(*off, out, max,
            "<polygon fill='%s' fill-opacity='0.7' points='%d,%d ",
            band_colors[b], pad_l, pad_t + plot_h);

        for (int i = 0; i < count; i++) {
            double cumulative = 0;
            for (int k = 0; k <= b; k++) cumulative += bands[k][i];
            int x = pad_l + plot_w * i / (count - 1);
            int y = pad_t + plot_h - (int)(cumulative / 100.0 * plot_h);
            APPEND(*off, out, max, "%d,%d ", x, y);
        }
        APPEND(*off, out, max, "%d,%d '/>", w - pad_r, pad_t + plot_h);
    }

    /* X labels */
    int label_step = count > 10 ? count / 6 : 1;
    for (int i = 0; i < count; i += label_step) {
        int x = pad_l + plot_w * i / (count - 1);
        APPEND(*off, out, max,
            "<text x='%d' y='%d' fill='#666' font-size='11' text-anchor='middle'>%s</text>",
            x, h - pad_b + 16, x_labels[i]);
    }

    /* Legend */
    int lx = pad_l;
    int ly = h - 20;
    for (int b = 0; b < num_bands; b++) {
        APPEND(*off, out, max,
            "<rect x='%d' y='%d' width='12' height='12' fill='%s' rx='2'/>"
            "<text x='%d' y='%d' fill='#ccc' font-size='11'>%s</text>",
            lx, ly - 10, band_colors[b], lx + 16, ly, band_labels[b]);
        lx += 16 + 8 * (int)strlen(band_labels[b]) + 20;
    }

    APPEND(*off, out, max, "</svg></div>");
}

/* Stats page — computed in background thread, served from cache */
static char g_stats_cache[262144] = ""; /* 256KB for all charts */
static size_t g_stats_cache_len = 0;

static void *stats_compute_thread(void *arg)
{
    (void)arg;
    printf("Stats background: computing...\n");
    fflush(stdout);

    uint8_t *r = malloc(262144);
    if (!r) { g_stats_computing = 0; return NULL; }
    size_t max = 262144;
    size_t off = 0;

    /* Use our own RPC connection */
    char buf[65536];
    size_t bufsz = sizeof(buf);
    int tip = 0;
    double diff = 0;
    int64_t total_supply = 0;
    int64_t utxo_count_val = 0;
    int64_t mp_count = 0;
    double mp_bytes = 0;

    rpc_call("getblockchaininfo", "[]", buf, bufsz);
    tip = (int)json_extract_int(buf, "blocks");
    diff = json_extract_real(buf, "difficulty");

    rpc_call("getmempoolinfo", "[]", buf, bufsz);
    mp_count = json_extract_int(buf, "size");
    mp_bytes = (double)json_extract_int(buf, "bytes");

    rpc_call("gettxoutsetinfo", "[]", buf, bufsz);
    utxo_count_val = json_extract_int(buf, "txouts");
    double supply_dbl = json_extract_real(buf, "total_amount");
    total_supply = (int64_t)(supply_dbl * 100000000.0);

    double hashrate = diff * 8192.0 / 150.0;
    char hr_str[64];
    if (hashrate > 1e9) snprintf(hr_str, sizeof(hr_str), "%.2f GH/s", hashrate / 1e9);
    else if (hashrate > 1e6) snprintf(hr_str, sizeof(hr_str), "%.2f MH/s", hashrate / 1e6);
    else if (hashrate > 1e3) snprintf(hr_str, sizeof(hr_str), "%.2f KH/s", hashrate / 1e3);
    else snprintf(hr_str, sizeof(hr_str), "%.0f H/s", hashrate);

    char supply_str[32];
    format_zcl(supply_str, sizeof(supply_str), total_supply);

    APPEND(off, r, max, EXPLORER_HEADER("Network Stats") EXPLORER_NAV
        "<h1>Network Statistics</h1>");

    APPEND(off, r, max,
        "<div class='stats-row'>"
        "<div class='stat'><div class='num'>%d</div><div class='lbl'>Block Height</div></div>"
        "<div class='stat'><div class='num'>%.2f</div><div class='lbl'>Difficulty</div></div>"
        "<div class='stat'><div class='num'>%s</div><div class='lbl'>Est. Hashrate</div></div>"
        "</div>"
        "<div class='stats-row'>"
        "<div class='stat'><div class='num'>%s</div><div class='lbl'>Circulating Supply</div></div>"
        "<div class='stat'><div class='num'>%" PRId64 "</div><div class='lbl'>UTXOs</div></div>"
        "<div class='stat'><div class='num'>%" PRId64 "</div><div class='lbl'>Mempool Txs</div></div>"
        "<div class='stat'><div class='num'>%.1f KB</div><div class='lbl'>Mempool Size</div></div>"
        "</div>",
        tip, diff, hr_str,
        supply_str, utxo_count_val, mp_count, (double)mp_bytes / 1024.0);

    /* Link to HODL wave */
    APPEND(off, r, max,
        "<div class='card' style='text-align:center'>"
        "<a href='/explorer/hodl' style='font-size:20px;font-weight:700'>"
        "View Full HODL Wave Chart &rarr;</a>"
        "<p style='color:#888;margin:4px 0 0;font-size:14px'>"
        "9-year UTXO age distribution from genesis</p></div>");

    /* ── Interactive chart with CSS-only tab controls ── */

    /* Inline CSS for the tab system (pure HTML/CSS, no JS) */
    APPEND(off, r, max,
        "<style>"
        ".tabs input{display:none}"
        ".tabs .tab-bar{display:flex;gap:0;margin:12px 0 0}"
        ".tabs label{padding:10px 20px;background:#1a1a1a;color:#888;"
        "cursor:pointer;font-size:16px;font-weight:600;border:1px solid #222;"
        "border-bottom:none;border-radius:8px 8px 0 0;transition:all 0.2s}"
        ".tabs label:hover{color:#fff;background:#222}"
        ".tabs .panel{display:none;background:#111;border:1px solid #222;"
        "border-radius:0 8px 8px 8px;padding:16px}"
        "#d24h:checked ~ .tab-bar label[for=d24h],"
        "#d7d:checked ~ .tab-bar label[for=d7d],"
        "#d30d:checked ~ .tab-bar label[for=d30d],"
        "#d1y:checked ~ .tab-bar label[for=d1y],"
        "#dall:checked ~ .tab-bar label[for=dall]"
        "{background:#111;color:#4db8ff;border-bottom-color:#111}"
        "#d24h:checked ~ #p-d24h,"
        "#d7d:checked ~ #p-d7d,"
        "#d30d:checked ~ #p-d30d,"
        "#d1y:checked ~ #p-d1y,"
        "#dall:checked ~ #p-dall{display:block}"
        "#h24h:checked ~ .tab-bar label[for=h24h],"
        "#h7d:checked ~ .tab-bar label[for=h7d],"
        "#h30d:checked ~ .tab-bar label[for=h30d],"
        "#h1y:checked ~ .tab-bar label[for=h1y],"
        "#hall:checked ~ .tab-bar label[for=hall]"
        "{background:#111;color:#33ff99;border-bottom-color:#111}"
        "#h24h:checked ~ #p-h24h,"
        "#h7d:checked ~ #p-h7d,"
        "#h30d:checked ~ #p-h30d,"
        "#h1y:checked ~ #p-h1y,"
        "#hall:checked ~ #p-hall{display:block}"
        "</style>");

    /* Compute charts at all time scales */
    struct { const char *label; const char *id; int blocks; } ranges[] = {
        {"24h",  "24h",  576},
        {"7d",   "7d",   4032},
        {"30d",  "30d",  17280},
        {"1yr",  "1y",   210240},
        {"All",  "all",  tip},
    };
    int num_ranges = 5;

    /* Pre-compute all chart data */
    double all_diff[5][40], all_hr[5][40];
    char all_labels[5][40][20];

    for (int ri = 0; ri < num_ranges; ri++) {
        int total = ranges[ri].blocks;
        if (total > tip) total = tip;
        int step = total / 40;
        if (step < 1) step = 1;

        for (int i = 0; i < 40; i++) {
            int h = tip - total + (i + 1) * step;
            if (h < 0) h = 0;
            if (h > tip) h = tip;
            all_diff[ri][i] = diff;
            all_hr[ri][i] = diff * 8192.0 / 150.0; /* raw H/s */

            char params[64];
            snprintf(params, sizeof(params), "[%d]", h);
            rpc_call("getblockhash", params, buf, bufsz);
            char hash[65] = "";
            json_extract_str(buf, "result", hash, sizeof(hash));
            if (hash[0]) {
                char p2[128];
                snprintf(p2, sizeof(p2), "[\"%s\"]", hash);
                rpc_call("getblock", p2, buf, bufsz);
                double d = json_extract_real(buf, "difficulty");
                if (d > 0) {
                    all_diff[ri][i] = d;
                    all_hr[ri][i] = d * 8192.0 / 150.0; /* raw H/s */
                }
            }
            snprintf(all_labels[ri][i], sizeof(all_labels[ri][i]), "%d", h);
        }
    }

    /* ── Difficulty tabbed chart ── */
    APPEND(off, r, max,
        "<h2>Difficulty</h2>"
        "<div class='tabs'>"
        "<input type='radio' name='dtab' id='d24h'>"
        "<input type='radio' name='dtab' id='d7d'>"
        "<input type='radio' name='dtab' id='d30d' checked>"
        "<input type='radio' name='dtab' id='d1y'>"
        "<input type='radio' name='dtab' id='dall'>"
        "<div class='tab-bar'>"
        "<label for='d24h'>24h</label>"
        "<label for='d7d'>7 Days</label>"
        "<label for='d30d'>30 Days</label>"
        "<label for='d1y'>1 Year</label>"
        "<label for='dall'>All Time</label>"
        "</div>");

    for (int ri = 0; ri < num_ranges; ri++) {
        APPEND(off, r, max, "<div class='panel' id='p-d%s'>", ranges[ri].id);
        svg_line_chart((char *)r, max, &off, "",
                        "#4db8ff", all_diff[ri], all_labels[ri], 40, "Difficulty");
        APPEND(off, r, max, "</div>");
    }
    APPEND(off, r, max, "</div>");

    /* ── Hashrate tabbed chart ── */
    APPEND(off, r, max,
        "<h2>Hashrate</h2>"
        "<div class='tabs'>"
        "<input type='radio' name='htab' id='h24h'>"
        "<input type='radio' name='htab' id='h7d'>"
        "<input type='radio' name='htab' id='h30d' checked>"
        "<input type='radio' name='htab' id='h1y'>"
        "<input type='radio' name='htab' id='hall'>"
        "<div class='tab-bar'>"
        "<label for='h24h'>24h</label>"
        "<label for='h7d'>7 Days</label>"
        "<label for='h30d'>30 Days</label>"
        "<label for='h1y'>1 Year</label>"
        "<label for='hall'>All Time</label>"
        "</div>");

    for (int ri = 0; ri < num_ranges; ri++) {
        APPEND(off, r, max, "<div class='panel' id='p-h%s'>", ranges[ri].id);
        svg_line_chart((char *)r, max, &off, "",
                        "#33ff99", all_hr[ri], all_labels[ri], 40, "H/s");
        APPEND(off, r, max, "</div>");
    }
    APPEND(off, r, max, "</div>");

    APPEND(off, r, max, EXPLORER_FOOTER);

    if (off > 0 && off < sizeof(g_stats_cache)) {
        memcpy(g_stats_cache, r, off);
        g_stats_cache_len = off;
    }
    free(r);
    g_stats_computing = 0;
    printf("Stats background: cached %zu bytes\n", g_stats_cache_len);
    fflush(stdout);
    return NULL;
}

static size_t serve_stats(uint8_t *r, size_t max)
{
    /* Return cached version if available */
    if (g_stats_cache_len > 0) {
        size_t copy = g_stats_cache_len < max ? g_stats_cache_len : max;
        memcpy(r, g_stats_cache, copy);
        return copy;
    }

    /* Not cached yet — trigger background computation if not running */
    if (!g_stats_computing) {
        g_stats_computing = 1;
        pthread_t t;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attr, 1024 * 1024); /* 1MB stack */
        pthread_create(&t, &attr, stats_compute_thread, NULL);
        pthread_attr_destroy(&attr);
    }
    size_t off = 0;
    APPEND(off, r, max,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n\r\n"
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta http-equiv='refresh' content='3'>"
        "<link rel='stylesheet' href='/explorer/style.css'>"
        "</head><body>" EXPLORER_NAV
        "<div style='text-align:center;margin:80px 0'>"
        "<h1 style='font-size:36px;color:#33ff99'>Loading Statistics...</h1>"
        "<p style='font-size:20px;color:#888'>Computing charts from blockchain data.</p>"
        "<p style='font-size:16px;color:#555'>Auto-refreshing every 3 seconds...</p>"
        "</div>" EXPLORER_FOOTER);
    return off;
}

/* ── ZSLP Tokens Page ─────────────────────────────────────── */

static size_t serve_tokens(uint8_t *r, size_t max)
{
    size_t off = 0;
    char buf[65536];

    APPEND(off, r, max, EXPLORER_HEADER("ZSLP Tokens") EXPLORER_NAV);
    APPEND(off, r, max,
        "<h1>ZSLP Tokens</h1>"
        "<p style='color:#aaa;font-size:16px'>"
        "Simple Ledger Protocol tokens on ZClassic. "
        "Tokens use OP_RETURN outputs to encode GENESIS, SEND, and MINT operations.</p>");

    /* Scan recent blocks for ZSLP transactions */
    rpc_call("getblockcount", "[]", buf, sizeof(buf));
    int tip = (int)json_extract_int(buf, "result");

    APPEND(off, r, max,
        "<h2>Recent Token Transactions</h2>"
        "<table><tr><th>Block</th><th>Type</th><th>Ticker</th>"
        "<th>Name / Token ID</th><th>TxID</th></tr>");

    int found = 0;
    /* Scan last 500 blocks for SLP txs */
    for (int h = tip; h > tip - 500 && h >= 0 && found < 50 && off + 1024 < max; h--) {
        char params[64];
        snprintf(params, sizeof(params), "[%d]", h);
        rpc_call("getblockhash", params, buf, sizeof(buf));
        char hash[65] = "";
        json_extract_str(buf, "result", hash, sizeof(hash));
        if (!hash[0]) continue;

        char p2[128];
        snprintf(p2, sizeof(p2), "[\"%s\", true]", hash);
        rpc_call("getblock", p2, buf, sizeof(buf));

        /* Check tx count — skip blocks with only coinbase */
        const char *txarr = strstr(buf, "\"tx\":[");
        if (!txarr) continue;
        const char *txend = strchr(txarr, ']');
        if (!txend) continue;

        /* Count txs */
        int ntx = 1;
        for (const char *p = txarr; p < txend; p++)
            if (*p == ',') ntx++;
        if (ntx <= 1) continue; /* only coinbase */

        /* Check each non-coinbase tx */
        const char *p = txarr + 6;
        int idx = 0;
        while (p < txend && found < 50 && off + 1024 < max) {
            if (*p == '"') {
                p++;
                const char *end = strchr(p, '"');
                if (!end) break;
                char txid[65];
                size_t tlen = (size_t)(end - p);
                if (tlen > 64) tlen = 64;
                memcpy(txid, p, tlen);
                txid[tlen] = '\0';
                p = end + 1;

                if (idx > 0) { /* skip coinbase */
                    /* Fetch raw tx to check for OP_RETURN / SLP */
                    char tp[128];
                    snprintf(tp, sizeof(tp), "[\"%s\", 1]", txid);
                    char txbuf[65536];
                    rpc_call("getrawtransaction", tp, txbuf, sizeof(txbuf));

                    /* Quick check: does it have scriptPubKey with OP_RETURN? */
                    if (strstr(txbuf, "\"type\":\"nulldata\"")) {
                        /* Try to find the hex */
                        const char *hex_start = strstr(txbuf, "\"hex\":\"6a");
                        if (hex_start) {
                            hex_start += 7; /* skip "hex":" */
                            const char *hex_end = strchr(hex_start, '"');
                            if (hex_end) {
                                /* Decode hex to bytes and try SLP parse */
                                size_t hlen = (size_t)(hex_end - hex_start);
                                if (hlen < 2048) {
                                    uint8_t script[1024];
                                    size_t slen = 0;
                                    for (size_t j = 0; j + 1 < hlen && slen < sizeof(script); j += 2) {
                                        unsigned int byte;
                                        if (sscanf(hex_start + j, "%2x", &byte) == 1)
                                            script[slen++] = (uint8_t)byte;
                                    }

                                    struct slp_message slp;
                                    if (slp_parse(script, slen, &slp)) {
                                        char safe_ticker[128] = "", safe_name[256] = "";
                                        char short_txid[18];
                                        snprintf(short_txid, sizeof(short_txid), "%.8s...%.4s",
                                                 txid, txid + 60);

                                        const char *type_str = "?";
                                        if (slp.type == SLP_TX_GENESIS) {
                                            type_str = "GENESIS";
                                            html_escape(safe_ticker, sizeof(safe_ticker), slp.ticker);
                                            html_escape(safe_name, sizeof(safe_name), slp.name);
                                        } else if (slp.type == SLP_TX_SEND) {
                                            type_str = "SEND";
                                            char tid[65];
                                            uint256_get_hex(&slp.token_id, tid);
                                            snprintf(safe_name, sizeof(safe_name),
                                                     "<a href='/explorer/tx/%s' class='hash'>%.16s...</a>",
                                                     tid, tid);
                                        } else if (slp.type == SLP_TX_MINT) {
                                            type_str = "MINT";
                                            char tid[65];
                                            uint256_get_hex(&slp.token_id, tid);
                                            snprintf(safe_name, sizeof(safe_name),
                                                     "<a href='/explorer/tx/%s' class='hash'>%.16s...</a>",
                                                     tid, tid);
                                        }

                                        APPEND(off, r, max,
                                            "<tr><td><a href='/explorer/block/%d'>%d</a></td>"
                                            "<td><span class='tag tag-slp'>%s</span></td>"
                                            "<td style='color:#ff99ff;font-weight:700'>%s</td>"
                                            "<td>%s</td>"
                                            "<td class='hash'><a href='/explorer/tx/%s'>%s</a></td></tr>",
                                            h, h, type_str, safe_ticker, safe_name, txid, short_txid);
                                        found++;
                                    }
                                }
                            }
                        }
                    }
                }
                idx++;
            } else {
                p++;
            }
        }
    }

    APPEND(off, r, max, "</table>");

    if (found == 0) {
        APPEND(off, r, max,
            "<div class='card'>"
            "<p style='font-size:18px'>No ZSLP token transactions found in recent blocks.</p>"
            "<p style='color:#888'>ZSLP tokens use the Simple Ledger Protocol (SLP) with "
            "OP_RETURN outputs. Create a token with <code>zslp_create_token</code> via RPC.</p>"
            "</div>");
    }

    APPEND(off, r, max,
        "<h2>About ZSLP</h2>"
        "<div class='card'>"
        "<p style='font-size:16px;line-height:1.8'>"
        "ZSLP (ZClassic Simple Ledger Protocol) enables custom tokens on the ZClassic blockchain. "
        "Based on the SLP specification from Bitcoin Cash, tokens are encoded entirely in "
        "OP_RETURN outputs with no consensus changes required.</p>"
        "<div class='grid' style='margin-top:12px'>"
        "<div class='label'>GENESIS</div><div class='val'>Create a new token (ticker, name, supply, decimals)</div>"
        "<div class='label'>SEND</div><div class='val'>Transfer tokens between addresses</div>"
        "<div class='label'>MINT</div><div class='val'>Create additional supply (if baton exists)</div>"
        "<div class='label'>Token ID</div><div class='val'>The GENESIS transaction hash uniquely identifies each token</div>"
        "</div></div>");

    APPEND(off, r, max, EXPLORER_FOOTER);
    return off;
}

/* ── 9-Year HODL Wave Chart ───────────────────────────────── */

/* Cache — computed in background, served instantly */
static char g_hodl_cache[65536] = "";
static size_t g_hodl_cache_len = 0;


static size_t serve_hodl(uint8_t *r, size_t max)
{
    if (!g_ndb || !g_ndb->open) return 0;

    /* Return cached version if available */
    if (g_hodl_cache_len > 0) {
        size_t copy = g_hodl_cache_len < max ? g_hodl_cache_len : max;
        memcpy(r, g_hodl_cache, copy);
        return copy;
    }

    /* If not cached and not computing, start background computation */
    if (!g_hodl_computing) {
        g_hodl_computing = 1;
        pthread_t t;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&t, &attr, hodl_compute_thread, NULL);
        pthread_attr_destroy(&attr);
    }

    /* Return a "computing" placeholder page */
    size_t off = 0;
    APPEND(off, r, max,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n\r\n"
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta http-equiv='refresh' content='5'>"
        "<link rel='stylesheet' href='/explorer/style.css'>"
        "</head><body>" EXPLORER_NAV
        "<div style='text-align:center;margin:80px 0'>"
        "<h1 style='font-size:36px;color:#aa66ff;font-family:Georgia,serif'>"
        "Computing HODL Wave...</h1>"
        "<p style='font-size:20px;color:#888'>Scanning UTXO set from SQLite.</p>"
        "<p style='font-size:16px;color:#555'>Auto-refreshing...</p>"
        "</div>" EXPLORER_FOOTER);
    return off;
}

static void *hodl_compute_thread(void *arg)
{
    (void)arg;
    printf("HODL background: starting computation...\n");
    fflush(stdout);

    /* We need our own SQLite connection for the background thread */
    char db_path[1024];
    snprintf(db_path, sizeof(db_path), "%s/node.db", g_datadir);
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        printf("HODL background: failed to open db\n");
        g_hodl_computing = 0;
        return NULL;
    }
    sqlite3_exec(db, "PRAGMA mmap_size=268435456", NULL, NULL, NULL);

    size_t off = 0;
    uint8_t *r = (uint8_t *)malloc(65536);
    size_t max = 65536;
    if (!r) { sqlite3_close(db); g_hodl_computing = 0; return NULL; }
    char buf[65536];

    /* Get current tip from SQLite (our indexed data) */
    int tip = 0;
    {
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT MAX(height) FROM blocks", -1, &stmt, NULL) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW)
                tip = sqlite3_column_int(stmt, 0);
            sqlite3_finalize(stmt);
        }
    }
    if (tip < 1) {
        rpc_call("getblockcount", "[]", buf, sizeof(buf));
        tip = (int)json_extract_int(buf, "result");
    }
    if (tip < 1) tip = 3047000;

    printf("HODL chart: tip height = %d\n", tip);

    /* HODL Wave computation using REAL UTC timestamps.
     * Sample monthly from genesis (Nov 2016) to now.
     * For each month: find height at that time, find height at time-1yr,
     * compute ratio of UTXO value older than 1yr. */

    /* Genesis: Nov 6, 2016 = 1478403829 */
    int64_t genesis_ts = 1478403829;
    int64_t now_ts = (int64_t)time(NULL);
    int64_t one_year = 365 * 86400;
    int64_t one_month = 30 * 86400;

    /* Start from genesis — show the full history including the first year at 0% */
    int64_t start_ts = genesis_ts;
    int npts_raw = (int)((now_ts - start_ts) / one_month) + 1;
    if (npts_raw < 2) npts_raw = 2;
    if (npts_raw > 120) npts_raw = 120;
    int npts = npts_raw;

    double pct_over_1yr[120];
    char labels[120][20];
    memset(pct_over_1yr, 0, sizeof(pct_over_1yr));

    printf("HODL chart: computing %d monthly points from %"PRId64" to %"PRId64"...\n",
           npts, start_ts, now_ts);
    fflush(stdout);
    int64_t t_hodl = (int64_t)time(NULL);

    /* Single-pass: scan UTXOs sorted by height, build cumulative totals.
     * Also build a height→time lookup from blocks table. */

    /* Step 1: Build height→time map from sampled blocks (fast: ~1s).
     * Then compute checkpoints using interpolation. */
    #define TIME_SAMPLES 200
    int sample_h[TIME_SAMPLES];
    int64_t sample_t[TIME_SAMPLES];
    int num_samples = 0;
    {
        sqlite3_stmt *s = NULL;
        int step = tip > TIME_SAMPLES ? tip / TIME_SAMPLES : 1;
        char sql[256];
        snprintf(sql, sizeof(sql),
            "SELECT height, time FROM blocks WHERE height %% %d = 0 "
            "AND time > 0 ORDER BY height", step);
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
            while (sqlite3_step(s) == SQLITE_ROW && num_samples < TIME_SAMPLES) {
                sample_h[num_samples] = sqlite3_column_int(s, 0);
                sample_t[num_samples] = sqlite3_column_int64(s, 1);
                num_samples++;
            }
            sqlite3_finalize(s);
        }
    }

    /* Build checkpoint list using sample interpolation */
    int64_t cp_time[120];
    int cp_height[120];
    int cp_old_height[120];

    for (int i = 0; i < npts; i++) {
        cp_time[i] = start_ts + (int64_t)i * (now_ts - start_ts) / (npts - 1);

        /* Find height at this time by binary search in samples */
        cp_height[i] = 0;
        for (int k = num_samples - 1; k >= 0; k--) {
            if (sample_t[k] <= cp_time[i]) { cp_height[i] = sample_h[k]; break; }
        }
        cp_old_height[i] = 0;
        int64_t old_time = cp_time[i] - one_year;
        for (int k = num_samples - 1; k >= 0; k--) {
            if (sample_t[k] <= old_time) { cp_old_height[i] = sample_h[k]; break; }
        }

        /* Generate label */
        time_t t = (time_t)cp_time[i];
        struct tm tm;
        gmtime_r(&t, &tm);
        snprintf(labels[i], sizeof(labels[i]), "%d", tm.tm_year + 1900);
        /* Label January of each year + the very first point (genesis) */
        if (tm.tm_mon != 0 && i != 0) labels[i][0] = '\0';
    }

    /* Step 2: Fast GROUP BY approach — aggregate UTXOs into 10K-block buckets,
     * then compute cumulative sums. ~2 seconds for 1.3M UTXOs. */
    {
        #define BUCKET_SIZE 10000
        #define MAX_BUCKETS 400
        int64_t bucket_vals[MAX_BUCKETS];
        int bucket_count = 0;
        memset(bucket_vals, 0, sizeof(bucket_vals));

        sqlite3_stmt *stmt = NULL;
        char sql_buf[256];
        snprintf(sql_buf, sizeof(sql_buf),
            "SELECT (height/%d)*%d, SUM(value) FROM utxos "
            "GROUP BY height/%d ORDER BY 1", BUCKET_SIZE, BUCKET_SIZE, BUCKET_SIZE);

        if (sqlite3_prepare_v2(db, sql_buf, -1, &stmt, NULL) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW && bucket_count < MAX_BUCKETS) {
                int bh = sqlite3_column_int(stmt, 0);
                int64_t bv = sqlite3_column_int64(stmt, 1);
                int idx = bh / BUCKET_SIZE;
                if (idx >= 0 && idx < MAX_BUCKETS) {
                    bucket_vals[idx] = bv;
                    if (idx >= bucket_count) bucket_count = idx + 1;
                }
            }
            sqlite3_finalize(stmt);
        }

        /* Build cumulative sum array */
        int64_t cumsum[MAX_BUCKETS + 1];
        cumsum[0] = 0;
        for (int i = 0; i < bucket_count; i++)
            cumsum[i + 1] = cumsum[i] + bucket_vals[i];

        /* For each checkpoint, lookup cumulative values using bucket index */
        for (int i = 0; i < npts; i++) {
            int at_bucket = cp_height[i] / BUCKET_SIZE;
            int old_bucket = cp_old_height[i] / BUCKET_SIZE;
            if (at_bucket >= bucket_count) at_bucket = bucket_count;
            if (old_bucket >= bucket_count) old_bucket = bucket_count;
            if (at_bucket < 0) at_bucket = 0;
            if (old_bucket < 0) old_bucket = 0;

            int64_t total_val = cumsum[at_bucket];
            int64_t old_val = cumsum[old_bucket];

            pct_over_1yr[i] = total_val > 0
                ? (double)old_val / (double)total_val * 100.0 : 0;
            if (pct_over_1yr[i] > 100) pct_over_1yr[i] = 100;
            if (pct_over_1yr[i] < 0) pct_over_1yr[i] = 0;
        }
    }

    int64_t hodl_elapsed = (int64_t)time(NULL) - t_hodl;
    printf("HODL chart: computed %d points in %" PRId64 "s\n", npts, hodl_elapsed);
    fflush(stdout);

    /* Get current >1yr percentage for the headline */
    double current_pct = pct_over_1yr[npts - 1];

    APPEND(off, r, max, EXPLORER_HEADER("HODL Wave") EXPLORER_NAV);

    /* Newspaper-style headline */
    APPEND(off, r, max,
        "<div style='text-align:center;margin:30px 0 10px'>"
        "<h1 style='font-size:48px;color:#fff;font-weight:800;margin:0;"
        "font-family:Georgia,\"Times New Roman\",serif'>"
        "%.1f%% of ZCL Hasn't Moved in Over 1 Year</h1>"
        "<p style='font-size:20px;color:#888;margin:8px 0 0;"
        "font-family:Georgia,serif'>"
        "9-Year HODL Wave &mdash; ZClassic Blockchain Analysis</p>"
        "</div>",
        current_pct);

    /* Large SVG chart — newspaper-quality */
    int w = 1000, h = 500;
    int pad_l = 70, pad_r = 30, pad_t = 30, pad_b = 60;
    int plot_w = w - pad_l - pad_r;
    int plot_h = h - pad_t - pad_b;

    APPEND(off, r, max,
        "<svg viewBox='0 0 %d %d' style='width:100%%;max-width:%dpx;height:auto;"
        "background:#0c0c0c;border-radius:12px;margin:20px auto;display:block;"
        "border:1px solid #1a1a1a'>",
        w, h, w);

    /* Y-axis grid + labels (0%, 25%, 50%, 75%, 100%) */
    for (int g = 0; g <= 4; g++) {
        int y = pad_t + plot_h - (plot_h * g / 4);
        APPEND(off, r, max,
            "<line x1='%d' y1='%d' x2='%d' y2='%d' stroke='#1a1a1a' stroke-width='1'/>",
            pad_l, y, w - pad_r, y);
        APPEND(off, r, max,
            "<text x='%d' y='%d' fill='#888' font-size='16' "
            "font-family='Georgia,serif' text-anchor='end' font-weight='600'>%d%%</text>",
            pad_l - 12, y + 6, g * 25);
    }

    /* Y-axis title */
    APPEND(off, r, max,
        "<text x='18' y='%d' fill='#aaa' font-size='14' "
        "font-family='Georgia,serif' "
        "transform='rotate(-90,18,%d)' text-anchor='middle'>"
        "Coins Unmoved &gt;1 Year</text>",
        pad_t + plot_h / 2, pad_t + plot_h / 2);

    /* Filled area */
    APPEND(off, r, max,
        "<defs><linearGradient id='hodlGrad' x1='0' y1='0' x2='0' y2='1'>"
        "<stop offset='0%%' stop-color='#8844ff' stop-opacity='0.6'/>"
        "<stop offset='100%%' stop-color='#8844ff' stop-opacity='0.05'/>"
        "</linearGradient></defs>");

    APPEND(off, r, max,
        "<polygon fill='url(#hodlGrad)' points='%d,%d ",
        pad_l, pad_t + plot_h);
    for (int i = 0; i < npts; i++) {
        int x = pad_l + plot_w * i / (npts - 1);
        int y = pad_t + plot_h - (int)(pct_over_1yr[i] / 100.0 * plot_h);
        APPEND(off, r, max, "%d,%d ", x, y);
    }
    APPEND(off, r, max, "%d,%d '/>", w - pad_r, pad_t + plot_h);

    /* Line on top */
    APPEND(off, r, max,
        "<polyline fill='none' stroke='#aa66ff' stroke-width='3' "
        "stroke-linejoin='round' points='");
    for (int i = 0; i < npts; i++) {
        int x = pad_l + plot_w * i / (npts - 1);
        int y = pad_t + plot_h - (int)(pct_over_1yr[i] / 100.0 * plot_h);
        APPEND(off, r, max, "%d,%d ", x, y);
    }
    APPEND(off, r, max, "'/>");

    /* Current value dot + label */
    {
        int x = pad_l + plot_w;
        int y = pad_t + plot_h - (int)(current_pct / 100.0 * plot_h);
        APPEND(off, r, max,
            "<circle cx='%d' cy='%d' r='6' fill='#aa66ff'/>"
            "<text x='%d' y='%d' fill='#fff' font-size='18' "
            "font-family='Georgia,serif' font-weight='700' text-anchor='end'>"
            "%.1f%%</text>",
            x, y, x - 12, y - 12, current_pct);
    }

    /* X-axis year labels */
    for (int i = 0; i < npts; i++) {
        if (labels[i][0] != '\0') {
            int x = pad_l + plot_w * i / (npts - 1);
            APPEND(off, r, max,
                "<line x1='%d' y1='%d' x2='%d' y2='%d' stroke='#333' stroke-width='1'/>"
                "<text x='%d' y='%d' fill='#888' font-size='16' "
                "font-family='Georgia,serif' text-anchor='middle'>%s</text>",
                x, pad_t + plot_h, x, pad_t + plot_h + 8,
                x, pad_t + plot_h + 28, labels[i]);
        }
    }

    /* Title inside chart */
    APPEND(off, r, max,
        "<text x='%d' y='%d' fill='#555' font-size='13' "
        "font-family='Georgia,serif' text-anchor='end'>"
        "Source: ZClassic23 SQLite UTXO Index &mdash; zclnet.net</text>",
        w - pad_r, h - 8);

    APPEND(off, r, max, "</svg>");

    /* Description */
    APPEND(off, r, max,
        "<div style='max-width:800px;margin:20px auto;font-family:Georgia,serif;"
        "font-size:18px;line-height:1.8;color:#ccc'>"
        "<p>This chart shows the percentage of the ZClassic coin supply that has "
        "remained unmoved for more than one year, computed monthly over the past "
        "9 years from the live UTXO set indexed in SQLite.</p>"
        "<p style='color:#888'>A high percentage indicates strong holder conviction "
        "&mdash; coins sitting in cold storage rather than being traded. "
        "Currently <b style='color:#aa66ff'>%.1f%%</b> of all ZCL has not moved "
        "in over a year.</p>"
        "</div>",
        current_pct);

    APPEND(off, r, max, EXPLORER_FOOTER);

    /* Store in global cache */
    if (off > 0 && off < sizeof(g_hodl_cache)) {
        memcpy(g_hodl_cache, r, off);
        g_hodl_cache_len = off;
    }

    free(r);
    sqlite3_close(db);
    g_hodl_computing = 0;
    printf("HODL background: cached %zu bytes\n", g_hodl_cache_len);
    fflush(stdout);
    return NULL;
}

/* ── CSS Stylesheet ───────────────────────────────────────── */

static size_t serve_css(uint8_t *r, size_t max)
{
    /* Reload CSS from disk each time (allows live editing) */
    load_css();
    size_t off = 0;
    int n = snprintf((char *)r, max,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/css; charset=utf-8\r\n"
        "Cache-Control: public, max-age=60\r\n"
        "Connection: close\r\n\r\n");
    if (n > 0) off = (size_t)n;
    if (off + g_css_len < max) {
        memcpy(r + off, g_css_cache, g_css_len);
        off += g_css_len;
    }
    return off;
}

/* ── Main Request Handler ─────────────────────────────────── */

size_t explorer_handle_request(const char *method, const char *path,
                                const uint8_t *body, size_t body_len,
                                uint8_t *response, size_t response_max)
{
    (void)method; (void)body; (void)body_len;
    if (!path || !response) return 0;

    if (strcmp(path, "/explorer/style.css") == 0)
        return serve_css(response, response_max);

    if (strcmp(path, "/explorer/favicon.png") == 0 ||
        strcmp(path, "/favicon.ico") == 0) {
        char fpath[1200];
        snprintf(fpath, sizeof(fpath), "%s/explorer/favicon.png", g_datadir);
        FILE *f = fopen(fpath, "rb");
        if (f) {
            size_t off = 0;
            int n = snprintf((char *)response, response_max,
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: image/png\r\n"
                "Cache-Control: public, max-age=86400\r\n"
                "Connection: close\r\n\r\n");
            if (n > 0) off = (size_t)n;
            off += fread(response + off, 1, response_max - off, f);
            fclose(f);
            return off;
        }
        return 0;
    }

    if (strcmp(path, "/explorer") == 0 || strcmp(path, "/explorer/") == 0)
        return serve_dashboard(response, response_max);

    if (strcmp(path, "/explorer/stats") == 0 || strcmp(path, "/explorer/stats/") == 0)
        return serve_stats(response, response_max);

    if (strcmp(path, "/explorer/tokens") == 0 || strcmp(path, "/explorer/tokens/") == 0)
        return serve_tokens(response, response_max);

    if (strcmp(path, "/explorer/hodl") == 0 || strcmp(path, "/explorer/hodl/") == 0)
        return serve_hodl(response, response_max);

    if (strncmp(path, "/explorer/block/", 16) == 0)
        return serve_block(path + 16, response, response_max);

    if (strncmp(path, "/explorer/tx/", 13) == 0)
        return serve_tx(path + 13, response, response_max);

    if (strncmp(path, "/explorer/address/", 18) == 0)
        return serve_address(path + 18, response, response_max);

    if (strncmp(path, "/explorer/search", 16) == 0) {
        const char *q = strstr(path, "q=");
        return serve_search(q ? q + 2 : "", response, response_max);
    }

    return 0; /* unhandled → caller returns 404 */
}
