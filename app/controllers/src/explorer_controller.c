/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Block explorer controller — comprehensive blockchain explorer served
 * over Tor .onion. Supports blocks, transactions (transparent + shielded),
 * ZSLP tokens, and address lookups. */

#include "controllers/explorer_controller.h"
#include "controllers/explorer_stats.h"
#include "controllers/explorer_factoids.h"
#include "controllers/api_controller.h"
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
#include "zslp/slp.h"
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
#include <stdatomic.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>
#include <math.h>

#include "controllers/explorer_internal.h"
#include "util/template.h"
#include "views/wallet_templates_gen.h"
#include "views/explorer_css.h"
#include "views/format_helpers.h"

struct explorer_context {
    struct main_state *main_state;
    struct tx_mempool *mempool;
    struct coins_view_cache *coins_tip;
    struct node_db *node_db;
    const char *datadir;
};

struct explorer_rpc_backend {
    char user[128];
    char pass[128];
    int proxy_port;
};

struct explorer_assets {
    char explorer_dir[1024];
    char css_cache[8192];
    size_t css_len;
};

static struct explorer_context g_explorer_ctx = {0};
static struct explorer_rpc_backend g_explorer_rpc = {
    .user = "zcluser",
    .pass = "zclpass",
    .proxy_port = 8023,
};
static struct explorer_assets g_explorer_assets = {0};

static struct explorer_context *explorer_ctx(void)
{
    return &g_explorer_ctx;
}

static struct explorer_rpc_backend *explorer_rpc(void)
{
    return &g_explorer_rpc;
}

static struct explorer_assets *explorer_assets(void)
{
    return &g_explorer_assets;
}

/* Transitional aliases while this large controller is moved over in slices. */
/* ── Template system ───────────────────────────────────────── */

static void ensure_explorer_dir(void)
{
    struct explorer_context *ctx = explorer_ctx();
    struct explorer_assets *assets = explorer_assets();
    if (!ctx->datadir) return;
    snprintf(assets->explorer_dir, sizeof(assets->explorer_dir), "%s/explorer",
             ctx->datadir);
    mkdir(assets->explorer_dir, 0755);
}

static void write_default_file(const char *filename, const char *content)
{
    struct explorer_assets *assets = explorer_assets();
    char path[1200];
    snprintf(path, sizeof(path), "%s/%s", assets->explorer_dir, filename);
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
    struct explorer_assets *assets = explorer_assets();
    char path[1200];
    snprintf(path, sizeof(path), "%s/style.css", assets->explorer_dir);
    FILE *f = fopen(path, "r");
    if (f) {
        assets->css_len = fread(assets->css_cache, 1, sizeof(assets->css_cache) - 1, f);
        assets->css_cache[assets->css_len] = '\0';
        fclose(f);
    } else {
        /* Fallback to compiled-in CSS */
        assets->css_len = strlen(explorer_css);
        if (assets->css_len >= sizeof(assets->css_cache))
            assets->css_len = sizeof(assets->css_cache) - 1;
        memcpy(assets->css_cache, explorer_css, assets->css_len);
        assets->css_cache[assets->css_len] = '\0';
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
static void *tokens_compute_thread(void *arg);
static void *factoids_compute_thread(void *arg);
static _Atomic int g_stats_computing;
static _Atomic int g_tokens_computing;
static _Atomic int g_hodl_computing;
static _Atomic int g_factoids_computing;
static _Atomic int g_prewarm_started;

static bool explorer_start_detached_thread(pthread_t *thread_out,
                                           void *(*entry)(void *),
                                           void *arg,
                                           size_t stack_size)
{
    pthread_attr_t attr;
    bool ok = false;

    if (!thread_out || !entry)
        return false;
    if (pthread_attr_init(&attr) != 0)
        return false;
    if (pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) != 0)
        goto cleanup;
    if (stack_size > 0 && pthread_attr_setstacksize(&attr, stack_size) != 0)
        goto cleanup;
    if (pthread_create(thread_out, &attr, entry, arg) != 0)
        goto cleanup;
    ok = true;

cleanup:
    pthread_attr_destroy(&attr);
    return ok;
}

static bool explorer_start_once(_Atomic int *flag,
                                void *(*entry)(void *),
                                const char *name)
{
    int expected = 0;
    pthread_t t;

    if (!flag || !entry)
        return false;
    if (!atomic_compare_exchange_strong(flag, &expected, 1))
        return expected == 1;
    if (!explorer_start_detached_thread(&t, entry, NULL, 2 * 1024 * 1024)) {
        atomic_store(flag, 0);
        if (name)
            fprintf(stderr, "Explorer: failed to start %s thread\n", name);
        return false;
    }
    return true;
}

static void prewarm_caches(void)
{
    /* Delay 5 seconds to let RPC server start */
    sleep(5);

    printf("Explorer: pre-warming stats cache...\n");
    fflush(stdout);
    explorer_start_once(&g_stats_computing, stats_compute_thread,
                        "stats_compute");

    printf("Explorer: pre-warming HODL wave cache...\n");
    fflush(stdout);
    explorer_start_once(&g_hodl_computing, hodl_compute_thread,
                        "hodl_compute");

    printf("Explorer: pre-warming tokens cache...\n");
    fflush(stdout);
    explorer_start_once(&g_tokens_computing, tokens_compute_thread,
                        "tokens_compute");

    printf("Explorer: pre-warming factoids cache...\n");
    fflush(stdout);
    explorer_start_once(&g_factoids_computing, factoids_compute_thread,
                        "factoids_compute");
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
    struct explorer_context *ctx = explorer_ctx();
    ctx->main_state = ms;
    ctx->mempool = mp;
    ctx->coins_tip = coins_tip;
    ctx->node_db = ndb;
    ctx->datadir = datadir;
    init_default_templates();

    /* Pre-warm caches in background after startup */
    explorer_start_once(&g_prewarm_started, prewarm_thread, "prewarm");
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
    addr.sin_port = htons((uint16_t)explorer_rpc()->proxy_port);

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
    snprintf(auth_plain, sizeof(auth_plain), "%s:%s",
             explorer_rpc()->user, explorer_rpc()->pass);
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
/* JSON extraction: use shared zcl_json_extract_* from format_helpers.h.
 * Thin wrappers preserve the old call-site signatures. */
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

void explorer_set_rpc(const char *user, const char *pass, int port)
{
    struct explorer_rpc_backend *rpc = explorer_rpc();
    if (user) snprintf(rpc->user, sizeof(rpc->user), "%s", user);
    if (pass) snprintf(rpc->pass, sizeof(rpc->pass), "%s", pass);
    if (port > 0) rpc->proxy_port = port;
}

static int native_chain_height(void)
{
    struct explorer_context *ctx = explorer_ctx();
    if (ctx->main_state)
        return active_chain_height(&ctx->main_state->chain_active);
    /* Fallback: query SQLite when running without full node (e.g. GTK browser) */
    if (ctx->node_db && ctx->node_db->db) {
        sqlite3_stmt *s = NULL;
        int h = -1;
        if (sqlite3_prepare_v2(ctx->node_db->db,
                "SELECT MAX(height) FROM blocks", -1, &s, NULL) == SQLITE_OK && s) {
            if (sqlite3_step(s) == SQLITE_ROW)
                h = sqlite3_column_int(s, 0);
            sqlite3_finalize(s);
        }
        return h;
    }
    return -1;
}

static bool use_rpc_proxy(void)
{
    /* Use RPC/SQLite proxy when no main_state (standalone browser)
     * or when chain height is not available */
    if (!explorer_ctx()->main_state) return true;
    return native_chain_height() < 1;
}

/* ── Helpers ──────────────────────────────────────────────── */

/* html_escape provided by util/template.h (included above) */

/* difficulty_from_bits() now in chain/pow.h */
#include "chain/pow.h"

static double get_difficulty(const struct block_index *bi)
{
    if (!bi) return 1.0;
    return difficulty_from_bits(bi->nBits);
}

static bool explorer_param_is_printable_ascii(const char *s)
{
    if (!s) return false;
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c < 32 || c > 126)
            return false;
    }
    return true;
}

static void format_time(char *buf, size_t max, uint32_t t)
{
    zcl_format_time(buf, max, (int64_t)t);
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
    zcl_format_zcl(buf, max, zatoshi);
}

static bool is_all_hex(const char *s, size_t len)
{
    return zcl_is_all_hex(s, len);
}

