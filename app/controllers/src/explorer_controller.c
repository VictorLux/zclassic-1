/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Block explorer controller — comprehensive blockchain explorer served
 * over Tor .onion. Supports blocks, transactions (transparent + shielded),
 * ZSLP tokens, and address lookups. */

#include "platform/time_compat.h"
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
#include "encoding/utilstrencodings.h"
#include "core/serialize.h"
#include "keys/key_io.h"
#include "models/database.h"
#include "models/hodl_wave.h"
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
#include "util/ar_step_readonly.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include "controllers/explorer_internal.h"
#include "util/template.h"
#include "views/wallet_templates_gen.h"
#include "views/explorer_css.h"
#include "views/format_helpers.h"
#include "explorer_controller_internal.h"

/* struct explorer_context defined in explorer_controller_internal.h */

struct explorer_rpc_backend {
    char user[128];
    char pass[128];
    int proxy_port;
};

/* struct explorer_assets defined in explorer_controller_internal.h */

static struct explorer_context g_explorer_ctx = {0};
static struct explorer_rpc_backend g_explorer_rpc = {
    .user = "zcluser",
    .pass = "zclpass",
    .proxy_port = 8023,
};
static struct explorer_assets g_explorer_assets = {0};

struct explorer_context *explorer_ctx(void)
{
    return &g_explorer_ctx;
}

static struct explorer_rpc_backend *explorer_rpc(void)
{
    return &g_explorer_rpc;
}

struct explorer_assets *explorer_assets(void)
{
    return &g_explorer_assets;
}

/* Transitional aliases while this large controller is moved over in slices. */
/* ── Template system ───────────────────────────────────────── */

void ensure_explorer_dir(void)
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

void load_css(void)
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

/* compute threads forward-declared in explorer_controller_internal.h */
/* g_stats_computing defined in explorer_controller_pages.c */
/* g_tokens_computing defined in explorer_controller_pages.c */
/* g_factoids_computing defined in explorer_controller_pages.c */
static _Atomic int g_prewarm_started;

bool explorer_start_detached_thread(pthread_t *thread_out,
                                           void *(*entry)(void *),
                                           void *arg,
                                           size_t stack_size)
{
    pthread_attr_t attr;
    bool ok = false;

    if (!thread_out || !entry)
        LOG_FAIL("explorer", "explorer_start_detached_thread: NULL thread_out or entry");
    if (pthread_attr_init(&attr) != 0)
        LOG_FAIL("explorer", "explorer_start_detached_thread: pthread_attr_init failed");
    if (pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) != 0)
        goto cleanup;
    if (stack_size > 0 && pthread_attr_setstacksize(&attr, stack_size) != 0)
        goto cleanup;
    /* raw-pthread-ok: detached-helper-wrapper (custom stack via pthread_attr) */
    if (pthread_create(thread_out, &attr, entry, arg) != 0)
        goto cleanup;
    ok = true;

cleanup:
    pthread_attr_destroy(&attr);
    return ok;
}

bool explorer_start_once(_Atomic int *flag,
                                void *(*entry)(void *),
                                const char *name)
{
    int expected = 0;
    pthread_t t;

    if (!flag || !entry)
        LOG_FAIL("explorer", "explorer_start_once: NULL flag or entry for %s", name ? name : "unknown");
    if (!atomic_compare_exchange_strong(flag, &expected, 1))
        return expected == 1;
    if (!explorer_start_detached_thread(&t, entry, NULL, 2 * 1024 * 1024)) {
        atomic_store(flag, 0);
        LOG_FAIL("explorer", "Explorer: failed to start %s thread", name ? name : "unknown");
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
    int chain_tip_h = ms ? active_chain_height(&ms->chain_active) : 0;
    int best_header = (ms && ms->pindex_best_header) ?
        ms->pindex_best_header->nHeight : chain_tip_h;
    if (best_header - chain_tip_h > 1000) {
        printf("Explorer prewarm deferred during IBD "
               "(chain=%d, headers=%d, behind=%d)\n",
               chain_tip_h, best_header, best_header - chain_tip_h);
        return;
    }
    explorer_start_once(&g_prewarm_started, prewarm_thread, "prewarm");
}

/* ── RPC proxy to local zclassicd ─────────────────────────── */

int rpc_call(const char *method, const char *params_json,
                     char *out, size_t outmax)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) LOG_ERR("explorer", "rpc_call(%s): socket() failed", method);

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
        LOG_ERR("explorer", "rpc_call(%s): connect to port %d failed", method, explorer_rpc()->proxy_port);
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

    if (write(fd, req, (size_t)rlen) != rlen) { close(fd); LOG_ERR("explorer", "rpc_call(%s): write failed", method); }

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
bool json_extract_str(const char *json, const char *key,
                              char *out, size_t outmax)
{
    return zcl_json_extract_str(json, key, out, outmax);
}