static bool is_all_digits(const char *s)
{
    return zcl_is_all_digits(s);
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

/* ── Macros from explorer_internal.h ──────────────────────── */
/* EXPLORER_HEADER, EXPLORER_NAV, EXPLORER_FOOTER, and APPEND
 * are all defined in controllers/explorer_internal.h (single source
 * of truth for all explorer pages). */

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

    APPEND(off, r, max, EXPLORER_HEADER("Dashboard"));
    off += explorer_emit_nav((char *)r + off, max - off, "blocks");

    char ht_fmt[32];
    format_with_commas(ht_fmt, sizeof(ht_fmt), tip);
    APPEND(off, r, max,
        "<div class='stats-row'>"
        "<div class='stat'><div class='num'>%s</div><div class='lbl'>Block Height</div></div>"
        "<div class='stat'><div class='num'>%.2f</div><div class='lbl'>Difficulty</div></div>"
        "<div class='stat'><div class='num'>%" PRId64 "</div><div class='lbl'>Mempool Txs</div></div>"
        "<div class='stat'><div class='num'>%.1f KB</div><div class='lbl'>Mempool Size</div></div>"
        "</div>",
        ht_fmt, diff, mp_count, (double)mp_bytes / 1024.0);

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

static size_t serve_dashboard_native_page(uint8_t *r, size_t max, int page)
{
    struct explorer_context *ctx = explorer_ctx();
    size_t off = 0;

    int tip = active_chain_height(&ctx->main_state->chain_active);
    const struct block_index *tip_bi = active_chain_tip(&ctx->main_state->chain_active);

    APPEND(off, r, max, EXPLORER_HEADER("Dashboard"));
    off += explorer_emit_nav((char *)r + off, max - off, "blocks");

    size_t mp_count = ctx->mempool ? tx_mempool_size(ctx->mempool) : 0;
    uint64_t mp_bytes = ctx->mempool ? tx_mempool_total_size(ctx->mempool) : 0;

    char ht_fmt[32];
    format_with_commas(ht_fmt, sizeof(ht_fmt), tip);
    APPEND(off, r, max,
        "<div class='stats-row'>"
        "<div class='stat'><div class='num'>%s</div><div class='lbl'>Block Height</div></div>"
        "<div class='stat'><div class='num'>%.2f</div><div class='lbl'>Difficulty</div></div>"
        "<div class='stat'><div class='num'>%zu</div><div class='lbl'>Mempool Txs</div></div>"
        "<div class='stat'><div class='num'>%.1f KB</div><div class='lbl'>Mempool Size</div></div>"
        "</div>",
        ht_fmt, get_difficulty(tip_bi), mp_count, (double)mp_bytes / 1024.0);

    APPEND(off, r, max,
        "<h2>Latest Blocks</h2>"
        "<table><tr><th>Height</th><th>Hash</th><th>Time</th>"
        "<th>Txs</th><th>Difficulty</th><th>Shielded</th></tr>");

    int per_page = 25;
    if (page < 0) page = 0;
    int start_height = tip - page * per_page;
    int end_height = start_height - per_page + 1;
    if (end_height < 0) end_height = 0;

    for (int h = start_height; h >= end_height && h >= 0; h--) {
        const struct block_index *bi = active_chain_at(&ctx->main_state->chain_active, h);
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

        char h_fmt[32];
        format_with_commas(h_fmt, sizeof(h_fmt), h);
        APPEND(off, r, max,
            "<tr><td><a href='/explorer/block/%d'>%s</a></td>"
            "<td class='hash'><a href='/explorer/block/%s'>%s</a></td>"
            "<td>%s</td><td>%u</td><td>%.4f</td><td class='amount'>%s</td></tr>",
            h, h_fmt, hash, short_hash, ts, bi->nTx, get_difficulty(bi), sap_val);

        if (off + 512 >= max) break;
    }

    APPEND(off, r, max, "</table>");

    /* Pagination */
    APPEND(off, r, max, "<div class='pager'>");
    if (page > 0)
        APPEND(off, r, max, "<a href='/explorer?page=%d'>&larr; Newer</a>", page - 1);
    if (end_height > 0)
        APPEND(off, r, max, "<a href='/explorer?page=%d'>Older &rarr;</a>", page + 1);
    APPEND(off, r, max, "</div>");

    APPEND(off, r, max, EXPLORER_FOOTER);
    return off;
}

__attribute__((unused))
static size_t serve_dashboard_native(uint8_t *r, size_t max)
{
    return serve_dashboard_native_page(r, max, 0);
}

/* ── Dashboard (SQLite-only, no RPC or main_state needed) ── */


static size_t serve_dashboard_with_page(uint8_t *r, size_t max, int page)
{
    struct explorer_context *ctx = explorer_ctx();
    /* Use native if chain is loaded, otherwise fall back to RPC proxy */
    if (ctx->main_state && active_chain_height(&ctx->main_state->chain_active) > 0)
        return serve_dashboard_native_page(r, max, page);
    return serve_dashboard_rpc(r, max);
}

static size_t serve_dashboard(uint8_t *r, size_t max)
{
    return serve_dashboard_with_page(r, max, 0);
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

    APPEND(off, r, max, EXPLORER_HEADER("Block"));
    off += explorer_emit_nav((char *)r + off, max - off, "blocks");

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
    struct explorer_context *ctx = explorer_ctx();
    if (use_rpc_proxy())
        return serve_block_rpc(param, r, max);
    if (!ctx->main_state || !param || !param[0]) return 0;
    size_t off = 0;

    const struct block_index *bi = NULL;

    if (is_all_digits(param)) {
        int h = atoi(param);
        int tip = active_chain_height(&ctx->main_state->chain_active);
        if (h >= 0 && h <= tip)
            bi = active_chain_at(&ctx->main_state->chain_active, h);
    } else if (strlen(param) == 64 && is_all_hex(param, 64)) {
        struct uint256 hash;
        uint256_set_hex(&hash, param);
        bi = (const struct block_index *)block_map_find(
            &ctx->main_state->map_block_index, &hash);
    }

    if (!bi) {
        char safe_param[256];
        html_escape(safe_param, sizeof(safe_param), param ? param : "");
        return (size_t)snprintf((char *)r, max,
            "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
            "<!DOCTYPE html><html><head><link rel='stylesheet' href='/explorer/style.css'></head><body>"
            EXPLORER_NAV "<h2>Block Not Found</h2>"
            "<p>No block found for: <code>%s</code></p>"
            EXPLORER_FOOTER, safe_param);
    }

    int height = bi->nHeight;
    int tip = active_chain_height(&ctx->main_state->chain_active);

    char hash[65] = "";
    if (bi->phashBlock) uint256_get_hex(bi->phashBlock, hash);
    /* Read block from disk early to get header fields (merkle root, etc.)
     * The block_index mmap doesn't store these — only the full block has them. */
    struct block blk;
    block_init(&blk);
    bool loaded = ctx->datadir && read_block_from_disk_index(&blk, bi, ctx->datadir);

    char merkle[65], sapling_root[65], nonce[65];
    if (loaded) {
        uint256_get_hex(&blk.header.hashMerkleRoot, merkle);
        uint256_get_hex(&blk.header.hashFinalSaplingRoot, sapling_root);
        uint256_get_hex(&blk.header.nNonce, nonce);
    } else {
        uint256_get_hex(&bi->hashMerkleRoot, merkle);
        uint256_get_hex(&bi->hashFinalSaplingRoot, sapling_root);
        uint256_get_hex(&bi->nNonce, nonce);
    }

    char ts[32];
    format_time(ts, sizeof(ts), bi->nTime);
    char sap_val[32] = "0";
    format_zcl(sap_val, sizeof(sap_val), bi->nSaplingValue);
    char sprout_val[32] = "0";
    format_zcl(sprout_val, sizeof(sprout_val), bi->nSproutValue);

    APPEND(off, r, max, EXPLORER_HEADER("Block"));
    off += explorer_emit_nav((char *)r + off, max - off, "blocks");

    /* Navigation */
    {
        char prev_fmt[32], next_fmt[32], h_fmt[32], conf_fmt[32];
        format_with_commas(prev_fmt, sizeof(prev_fmt), height - 1);
        format_with_commas(next_fmt, sizeof(next_fmt), height + 1);
        format_with_commas(h_fmt, sizeof(h_fmt), height);
        format_with_commas(conf_fmt, sizeof(conf_fmt), tip - height + 1);

        APPEND(off, r, max, "<div class='pager'>");
        if (height > 0)
            APPEND(off, r, max, "<a href='/explorer/block/%d'>&laquo; Block %s</a>", height - 1, prev_fmt);
        if (height < tip)
            APPEND(off, r, max, "<a href='/explorer/block/%d'>Block %s &raquo;</a>", height + 1, next_fmt);
        APPEND(off, r, max, "</div>");

        APPEND(off, r, max,
            "<h2>Block %s</h2>"
            "<div class='card'><div class='grid'>"
            "<div class='label'>Hash</div><div class='val hash'>%s</div>"
            "<div class='label'>Height</div><div class='val'>%s</div>"
            "<div class='label'>Confirmations</div><div class='val'>%s</div>"
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
            h_fmt, hash, h_fmt, conf_fmt, ts, bi->nTx,
            get_difficulty(bi), merkle, sapling_root, nonce,
            bi->nBits, sap_val, sprout_val);
    }

    /* Block already loaded above for header fields */

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

            /* Combined transparent + shielded counts */
            size_t total_in = tx->num_vin + tx->num_shielded_spend +
                              tx->num_joinsplit;
            size_t total_out = tx->num_vout + tx->num_shielded_output +
                               tx->num_joinsplit;

            char idx_s[16], in_s[16], out_s[16];
            snprintf(idx_s, sizeof(idx_s), "%zu", i);
            snprintf(in_s, sizeof(in_s), "%zu", total_in);
            snprintf(out_s, sizeof(out_s), "%zu", total_out);

            struct template_var vars[] = {
                { "index",      idx_s },
                { "txid",       txid },
                { "short_txid", short_txid },
                { "type_tags",  type_tags },
                { "inputs",     in_s },
                { "outputs",    out_s },
                { "value",      val },
            };
            off += template_render(TMPL_EXPLORER_TX_ROW,
                                   vars, sizeof(vars)/sizeof(vars[0]),
                                   (char *)r + off, max - off);
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

    APPEND(off, r, max, EXPLORER_HEADER("Transaction"));
    off += explorer_emit_nav((char *)r + off, max - off, NULL);

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
        format_zcl(vb, sizeof(vb), (int64_t)(value_balance * (double)ZATOSHI_PER_ZCL));
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
            format_zcl(val_fmt, sizeof(val_fmt), (int64_t)(val * (double)ZATOSHI_PER_ZCL));

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
    struct explorer_context *ctx = explorer_ctx();
    if (use_rpc_proxy())
        return serve_tx_rpc(param, r, max);
    if (!ctx->main_state || !param || strlen(param) != 64 ||
        !is_all_hex(param, 64) || !explorer_param_is_printable_ascii(param))
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
    bool in_mempool = ctx->mempool &&
                      tx_mempool_lookup(ctx->mempool, &txhash, &tx);

    /* Try tx index */
    int block_height = -1;
    char block_hash_hex[65] = "";
    struct block blk;
    block_init(&blk);
    bool from_block = false;

    if (!in_mempool && ctx->node_db) {
        struct db_tx_index txi;
        if (db_tx_find(ctx->node_db, txhash.data, &txi)) {
            block_height = txi.block_height;

            /* Load block from disk */
            const struct block_index *bi =
                active_chain_at(&ctx->main_state->chain_active, block_height);
            if (bi && ctx->datadir && read_block_from_disk_index(&blk, bi, ctx->datadir)) {
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
        /* SQLite didn't have it — fall back to RPC (covers txindex gaps) */
        size_t rpc_result = serve_tx_rpc(param, r, max);
        if (rpc_result > 0) return rpc_result;
        char safe_param[256];
        html_escape(safe_param, sizeof(safe_param), param ? param : "");
        return (size_t)snprintf((char *)r, max,
            "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
            "<!DOCTYPE html><html><head><link rel='stylesheet' href='/explorer/style.css'></head><body>"
            EXPLORER_NAV "<h2>Transaction Not Found</h2>"
            "<p>TxID: <code>%s</code></p>"
            "<p style='color:#666'>Not in mempool or tx index.</p>" EXPLORER_FOOTER, safe_param);
    }

    size_t off = 0;
    int tip = active_chain_height(&ctx->main_state->chain_active);
    int confirmations = in_mempool ? 0 : (block_height >= 0 ? tip - block_height + 1 : 0);

    APPEND(off, r, max, EXPLORER_HEADER("Transaction"));
    off += explorer_emit_nav((char *)r + off, max - off, NULL);

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

    if (block_height >= 0) {
        char bh_fmt[32];
        format_with_commas(bh_fmt, sizeof(bh_fmt), block_height);
        APPEND(off, r, max,
            "<div class='label'>Block</div><div class='val'>"
            "<a href='/explorer/block/%d'>%s</a></div>",
            block_height, bh_fmt);
    }

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
            int64_t reward = block_height >= 0 ? get_block_subsidy(block_height, &chain_params_get()->consensus) : 0;
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

            /* Look up previous output value from tx_outputs table */
            char in_val[32] = "?";
            if (ctx->node_db && ctx->node_db->db) {
                sqlite3_stmt *vs = NULL;
                if (sqlite3_prepare_v2(ctx->node_db->db,
                        "SELECT value FROM tx_outputs WHERE txid=? AND vout=?",
                        -1, &vs, NULL) == SQLITE_OK && vs) {
                    sqlite3_bind_blob(vs, 1, tx.vin[i].prevout.hash.data, 32, SQLITE_STATIC);
                    sqlite3_bind_int(vs, 2, (int)tx.vin[i].prevout.n);
                    if (sqlite3_step(vs) == SQLITE_ROW) {
                        int64_t prev_val = sqlite3_column_int64(vs, 0);
                        format_zcl(in_val, sizeof(in_val), prev_val);
                    }
                    sqlite3_finalize(vs);
                }
            }

            if (in_val[0] != '?') {
                APPEND(off, r, max,
                    "<div class='io-row'>"
                    "<div class='io-idx'>%zu</div>"
                    "<div class='io-addr'><a href='/explorer/tx/%s'>%s</a>:%u</div>"
                    "<div class='io-val'>%s ZCL</div></div>",
                    i, prev_hash, prev_short, tx.vin[i].prevout.n, in_val);
            } else {
                APPEND(off, r, max,
                    "<div class='io-row'>"
                    "<div class='io-idx'>%zu</div>"
                    "<div class='io-addr'><a href='/explorer/tx/%s'>%s</a>:%u</div>"
                    "<div class='io-val' style='color:#666'>?</div></div>",
                    i, prev_hash, prev_short, tx.vin[i].prevout.n);
            }
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
    struct explorer_context *ctx = explorer_ctx();
    size_t param_len = param ? strlen(param) : 0;
    if (!ctx->main_state || !param || !param[0] || param_len >= 128 ||
        !explorer_param_is_printable_ascii(param))
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

    APPEND(off, r, max, EXPLORER_HEADER("Address"));
    off += explorer_emit_nav((char *)r + off, max - off, NULL);

    APPEND(off, r, max,
        "<h2>Address</h2>"
        "<div class='card'><div class='grid'>"
        "<div class='label'>Address</div><div class='val hash'>%s</div>"
        "<div class='label'>Type</div><div class='val'>%s</div>",
        safe_addr,
        dest.type == DEST_KEY_ID ? "P2PKH (Pay-to-PubKey-Hash)" : "P2SH (Pay-to-Script-Hash)");

    if (ctx->node_db && addr_hash) {
        int64_t balance = db_utxo_balance_for_address(ctx->node_db, addr_hash);
        char bal[32];
        format_zcl(bal, sizeof(bal), balance);
        APPEND(off, r, max,
            "<div class='label'>Balance</div><div class='val amount'>%s ZCL</div>",
            bal);
    }
    APPEND(off, r, max, "</div></div>");

    /* UTXO list */
    if (ctx->node_db && addr_hash) {
        struct db_utxo utxos[100];
        int count = db_utxo_list_for_address(ctx->node_db, addr_hash, utxos, 100);

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
    struct explorer_context *ctx = explorer_ctx();
    if (!query) return 0;

    /* URL-decode the query ('+' → space, %XX → byte) */
    char decoded[256];
    {
        size_t di = 0;
        for (size_t si = 0; query[si] && di < sizeof(decoded) - 1; si++) {
            if (query[si] == '%' && query[si+1] && query[si+2]) {
                char hex[3] = { query[si+1], query[si+2], '\0' };
                char *endp = NULL;
                long v = strtol(hex, &endp, 16);
                if (!endp || *endp != '\0' || v < 0 || v > 255)
                    return (size_t)snprintf((char *)r, max,
                        "HTTP/1.1 400 Bad Request\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
                        "<!DOCTYPE html><html><head><link rel='stylesheet' href='/explorer/style.css'></head><body>"
                        EXPLORER_NAV "<h2>Invalid Search Query</h2>"
                        "<p>Malformed percent-encoding in query.</p>" EXPLORER_FOOTER);
                decoded[di++] = (char)v;
                si += 2;
            } else if (query[si] == '+') {
                decoded[di++] = ' ';
            } else {
                decoded[di++] = query[si];
            }
        }
        decoded[di] = '\0';
    }

    /* Strip leading/trailing whitespace */
    const char *dq = decoded;
    while (*dq == ' ') dq++;
    size_t qlen = strlen(dq);
    char q[256];
    if (qlen >= sizeof(q)) qlen = sizeof(q) - 1;
    memcpy(q, dq, qlen);
    q[qlen] = '\0';
    while (qlen > 0 && q[qlen - 1] == ' ') q[--qlen] = '\0';

    if (!qlen) return serve_dashboard(r, max);
    if (!explorer_param_is_printable_ascii(q))
        return (size_t)snprintf((char *)r, max,
            "HTTP/1.1 400 Bad Request\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
            "<!DOCTYPE html><html><head><link rel='stylesheet' href='/explorer/style.css'></head><body>"
            EXPLORER_NAV "<h2>Invalid Search Query</h2>"
            "<p>Search input contains unsupported characters.</p>" EXPLORER_FOOTER);

    /* Block height? Always try — serve_block handles RPC fallback */
    if (is_all_digits(q)) {
        int h = atoi(q);
        if (h >= 0 && h < 100000000)
            return serve_block(q, r, max);
    }

    /* 64-hex: try as block hash first, then txid */
    if (qlen == 64 && is_all_hex(q, 64)) {
        /* Try block hash via native index */
        if (ctx->main_state) {
            struct uint256 hash;
            uint256_set_hex(&hash, q);
            const struct block_index *bi = block_map_find(
                &ctx->main_state->map_block_index, &hash);
            if (bi)
                return serve_block(q, r, max);
        }

        /* Try txid via SQLite index */
        if (ctx->node_db) {
            struct uint256 hash;
            uint256_set_hex(&hash, q);
            struct db_tx_index txi;
            if (db_tx_find(ctx->node_db, hash.data, &txi))
                return serve_tx(q, r, max);
        }

        /* Try mempool */
        if (ctx->mempool) {
            struct uint256 hash;
            uint256_set_hex(&hash, q);
            if (tx_mempool_exists(ctx->mempool, &hash))
                return serve_tx(q, r, max);
        }

        /* Fallback: try as tx via RPC, then block hash via RPC */
        {
            char rpc_buf[1024];
            char rpc_params[128];
            snprintf(rpc_params, sizeof(rpc_params), "[\"%s\", 1]", q);
            int rn = rpc_call("getrawtransaction", rpc_params,
                              rpc_buf, sizeof(rpc_buf));
            if (rn > 0 && strstr(rpc_buf, "\"error\":null"))
                return serve_tx(q, r, max);
        }
        return serve_block(q, r, max);
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
    APPEND(off, r, max, EXPLORER_HEADER("Search"));
    off += explorer_emit_nav((char *)r + off, max - off, NULL);
    APPEND(off, r, max,
        "<h2>Search Results</h2>"
        "<div class='card'>"
        "<p>No results for: <code>%s</code></p>"
        "<p style='color:#666'>Try a block height, block hash, transaction ID, or address.</p>"
        "</div>" EXPLORER_FOOTER, safe);
    return off;
}

/* ── Stats Page with SVG Charts ────────────────────────────── */

/* format_y_label and svg_line_chart moved to explorer_internal.h */

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
#define STATS_CACHE_SIZE (1024 * 1024) /* 1MB for comprehensive stats */
static char g_stats_cache[STATS_CACHE_SIZE] = "";
static size_t g_stats_cache_len = 0;

/* ── Disk cache helpers (survive restarts) ────────────────── */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"

static void cache_save(const char *name, const char *data, size_t len)
{
    struct explorer_assets *assets = explorer_assets();
    if (!assets->explorer_dir[0]) ensure_explorer_dir();
    if (!assets->explorer_dir[0] || len == 0) return;
    char path[1200];
    snprintf(path, sizeof(path), "%s/%s.cache", assets->explorer_dir, name);
    FILE *f = fopen(path, "w");
    if (f) { fwrite(data, 1, len, f); fclose(f); }
}

static size_t cache_load(const char *name, char *buf, size_t max)
{
    struct explorer_assets *assets = explorer_assets();
    if (!assets->explorer_dir[0]) ensure_explorer_dir();
    if (!assets->explorer_dir[0]) return 0;
    char path[1200];
    snprintf(path, sizeof(path), "%s/%s.cache", assets->explorer_dir, name);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t len = fread(buf, 1, max - 1, f);
    fclose(f);
    buf[len] = '\0';
    return len;
}

#pragma GCC diagnostic pop

static void *stats_compute_thread(void *arg)
{
    (void)arg;
    struct explorer_context *ctx = explorer_ctx();
    /* Load previous cache from disk for instant serving while recomputing */
    if (g_stats_cache_len == 0) {
        size_t disk_len = cache_load("stats", g_stats_cache, STATS_CACHE_SIZE);
        if (disk_len > 0) {
            g_stats_cache_len = disk_len;
            printf("Stats: loaded %zu bytes from disk cache (instant)\n", disk_len);
            fflush(stdout);
        }
    }
    printf("Stats background: computing comprehensive stats...\n");
    fflush(stdout);
    /* Compute fresh into a temp buffer so we don't blank the disk-loaded cache */
    char *tmp = malloc(STATS_CACHE_SIZE);
    if (!tmp) { g_stats_computing = 0; return NULL; }
    size_t len = explorer_stats_build((uint8_t *)tmp, STATS_CACHE_SIZE, ctx->datadir);
    if (len > 0) {
        memcpy(g_stats_cache, tmp, len);
        g_stats_cache_len = len;
        cache_save("stats", g_stats_cache, len);
    }
    free(tmp);
    g_stats_computing = 0;
    return NULL;
}

/* (stats_query_int64, stats_query_double, stats_tab_css, and stats body
 * moved to explorer_stats.c — see explorer_stats_build()) */

static size_t serve_stats(uint8_t *r, size_t max)
{
    /* Return cached version if available */
    if (g_stats_cache_len > 0) {
        size_t copy = g_stats_cache_len < max ? g_stats_cache_len : max;
        memcpy(r, g_stats_cache, copy);
        return copy;
    }

    /* Not cached yet — trigger background computation if not running */
    explorer_start_once(&g_stats_computing, stats_compute_thread,
                        "stats_compute");
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

/* ── Factoids Page ────────────────────────────────────────── */

#define FACTOIDS_CACHE_SIZE (1024 * 1024)  /* 1MB — 17 sections with SHA3 */
static char g_factoids_cache[FACTOIDS_CACHE_SIZE] = "";
static size_t g_factoids_cache_len = 0;

static void *factoids_compute_thread(void *arg)
{
    (void)arg;
    struct explorer_context *ctx = explorer_ctx();
    /* Load previous cache from disk for instant serving */
    if (g_factoids_cache_len == 0) {
        size_t disk_len = cache_load("factoids", g_factoids_cache, FACTOIDS_CACHE_SIZE);
        if (disk_len > 0) {
            g_factoids_cache_len = disk_len;
            printf("Factoids: loaded %zu bytes from disk cache (instant)\n", disk_len);
            fflush(stdout);
        }
    }
    printf("Factoids background: computing historian data...\n");
    fflush(stdout);
    size_t len = explorer_factoids_build((uint8_t *)g_factoids_cache,
                                          FACTOIDS_CACHE_SIZE, ctx->datadir);
    if (len > 0) {
        g_factoids_cache_len = len;
        cache_save("factoids", g_factoids_cache, len);
    }
    g_factoids_computing = 0;
    return NULL;
}

static size_t serve_factoids(uint8_t *r, size_t max)
{
    /* Return cached version if available */
    if (g_factoids_cache_len > 0) {
        size_t copy = g_factoids_cache_len < max ? g_factoids_cache_len : max;
        memcpy(r, g_factoids_cache, copy);
        return copy;
    }

    /* Not cached yet -- trigger background computation */
    explorer_start_once(&g_factoids_computing, factoids_compute_thread,
                        "factoids_compute");
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
        "<h1 style='font-size:36px;color:#33ff99'>Loading Factoids...</h1>"
        "<p style='font-size:20px;color:#888'>Computing historian data from blockchain.</p>"
        "<p style='font-size:16px;color:#555'>Auto-refreshing every 3 seconds...</p>"
        "</div>" EXPLORER_FOOTER);
    return off;
}

/* ── ZSLP Tokens Page ─────────────────────────────────────── */

/* Tokens page cache — precomputed in background */
static char g_tokens_cache[131072] = "";
static size_t g_tokens_cache_len = 0;

static void *tokens_compute_thread(void *arg)
{
    (void)arg;
    struct explorer_context *ctx = explorer_ctx();
    printf("Tokens background: computing...\n");
    fflush(stdout);

    char db_path[1024];
    snprintf(db_path, sizeof(db_path), "%s/node.db", ctx->datadir);
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        g_tokens_computing = 0;
        return NULL;
    }
    sqlite3_exec(db, "PRAGMA mmap_size=268435456", NULL, NULL, NULL);

    uint8_t *r = malloc(131072);
    if (!r) { sqlite3_close(db); g_tokens_computing = 0; return NULL; }
    size_t max = 131072;
    size_t off = 0;

    APPEND(off, r, max, EXPLORER_HEADER("ZSLP Tokens"));
    off += explorer_emit_nav((char *)r + off, max - off, "tokens");

    /* Count tokens and transfers */
    struct explorer_token_stats token_stats = {0};
    explorer_query_token_stats(db, &token_stats);
    int64_t token_count = token_stats.token_count;
    int64_t xfer_count = token_stats.transfer_count;

    APPEND(off, r, max,
        "<h1>ZSLP Tokens</h1>"
        "<div class='stats-row'>"
        "<div class='stat'><div class='num'>%" PRId64 "</div><div class='lbl'>Tokens Created</div></div>"
        "<div class='stat'><div class='num'>%" PRId64 "</div><div class='lbl'>Token Transfers</div></div>"
        "</div>"
        "<p style='color:#aaa;font-size:16px'>"
        "Simple Ledger Protocol (ZSLP) tokens on the ZClassic blockchain.</p>",
        token_count, xfer_count);

    /* Token list from SQLite */
    APPEND(off, r, max,
        "<h2>All Tokens (%" PRId64 ")</h2>"
        "<table><tr><th>Ticker</th><th>Name</th><th>Decimals</th>"
        "<th>Supply</th><th>Block</th></tr>",
        token_count);
    {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT ticker, name, decimals, total_minted, genesis_height, hex(token_id)"
                " FROM zslp_tokens ORDER BY genesis_height LIMIT 100",
                -1, &s, NULL) == SQLITE_OK) {
            while (sqlite3_step(s) == SQLITE_ROW && off + 512 < max) {
                const char *ticker = (const char *)sqlite3_column_text(s, 0);
                const char *name = (const char *)sqlite3_column_text(s, 1);
                int dec = sqlite3_column_int(s, 2);
                int64_t minted = sqlite3_column_int64(s, 3);
                int height = sqlite3_column_int(s, 4);
                const char *tid_hex = (const char *)sqlite3_column_text(s, 5);

                char safe_ticker[128] = "", safe_name[256] = "";
                html_escape(safe_ticker, sizeof(safe_ticker), ticker ? ticker : "");
                html_escape(safe_name, sizeof(safe_name), name ? name : "");

                /* Format supply with decimals */
                char supply[64];
                if (dec > 0 && dec <= 8) {
                    int64_t divisor = 1;
                    for (int d = 0; d < dec; d++) divisor *= 10;
                    snprintf(supply, sizeof(supply), "%" PRId64 ".%0*" PRId64,
                             minted / divisor, dec, minted % divisor);
                } else {
                    snprintf(supply, sizeof(supply), "%" PRId64, minted);
                }

                /* Build linkable token ID (reverse byte order for display) */
                char tid_link[65] = "";
                if (tid_hex && strlen(tid_hex) == 64) {
                    for (int k = 0; k < 32; k++) {
                        tid_link[k*2] = tid_hex[62-k*2];
                        tid_link[k*2+1] = tid_hex[63-k*2];
                    }
                    tid_link[64] = '\0';
                    /* lowercase */
                    for (int k = 0; k < 64; k++)
                        if (tid_link[k] >= 'A' && tid_link[k] <= 'F')
                            tid_link[k] += 32;
                }

                APPEND(off, r, max,
                    "<tr><td style='font-size:18px'>"
                    "<a href='/explorer/token/%s' style='color:#ff99ff;font-weight:700'>%s</a></td>"
                    "<td>%s</td>"
                    "<td>%d</td>"
                    "<td class='amount'>%s</td>"
                    "<td><a href='/explorer/block/%d'>%d</a></td></tr>",
                    tid_link, safe_ticker[0] ? safe_ticker : "(none)",
                    safe_name, dec, supply, height, height);
            }
            sqlite3_finalize(s);
        }
    }
    APPEND(off, r, max, "</table>");

    /* Recent transfers */
    APPEND(off, r, max,
        "<h2>Recent Transfers</h2>"
        "<table><tr><th>Block</th><th>Type</th><th>Token</th>"
        "<th>Amount</th></tr>");
    {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT x.block_height, x.tx_type, x.amount, t.ticker, t.decimals "
                "FROM zslp_transfers x "
                "LEFT JOIN zslp_tokens t ON x.token_id = t.token_id "
                "ORDER BY x.block_height DESC LIMIT 50",
                -1, &s, NULL) == SQLITE_OK) {
            while (sqlite3_step(s) == SQLITE_ROW && off + 256 < max) {
                int height = sqlite3_column_int(s, 0);
                int tx_type = sqlite3_column_int(s, 1);
                int64_t amount = sqlite3_column_int64(s, 2);
                const char *ticker = (const char *)sqlite3_column_text(s, 3);
                int dec = sqlite3_column_int(s, 4);

                const char *type_str = tx_type == 1 ? "GENESIS" :
                                       tx_type == 2 ? "MINT" :
                                       tx_type == 3 ? "SEND" : "?";
                const char *type_class = tx_type == 1 ? "tag-cb" :
                                         tx_type == 2 ? "tag-shielded" : "tag-slp";

                char amt[64];
                if (dec > 0 && dec <= 8) {
                    int64_t divisor = 1;
                    for (int d = 0; d < dec; d++) divisor *= 10;
                    snprintf(amt, sizeof(amt), "%" PRId64 ".%0*" PRId64,
                             amount / divisor, dec, amount % divisor);
                } else {
                    snprintf(amt, sizeof(amt), "%" PRId64, amount);
                }

                char safe_ticker[64] = "";
                html_escape(safe_ticker, sizeof(safe_ticker), ticker ? ticker : "");

                APPEND(off, r, max,
                    "<tr><td><a href='/explorer/block/%d'>%d</a></td>"
                    "<td><span class='tag %s'>%s</span></td>"
                    "<td style='color:#ff99ff'>%s</td>"
                    "<td class='amount'>%s</td></tr>",
                    height, height, type_class, type_str,
                    safe_ticker[0] ? safe_ticker : "?", amt);
            }
            sqlite3_finalize(s);
        }
    }
    APPEND(off, r, max, "</table>");

    /* About section */
    APPEND(off, r, max,
        "<h2>About ZSLP</h2>"
        "<div class='card'>"
        "<p style='font-size:16px;line-height:1.8'>"
        "ZSLP (ZClassic Simple Ledger Protocol) enables custom tokens on the ZClassic blockchain. "
        "Based on the SLP specification, tokens are encoded in "
        "OP_RETURN outputs with no consensus changes required.</p>"
        "<div class='grid' style='margin-top:12px'>"
        "<div class='label'>GENESIS</div><div class='val'>Create a new token (ticker, name, supply, decimals)</div>"
        "<div class='label'>SEND</div><div class='val'>Transfer tokens between addresses</div>"
        "<div class='label'>MINT</div><div class='val'>Create additional supply (if baton exists)</div>"
        "<div class='label'>Token ID</div><div class='val'>The GENESIS transaction hash uniquely identifies each token</div>"
        "<div class='label'>Lokad ID</div><div class='val'><code>SLP\\x00</code> (0x534c5000) in OP_RETURN</div>"
        "</div></div>");

    APPEND(off, r, max, EXPLORER_FOOTER);

    /* Cache result */
    if (off > 0 && off < sizeof(g_tokens_cache)) {
        memcpy(g_tokens_cache, r, off);
        g_tokens_cache_len = off;
    }
    free(r);
    sqlite3_close(db);
    g_tokens_computing = 0;
    printf("Tokens background: cached %zu bytes (%" PRId64 " tokens)\n",
           g_tokens_cache_len, token_count);
    fflush(stdout);
    return NULL;
}

static size_t serve_tokens(uint8_t *r, size_t max)
{
    if (g_tokens_cache_len > 0) {
        size_t copy = g_tokens_cache_len < max ? g_tokens_cache_len : max;
        memcpy(r, g_tokens_cache, copy);
        return copy;
    }
    if (!g_tokens_computing) {
        g_tokens_computing = 1;
        explorer_start_once(&g_tokens_computing, tokens_compute_thread,
                            "tokens_compute");
    }
    size_t off = 0;
    APPEND(off, r, max,
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n\r\n<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta http-equiv='refresh' content='3'>"
        "<link rel='stylesheet' href='/explorer/style.css'>"
        "</head><body>" EXPLORER_NAV
        "<div style='text-align:center;margin:80px 0'>"
        "<h1 style='font-size:32px;color:#ff99ff'>Loading Token Data...</h1>"
        "<p style='font-size:18px;color:#888'>Scanning SQLite for ZSLP tokens.</p>"
        "</div>" EXPLORER_FOOTER);
    return off;
}

/* ── ZSLP Token Detail Page ────────────────────────────────── */

static size_t serve_token_detail(const char *token_id_hex, uint8_t *r, size_t max)
{
    struct explorer_context *ctx = explorer_ctx();
    if (!token_id_hex || strlen(token_id_hex) != 64 || !ctx->datadir ||
        !is_all_hex(token_id_hex, 64) || !explorer_param_is_printable_ascii(token_id_hex))
        return 0;

    /* Open our own SQLite connection (called from HTTPS thread) */
    char db_path[1024];
    snprintf(db_path, sizeof(db_path), "%s/node.db", ctx->datadir);
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
        return 0;
    sqlite3_exec(db, "PRAGMA mmap_size=268435456", NULL, NULL, NULL);

    /* Parse hex token ID — try direct first, then reversed byte order */
    uint8_t token_id[32];
    uint8_t token_id_rev[32];
    for (int i = 0; i < 32; i++) {
        unsigned int b;
        if (sscanf(token_id_hex + i * 2, "%2x", &b) != 1) {
            sqlite3_close(db);
            return 0;
        }
        token_id[i] = (uint8_t)b;
        token_id_rev[31 - i] = (uint8_t)b;
    }

    /* Look up token — try both byte orders */
    char ticker[64] = "", name[128] = "", doc_url[256] = "";
    int decimals = 0, genesis_height = 0;
    int64_t total_minted = 0;
    bool found = false;

    /* Try direct byte order first, then reversed */
    const uint8_t *lookup_id = token_id;
    {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT ticker, name, decimals, document_url, genesis_height, total_minted "
                "FROM zslp_tokens WHERE token_id = ?",
                -1, &s, NULL) == SQLITE_OK) {
            sqlite3_bind_blob(s, 1, token_id, 32, SQLITE_STATIC);
            if (sqlite3_step(s) != SQLITE_ROW) {
                /* Try reversed */
                sqlite3_reset(s);
                sqlite3_bind_blob(s, 1, token_id_rev, 32, SQLITE_STATIC);
                if (sqlite3_step(s) == SQLITE_ROW)
                    lookup_id = token_id_rev;
            }
            if (sqlite3_column_text(s, 0)) {
                const char *t = (const char *)sqlite3_column_text(s, 0);
                const char *n = (const char *)sqlite3_column_text(s, 1);
                const char *u = (const char *)sqlite3_column_text(s, 3);
                if (t) snprintf(ticker, sizeof(ticker), "%s", t);
                if (n) snprintf(name, sizeof(name), "%s", n);
                if (u) snprintf(doc_url, sizeof(doc_url), "%s", u);
                decimals = sqlite3_column_int(s, 2);
                genesis_height = sqlite3_column_int(s, 4);
                total_minted = sqlite3_column_int64(s, 5);
                found = true;
            }
            sqlite3_finalize(s);
        }
    }

    if (!found) {
        sqlite3_close(db);
        return (size_t)snprintf((char *)r, max,
            "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
            "<!DOCTYPE html><html><head><link rel='stylesheet' href='/explorer/style.css'></head><body>"
            EXPLORER_NAV "<h2>Token Not Found</h2>"
            "<p>No ZSLP token with ID: <code>%s</code></p>" EXPLORER_FOOTER,
            token_id_hex);
    }

    size_t off = 0;
    char safe_ticker[128], safe_name[256], safe_url[512];
    html_escape(safe_ticker, sizeof(safe_ticker), ticker);
    html_escape(safe_name, sizeof(safe_name), name);
    html_escape(safe_url, sizeof(safe_url), doc_url);

    char supply[64];
    if (decimals > 0 && decimals <= 8) {
        int64_t divisor = 1;
        for (int d = 0; d < decimals; d++) divisor *= 10;
        snprintf(supply, sizeof(supply), "%" PRId64 ".%0*" PRId64,
                 total_minted / divisor, decimals, total_minted % divisor);
    } else {
        snprintf(supply, sizeof(supply), "%" PRId64, total_minted);
    }

    /* Count transfers for this token */
    int64_t xfer_count = 0;
    {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT count(*) FROM zslp_transfers WHERE token_id = ?",
                -1, &s, NULL) == SQLITE_OK) {
            sqlite3_bind_blob(s, 1, lookup_id, 32, SQLITE_STATIC);
            if (sqlite3_step(s) == SQLITE_ROW)
                xfer_count = sqlite3_column_int64(s, 0);
            sqlite3_finalize(s);
        }
    }

    APPEND(off, r, max, EXPLORER_HEADER("Token"));
    off += explorer_emit_nav((char *)r + off, max - off, "tokens");

    /* Token header */
    APPEND(off, r, max,
        "<h1 style='color:#ff99ff'>%s</h1>"
        "<h2>%s</h2>"
        "<div class='card'><div class='grid'>"
        "<div class='label'>Token ID</div><div class='val hash' style='font-size:13px'>%s</div>"
        "<div class='label'>Ticker</div><div class='val' style='color:#ff99ff;font-weight:700;font-size:20px'>%s</div>"
        "<div class='label'>Name</div><div class='val'>%s</div>"
        "<div class='label'>Decimals</div><div class='val'>%d</div>"
        "<div class='label'>Total Supply</div><div class='val amount' style='font-size:20px'>%s</div>"
        "<div class='label'>Genesis Block</div><div class='val'><a href='/explorer/block/%d'>%d</a></div>"
        "<div class='label'>Genesis TX</div><div class='val hash'><a href='/explorer/tx/%s'>%s</a></div>"
        "<div class='label'>Transfers</div><div class='val'>%" PRId64 "</div>",
        safe_ticker[0] ? safe_ticker : "(unnamed)",
        safe_name[0] ? safe_name : "ZSLP Token",
        token_id_hex,
        safe_ticker[0] ? safe_ticker : "(none)",
        safe_name, decimals, supply,
        genesis_height, genesis_height,
        token_id_hex, token_id_hex,
        xfer_count);

    if (safe_url[0])
        APPEND(off, r, max,
            "<div class='label'>Document URL</div><div class='val'>%s</div>",
            safe_url);

    APPEND(off, r, max, "</div></div>");

    /* Transfer history */
    APPEND(off, r, max,
        "<h2>Transfer History (%" PRId64 ")</h2>"
        "<table><tr><th>Block</th><th>Type</th><th>Amount</th></tr>",
        xfer_count);
    {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT block_height, tx_type, amount, hex(txid) "
                "FROM zslp_transfers WHERE token_id = ? "
                "ORDER BY block_height DESC LIMIT 100",
                -1, &s, NULL) == SQLITE_OK) {
            sqlite3_bind_blob(s, 1, lookup_id, 32, SQLITE_STATIC);
            while (sqlite3_step(s) == SQLITE_ROW && off + 512 < max) {
                int height = sqlite3_column_int(s, 0);
                int tx_type = sqlite3_column_int(s, 1);
                int64_t amount = sqlite3_column_int64(s, 2);
                const char *txid_hex = (const char *)sqlite3_column_text(s, 3);

                const char *type_str = tx_type == 1 ? "GENESIS" :
                                       tx_type == 2 ? "MINT" :
                                       tx_type == 3 ? "SEND" : "?";
                const char *type_class = tx_type == 1 ? "tag-cb" :
                                         tx_type == 2 ? "tag-shielded" : "tag-slp";

                char amt[64];
                if (decimals > 0 && decimals <= 8) {
                    int64_t divisor = 1;
                    for (int d = 0; d < decimals; d++) divisor *= 10;
                    snprintf(amt, sizeof(amt), "%" PRId64 ".%0*" PRId64,
                             amount / divisor, decimals, amount % divisor);
                } else {
                    snprintf(amt, sizeof(amt), "%" PRId64, amount);
                }

                /* Reverse txid for display */
                char txid_disp[65] = "";
                if (txid_hex && strlen(txid_hex) == 64) {
                    for (int k = 0; k < 32; k++) {
                        txid_disp[k*2] = txid_hex[62-k*2];
                        txid_disp[k*2+1] = txid_hex[63-k*2];
                    }
                    txid_disp[64] = '\0';
                    for (int k = 0; k < 64; k++)
                        if (txid_disp[k] >= 'A' && txid_disp[k] <= 'F')
                            txid_disp[k] += 32;
                }
                char short_tx[18];
                snprintf(short_tx, sizeof(short_tx), "%.8s...%.4s",
                         txid_disp, txid_disp + 60);

                APPEND(off, r, max,
                    "<tr><td><a href='/explorer/block/%d'>%d</a></td>"
                    "<td><span class='tag %s'>%s</span></td>"
                    "<td class='amount'>%s</td></tr>",
                    height, height, type_class, type_str, amt);
            }
            sqlite3_finalize(s);
        }
    }
    APPEND(off, r, max, "</table>");

    APPEND(off, r, max, EXPLORER_FOOTER);
    sqlite3_close(db);
    return off;
}