int64_t json_extract_int(const char *json, const char *key)
{
    int64_t v = -1;
    zcl_json_extract_int(json, key, &v);
    return v;
}

double json_extract_real(const char *json, const char *key)
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
            if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW)  
                h = sqlite3_column_int(s, 0);
            sqlite3_finalize(s);
        }
        return h;
    }
    LOG_ERR("explorer", "native_chain_height: no main_state or node_db available");
}

bool use_rpc_proxy(void)
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

double explorer_get_difficulty(const struct block_index *bi)
{
    if (!bi) return 1.0;
    return difficulty_from_bits(bi->nBits);
}

bool explorer_param_is_printable_ascii(const char *s)
{
    if (!s) return false;
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c < 32 || c > 126)
            return false;
    }
    return true;
}

void format_time(char *buf, size_t max, uint32_t t)
{
    zcl_format_time(buf, max, (int64_t)t);
}

void format_time_ago(char *buf, size_t max, uint32_t t)
{
    time_t now = platform_time_wall_time_t();
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

/* Encode a tx_destination to a t-address string */
bool addr_encode(char *out, size_t outmax,
                         const struct tx_destination *dest)
{
    const struct chain_params *cp = chain_params_get();
    if (!cp) LOG_FAIL("explorer", "addr_encode: chain_params_get returned NULL");
    size_t pk_len = 0, sh_len = 0;
    const unsigned char *pk_pfx = chain_params_base58_prefix(cp, B58_PUBKEY_ADDRESS, &pk_len);
    const unsigned char *sh_pfx = chain_params_base58_prefix(cp, B58_SCRIPT_ADDRESS, &sh_len);
    return encode_destination(dest, pk_pfx, pk_len, sh_pfx, sh_len, out, outmax);
}

bool addr_decode(const char *str, struct tx_destination *dest)
{
    const struct chain_params *cp = chain_params_get();
    if (!cp) LOG_FAIL("explorer", "addr_decode: chain_params_get returned NULL");
    size_t pk_len = 0, sh_len = 0;
    const unsigned char *pk_pfx = chain_params_base58_prefix(cp, B58_PUBKEY_ADDRESS, &pk_len);
    const unsigned char *sh_pfx = chain_params_base58_prefix(cp, B58_SCRIPT_ADDRESS, &sh_len);
    return decode_destination(str, pk_pfx, pk_len, sh_pfx, sh_len, dest);
}

/* ── Macros from explorer_internal.h ──────────────────────── */
/* EXPLORER_HEADER, EXPLORER_NAV, EXPLORER_FOOTER, and APPEND
 * are all defined in controllers/explorer_internal.h (single source
 * of truth for all explorer pages). */

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

    if (strcmp(path, "/explorer/names") == 0 || strcmp(path, "/explorer/names/") == 0)
        return serve_names(response, response_max);

    if (strcmp(path, "/explorer/market") == 0 || strcmp(path, "/explorer/market/") == 0)
        return serve_market(response, response_max);

    if (strcmp(path, "/explorer/swaps") == 0 || strcmp(path, "/explorer/swaps/") == 0)
        return serve_swaps(response, response_max);

    if (strcmp(path, "/explorer/messages") == 0 || strcmp(path, "/explorer/messages/") == 0)
        return serve_messages(response, response_max);

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