/* ── 9-Year HODL Wave Chart ───────────────────────────────── */

/* Cache — computed in background, served instantly */
static char g_hodl_cache[65536] = "";
static size_t g_hodl_cache_len = 0;


static size_t serve_hodl(uint8_t *r, size_t max)
{
    /* Return cached version if available (cache built by background thread) */
    if (g_hodl_cache_len > 0) {
        size_t copy = g_hodl_cache_len < max ? g_hodl_cache_len : max;
        memcpy(r, g_hodl_cache, copy);
        return copy;
    }

    /* If not cached and not computing, start background computation */
    if (!g_hodl_computing) {
        g_hodl_computing = 1;
        explorer_start_once(&g_hodl_computing, hodl_compute_thread,
                            "hodl_compute");
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
    struct explorer_context *ctx = explorer_ctx();
    printf("HODL background: starting computation...\n");
    fflush(stdout);

    /* We need our own SQLite connection for the background thread */
    char db_path[1024];
    snprintf(db_path, sizeof(db_path), "%s/node.db", ctx->datadir);
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

    /* Height-based HODL wave computation.
     * Uses fixed block spacing (150s pre-Buttercup, 75s post) to convert
     * between heights and time. No dependency on blocks.time column.
     * Same approach as the working gethodlwave RPC. */

    #define BUTTERCUP_HEIGHT  707000
    #define PRE_SPACING       150
    #define POST_SPACING      75

    /* Convert a timestamp to approximate block height */
    #define TS_TO_HEIGHT(ts) ( \
        ((ts) <= genesis_ts) ? 0 : \
        ((int64_t)((ts) - genesis_ts) < (int64_t)BUTTERCUP_HEIGHT * PRE_SPACING) \
            ? (int)(((ts) - genesis_ts) / PRE_SPACING) \
            : (int)(BUTTERCUP_HEIGHT + (((ts) - genesis_ts) - \
                    (int64_t)BUTTERCUP_HEIGHT * PRE_SPACING) / POST_SPACING))

    /* Build checkpoint list: for each monthly sample, compute the height
     * at that time and the height 1 year earlier. */
    int64_t cp_time[120];
    int cp_height[120];
    int cp_old_height[120];

    for (int i = 0; i < npts; i++) {
        cp_time[i] = start_ts + (int64_t)i * (now_ts - start_ts) / (npts - 1);
        cp_height[i] = TS_TO_HEIGHT(cp_time[i]);
        if (cp_height[i] > tip) cp_height[i] = tip;
        int64_t old_time = cp_time[i] - one_year;
        if (old_time <= genesis_ts)
            cp_old_height[i] = -1; /* no coins exist 1yr before this point */
        else
            cp_old_height[i] = TS_TO_HEIGHT(old_time);

        /* Generate label — year for January */
        time_t t = (time_t)cp_time[i];
        struct tm tm;
        gmtime_r(&t, &tm);
        snprintf(labels[i], sizeof(labels[i]), "%d", tm.tm_year + 1900);
        if (tm.tm_mon != 0 && i != 0) labels[i][0] = '\0';
    }

    /* Aggregate UTXOs into height buckets, build cumulative sum.
     * Filter out garbage heights (> tip + 1000). */
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
            "WHERE height <= %d "
            "GROUP BY height/%d ORDER BY 1",
            BUCKET_SIZE, BUCKET_SIZE, tip + 1000, BUCKET_SIZE);

        if (sqlite3_prepare_v2(db, sql_buf, -1, &stmt, NULL) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
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

        /* Build cumulative sum: cumsum[i] = sum of buckets 0..(i-1) */
        int64_t cumsum[MAX_BUCKETS + 1];
        cumsum[0] = 0;
        for (int i = 0; i < bucket_count; i++)
            cumsum[i + 1] = cumsum[i] + bucket_vals[i];

        /* For each checkpoint, compute % of value older than 1 year */
        for (int i = 0; i < npts; i++) {
            int at_idx = cp_height[i] >= 0 ? cp_height[i] / BUCKET_SIZE + 1 : 0;
            int old_idx = cp_old_height[i] >= 0 ? cp_old_height[i] / BUCKET_SIZE + 1 : 0;
            if (at_idx > bucket_count) at_idx = bucket_count;
            if (old_idx > bucket_count) old_idx = bucket_count;
            if (at_idx < 0) at_idx = 0;
            if (old_idx < 0) old_idx = 0;

            int64_t total_val = cumsum[at_idx];
            int64_t old_val = cumsum[old_idx];

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

    APPEND(off, r, max, EXPLORER_HEADER("HODL Wave"));
    off += explorer_emit_nav((char *)r + off, max - off, "hodl");

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
            "<text x='%d' y='%d' fill='#888' font-size='14' "
            "font-family='Georgia,serif' text-anchor='end'>%d%%</text>",
            pad_l - 10, y + 5, g * 25);
    }

    /* Y-axis title */
    APPEND(off, r, max,
        "<text x='16' y='%d' fill='#aaa' font-size='13' "
        "font-family='Georgia,serif' "
        "transform='rotate(-90,16,%d)' text-anchor='middle'>"
        "Coins Unmoved &gt;1 Year</text>",
        pad_t + plot_h / 2, pad_t + plot_h / 2);

    /* Gradient fill */
    APPEND(off, r, max,
        "<defs><linearGradient id='hodlGrad' x1='0' y1='0' x2='0' y2='1'>"
        "<stop offset='0%%' stop-color='#8844ff' stop-opacity='0.6'/>"
        "<stop offset='100%%' stop-color='#8844ff' stop-opacity='0.05'/>"
        "</linearGradient></defs>");

    /* Filled area */
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
        "<polyline fill='none' stroke='#aa66ff' stroke-width='2.5' "
        "stroke-linejoin='round' points='");
    for (int i = 0; i < npts; i++) {
        int x = pad_l + plot_w * i / (npts - 1);
        int y = pad_t + plot_h - (int)(pct_over_1yr[i] / 100.0 * plot_h);
        APPEND(off, r, max, "%d,%d ", x, y);
    }
    APPEND(off, r, max, "'/>");

    /* Data points with hover circles + invisible hit areas for tooltip */
    for (int i = 0; i < npts; i++) {
        int x = pad_l + plot_w * i / (npts - 1);
        int y = pad_t + plot_h - (int)(pct_over_1yr[i] / 100.0 * plot_h);
        time_t t = (time_t)cp_time[i];
        struct tm tm;
        gmtime_r(&t, &tm);
        char date_str[32];
        snprintf(date_str, sizeof(date_str), "%04d-%02d",
                 tm.tm_year + 1900, tm.tm_mon + 1);
        /* Small dot at each data point */
        APPEND(off, r, max,
            "<circle cx='%d' cy='%d' r='2' fill='#aa66ff' opacity='0.5'/>",
            x, y);
        /* Invisible wider hit area with SVG <title> tooltip */
        int hit_w = plot_w / npts;
        if (hit_w < 8) hit_w = 8;
        APPEND(off, r, max,
            "<rect x='%d' y='%d' width='%d' height='%d' fill='transparent' "
            "class='hodl-pt' data-x='%d' data-y='%d' data-pct='%.1f' data-date='%s'>"
            "<title>%s: %.1f%% unmoved &gt;1yr (h=%d)</title></rect>",
            x - hit_w/2, pad_t, hit_w, plot_h,
            x, y, pct_over_1yr[i], date_str,
            date_str, pct_over_1yr[i], cp_height[i]);
    }

    /* Current value dot + label */
    {
        int x = pad_l + plot_w;
        int y = pad_t + plot_h - (int)(current_pct / 100.0 * plot_h);
        APPEND(off, r, max,
            "<circle cx='%d' cy='%d' r='5' fill='#aa66ff'/>"
            "<text x='%d' y='%d' fill='#fff' font-size='16' "
            "font-family='Georgia,serif' font-weight='700' text-anchor='end'>"
            "%.1f%%</text>",
            x, y, x - 10, y - 10, current_pct);
    }

    /* X-axis: show year labels at January, plus quarter ticks */
    for (int i = 0; i < npts; i++) {
        time_t t = (time_t)cp_time[i];
        struct tm tm;
        gmtime_r(&t, &tm);
        int x = pad_l + plot_w * i / (npts - 1);

        if (tm.tm_mon == 0) {
            /* January — show year label + tick */
            APPEND(off, r, max,
                "<line x1='%d' y1='%d' x2='%d' y2='%d' stroke='#333' stroke-width='1'/>"
                "<text x='%d' y='%d' fill='#aaa' font-size='14' "
                "font-family='Georgia,serif' text-anchor='middle' font-weight='600'>"
                "%d</text>",
                x, pad_t + plot_h, x, pad_t + plot_h + 6,
                x, pad_t + plot_h + 24, tm.tm_year + 1900);
        } else if (tm.tm_mon % 3 == 0) {
            /* Quarter boundary — small tick only */
            APPEND(off, r, max,
                "<line x1='%d' y1='%d' x2='%d' y2='%d' stroke='#222' stroke-width='1'/>",
                x, pad_t + plot_h, x, pad_t + plot_h + 4);
        }
    }

    /* Source attribution */
    APPEND(off, r, max,
        "<text x='%d' y='%d' fill='#444' font-size='11' "
        "font-family='Georgia,serif' text-anchor='end'>"
        "Source: ZClassic23 UTXO Index &mdash; zclnet.net</text>",
        w - pad_r, h - 6);

    APPEND(off, r, max, "</svg>");

    /* JavaScript tooltip: shows crosshair + value on hover */
    APPEND(off, r, max,
        "<div id='hodl-tip' style='display:none;position:fixed;background:#1a1a2a;"
        "border:1px solid #aa66ff;border-radius:6px;padding:8px 12px;"
        "color:#fff;font-family:Georgia,serif;font-size:14px;"
        "pointer-events:none;z-index:999;box-shadow:0 4px 12px rgba(0,0,0,0.5)'></div>"
        "<script>"
        "document.querySelectorAll('.hodl-pt').forEach(function(el){"
        "el.addEventListener('mouseenter',function(e){"
        "var tip=document.getElementById('hodl-tip');"
        "tip.innerHTML='<b>'+el.dataset.date+'</b><br>'+el.dataset.pct+'%% unmoved &gt;1yr';"
        "tip.style.display='block';"
        "tip.style.left=(e.clientX+12)+'px';tip.style.top=(e.clientY-40)+'px'});"
        "el.addEventListener('mousemove',function(e){"
        "var tip=document.getElementById('hodl-tip');"
        "tip.style.left=(e.clientX+12)+'px';tip.style.top=(e.clientY-40)+'px'});"
        "el.addEventListener('mouseleave',function(){"
        "document.getElementById('hodl-tip').style.display='none'})});"
        "</script>");

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
    struct explorer_assets *assets = explorer_assets();
    /* Reload CSS from disk each time (allows live editing) */
    load_css();
    size_t off = 0;
    int n = snprintf((char *)r, max,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/css; charset=utf-8\r\n"
        "Cache-Control: public, max-age=60\r\n"
        "Connection: close\r\n\r\n");
    if (n > 0) off = (size_t)n;
    if (off + assets->css_len < max) {
        memcpy(r + off, assets->css_cache, assets->css_len);
        off += assets->css_len;
    }
    return off;
}

/* ── Event Log Page ───────────────────────────────────────── */

static size_t serve_events(uint8_t *r, size_t max)
{
    size_t off = 0;
    char *response = (char *)r;

    APPEND(off, response, max, EXPLORER_HEADER("Event Log — ZClassic23"));
    off += explorer_emit_nav(response + off, max - off, "events");

    APPEND(off, response, max,
        "<div class='content'>"
        "<h1>Event Log</h1>"
        "<p style='color:#888'>Live node events from the ring buffer. "
        "Auto-refreshes every 3 seconds.</p>"
        "<div style='margin:10px 0'>"
        "<label style='color:#aaa'>Show: </label>"
        "<select id='ev-count' style='background:#1a1a2e;color:#eee;border:1px solid #333;"
        "padding:4px 8px;border-radius:4px'>"
        "<option value='50'>50</option>"
        "<option value='100' selected>100</option>"
        "<option value='500'>500</option>"
        "<option value='2000'>2000</option>"
        "</select>"
        "<label style='color:#aaa;margin-left:16px'>Filter: </label>"
        "<input id='ev-filter' placeholder='type, peer, or data...' "
        "style='background:#1a1a2e;color:#eee;border:1px solid #333;"
        "padding:4px 8px;border-radius:4px;width:200px'>"
        "<span id='ev-status' style='color:#555;margin-left:16px;font-size:13px'>"
        "loading...</span>"
        "</div>"
        "<table class='block-table' style='font-size:13px'>"
        "<thead><tr>"
        "<th style='width:60px'>Seq</th>"
        "<th style='width:170px'>Time</th>"
        "<th style='width:180px'>Type</th>"
        "<th style='width:60px'>Peer</th>"
        "<th>Data</th>"
        "</tr></thead>"
        "<tbody id='ev-body'></tbody></table></div>");

    APPEND(off, response, max,
        "<script>"
        "const tbody=document.getElementById('ev-body'),"
        "sel=document.getElementById('ev-count'),"
        "flt=document.getElementById('ev-filter'),"
        "sts=document.getElementById('ev-status');"
        "function fmt(ts){"
        "const d=new Date(ts/1000);"
        "return d.toISOString().replace('T',' ').replace('Z','')}"
        "function cls(t){"
        "if(t.startsWith('val.'))return'color:#ff6b6b';"
        "if(t.startsWith('sync.'))return'color:#ffd93d';"
        "if(t.startsWith('peer.'))return'color:#6bcb77';"
        "if(t.startsWith('tcp.'))return'color:#4d96ff';"
        "if(t.startsWith('snap.'))return'color:#ff922b';"
        "if(t.startsWith('chain.'))return'color:#cc5de8';"
        "if(t.startsWith('tx.'))return'color:#66d9e8';"
        "if(t.startsWith('sys.'))return'color:#ff8787';"
        "return'color:#aaa'}"
        "function esc(s){const d=document.createElement('div');"
        "d.textContent=s;return d.innerHTML}"
        "async function refresh(){"
        "try{"
        "const r=await fetch('/api/events?count='+sel.value);"
        "const evs=await r.json();"
        "const f=flt.value.toLowerCase();"
        "let html='';"
        "for(let i=evs.length-1;i>=0;i--){"
        "const e=evs[i];"
        "if(f&&!(e.type+' '+e.peer+' '+e.data).toLowerCase().includes(f))continue;"
        "html+='<tr><td>'+e.seq+'</td>"
        "<td>'+fmt(e.ts)+'</td>"
        "<td style=\"'+cls(e.type)+'\">'+esc(e.type)+'</td>"
        "<td>'+(e.peer||'')+'</td>"
        "<td style=\"font-family:monospace;font-size:12px;word-break:break-all\">"
        "'+esc(e.data)+'</td></tr>'}"
        "tbody.innerHTML=html;"
        "sts.textContent=evs.length+' events ('+new Date().toLocaleTimeString()+')';"
        "}catch(e){sts.textContent='Error: '+e.message}}"
        "refresh();"
        "setInterval(refresh,3000);"
        "sel.onchange=refresh;"
        "flt.oninput=refresh;"
        "</script>");

    APPEND(off, response, max, EXPLORER_FOOTER);
    return off;
}

/* ── Main Request Handler ─────────────────────────────────── */

size_t explorer_handle_request(const char *method, const char *path,
                                const uint8_t *body, size_t body_len,
                                uint8_t *response, size_t response_max)
{
    (void)body; (void)body_len;
    if (!path || !response) return 0;

    /* Delegate /api/ routes to the REST API controller */
    if (strncmp(path, "/api/", 5) == 0 || strcmp(path, "/api") == 0) {
        return api_handle_request(method, path, body, body_len,
                                   response, response_max);
    }

    (void)method;

    if (strcmp(path, "/explorer/style.css") == 0)
        return serve_css(response, response_max);

    if (strcmp(path, "/explorer/favicon.png") == 0 ||
        strcmp(path, "/favicon.ico") == 0) {
        struct explorer_context *ctx = explorer_ctx();
        char fpath[1200];
        snprintf(fpath, sizeof(fpath), "%s/explorer/favicon.png", ctx->datadir);
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

    if (strcmp(path, "/explorer") == 0 || strcmp(path, "/explorer/") == 0 ||
        strncmp(path, "/explorer?", 10) == 0 || strncmp(path, "/explorer/?", 11) == 0) {
        int page = 0;
        const char *pp = strstr(path, "page=");
        if (pp) page = atoi(pp + 5);
        if (page < 0) page = 0;
        return serve_dashboard_with_page(response, response_max, page);
    }

    if (strcmp(path, "/explorer/stats") == 0 || strcmp(path, "/explorer/stats/") == 0)
        return serve_stats(response, response_max);

    if (strcmp(path, "/explorer/tokens") == 0 || strcmp(path, "/explorer/tokens/") == 0)
        return serve_tokens(response, response_max);

    if (strncmp(path, "/explorer/token/", 16) == 0)
        return serve_token_detail(path + 16, response, response_max);

    if (strcmp(path, "/explorer/hodl") == 0 || strcmp(path, "/explorer/hodl/") == 0)
        return serve_hodl(response, response_max);

    if (strcmp(path, "/explorer/events") == 0 || strcmp(path, "/explorer/events/") == 0)
        return serve_events(response, response_max);

    if (strcmp(path, "/explorer/factoids") == 0 || strcmp(path, "/explorer/factoids/") == 0)
        return serve_factoids(response, response_max);

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

    /* Wallet — self-contained HTML+CSS+JS, fetches /api/wallet */
    if (strcmp(path, "/wallet") == 0 || strcmp(path, "/wallet/") == 0) {
        size_t off = 0;
        APPEND(off, response, response_max,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Connection: close\r\n\r\n"
            "<!DOCTYPE html><html><head><meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>ZClassic23 Wallet</title>"
            "<style>"
            "*{margin:0;padding:0;box-sizing:border-box}"
            "body{background:#0d1117;color:#e6edf3;font-family:Inter,-apple-system,sans-serif;"
            "max-width:520px;margin:0 auto;padding:32px 24px;min-height:100vh}"
            ".hdr{display:flex;justify-content:space-between;align-items:center;margin-bottom:8px}"
            ".hdr h1{font-size:14px;font-weight:600;color:#8b949e}"
            ".sync{font-size:12px;color:#3fb950}"
            ".sync.warn{color:#d29922}"
            ".balance{text-align:center;margin:24px 0 16px}"
            ".balance .amount{font-size:42px;font-weight:700;letter-spacing:-1px}"
            ".balance .amount:hover{cursor:help}"
            ".subs{display:flex;justify-content:center;gap:48px;margin-bottom:20px}"
            ".sub{text-align:center}"
            ".sub .label{font-size:12px;color:#8b949e;margin-bottom:2px}"
            ".sub .val{font-size:16px;font-weight:500}"
            ".actions{display:flex;justify-content:center;gap:12px;margin-bottom:16px}"
            "button{background:#21262d;color:#c9d1d9;border:1px solid #30363d;"
            "border-radius:6px;padding:8px 28px;font-size:14px;cursor:pointer}"
            "button:hover{background:#30363d}"
            ".panel{background:#161b22;border:1px solid #21262d;border-radius:8px;"
            "padding:16px;margin-bottom:16px;display:none}"
            ".panel.show{display:block}"
            ".panel label{font-size:12px;color:#8b949e;display:block;margin-bottom:4px}"
            ".addr{font-size:15px;font-family:monospace;color:#58a6ff;"
            "word-break:break-all;user-select:all;margin:8px 0}"
            "input{width:100%%;background:#0d1117;color:#c9d1d9;border:1px solid #30363d;"
            "border-radius:6px;padding:8px 12px;font-family:monospace;font-size:14px;"
            "margin-bottom:8px}"
            "hr{border:none;border-top:1px solid #21262d;margin:16px 0}"
            ".activity h2{font-size:14px;font-weight:600;color:#8b949e;margin-bottom:12px}"
            ".tx{padding:12px 0;border-bottom:1px solid #161b22}"
            ".tx:last-child{border:none}"
            ".tx .line1{display:flex;justify-content:space-between;align-items:baseline}"
            ".tx .amt{font-size:14px;font-weight:600;color:#3fb950}"
            ".tx .ago{font-size:12px;color:#484f58}"
            ".tx .detail{font-size:11px;color:#30363d;margin-top:2px}"
            ".empty{text-align:center;color:#484f58;font-size:13px;padding:24px 0}"
            ".status{font-size:11px;color:#484f58;font-family:monospace;"
            "margin-top:16px;text-align:center}"
            ".copied{color:#3fb950;font-size:12px;margin-left:8px}"
            "</style></head><body>");
        APPEND(off, response, response_max,
            "<div class='hdr'><h1>ZClassic23</h1><span class='sync' id='sync'>Loading...</span></div>"
            "<div class='balance'>"
            "<div class='amount' id='bal' title='Hover for full precision'>—</div>"
            "</div>"
            "<div class='subs'>"
            "<div class='sub'><div class='label'>Transparent</div><div class='val' id='tbal'>—</div></div>"
            "<div class='sub'><div class='label'>Shielded</div><div class='val' id='sbal'>—</div></div>"
            "</div>"
            "<div class='actions'>"
            "<button onclick='togglePanel(\"recv\")'>Receive</button>"
            "<button onclick='togglePanel(\"send\")'>Send</button>"
            "</div>"
            "<div class='panel' id='recv'>"
            "<label>Your Address</label>"
            "<div class='addr' id='addr'>—</div>"
            "<button onclick='copyAddr()'>Copy</button><span class='copied' id='copied'></span>"
            "</div>"
            "<div class='panel' id='send'>"
            "<label>Recipient</label>"
            "<input id='to' placeholder='t1... address'>"
            "<label>Amount</label>"
            "<input id='amt' placeholder='0.00' type='number' step='0.001'>"
            "<button onclick='doSend()'>Send</button>"
            "<div id='sendmsg' style='font-size:12px;color:#484f58;margin-top:8px'></div>"
            "</div>"
            "<hr>"
            "<div class='activity'>"
            "<h2>Activity</h2>"
            "<div id='txlist'><div class='empty'>Loading...</div></div>"
            "</div>"
            "<div class='status' id='status'></div>"
            "<script>");
        APPEND(off, response, response_max,
            "function fmt(z,d){var w=Math.floor(z/1e8),f=Math.abs(z%%1e8);"
            "if(d===3)return w+'.'+String(Math.floor(f/1e5)).padStart(3,'0');"
            "var s=w+'.'+String(f).padStart(8,'0');return s.replace(/0+$/,'').replace(/\\.$/,'.0')}"
            "function ago(t,now){var d=now-t;if(d<60)return'just now';"
            "if(d<3600)return Math.floor(d/60)+' min ago';"
            "if(d<86400)return Math.floor(d/3600)+' hours ago';"
            "return Math.floor(d/86400)+' days ago'}"
            "function togglePanel(id){var p=document.getElementById(id);"
            "var other=id==='recv'?'send':'recv';"
            "document.getElementById(other).classList.remove('show');"
            "p.classList.toggle('show')}"
            "function copyAddr(){var a=document.getElementById('addr').textContent;"
            "navigator.clipboard.writeText(a).then(function(){"
            "document.getElementById('copied').textContent='\\u2713 Copied';"
            "setTimeout(function(){document.getElementById('copied').textContent=''},2000)})}"
            "function doSend(){document.getElementById('sendmsg').textContent="
            "'Sending requires the node to be running with RPC enabled'}"
            "function update(){fetch('/api/wallet').then(r=>r.json()).then(function(d){"
            "var total=d.transparent+d.shielded;"
            "document.getElementById('bal').textContent=fmt(total,3)+' ZCL';"
            "document.getElementById('bal').title=fmt(total,8)+' ZCL (exact)';"
            "document.getElementById('tbal').textContent=fmt(d.transparent,3)+' ZCL';"
            "document.getElementById('sbal').textContent=d.shielded>0?fmt(d.shielded,3)+' ZCL':'\\u2014';"
            "document.getElementById('addr').textContent=d.address||'No wallet keys';"
            "var age=d.now-d.block_time;"
            "var sy=document.getElementById('sync');"
            "if(d.block_time===0){sy.textContent='\\u25cf No blocks';sy.className='sync warn'}"
            "else if(age<300){sy.textContent='\\u25cf Synced';sy.className='sync'}"
            "else if(age<3600){sy.textContent='\\u25cf '+Math.floor(age/60)+' min behind';sy.className='sync warn'}"
            "else{sy.textContent='\\u25cf '+Math.floor(age/3600)+' hours behind';sy.className='sync warn'}"
            "document.getElementById('status').textContent='Block '+d.height;"
            "var h='';"
            "if(d.activity.length===0)h='<div class=\"empty\">No transactions yet</div>';"
            "d.activity.forEach(function(tx){"
            "h+='<div class=\"tx\"><div class=\"line1\">';"
            "h+='<span class=\"amt\">\\u2193 Received '+fmt(tx.value,3)+' ZCL</span>';"
            "h+='<span class=\"ago\">'+ago(tx.time,d.now)+'</span></div>';"
            "h+='<div class=\"detail\">Block '+tx.height+' \\u00b7 confirmed</div></div>'});"
            "document.getElementById('txlist').innerHTML=h"
            "}).catch(function(){document.getElementById('sync').textContent='\\u25cf Offline';"
            "document.getElementById('sync').className='sync warn'})}"
            "update();setInterval(update,3000)");
        APPEND(off, response, response_max,
            "</script></body></html>");
        return off;
    }

    return 0; /* unhandled → caller returns 404 */
}
