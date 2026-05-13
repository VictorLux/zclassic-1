/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Header Probe Service. See header for the high-level rationale.
 *
 * Layout:
 *   1. Config + creds (parse zclassic.conf — reused pattern from oracle)
 *   2. POSIX-sockets HTTP/1.1 JSON-RPC client (Basic auth)
 *   3. Parse JSON-RPC responses (string + integer results)
 *   4. header_probe_pull_range() — fetch + validate + insert
 *   5. on_tick() — periodic heartbeat callback
 *   6. init/start/stop + stats snapshot + dump_state_json
 *
 * Threading: the only background work is the heartbeat sweeper, which
 * is owned by lib/health/heartbeat.c. No new pthreads are created here.
 */

#include "services/header_probe_service.h"

#include "validation/main_state.h"
#include "validation/chainstate.h"
#include "validation/process_block.h"
#include "consensus/validation.h"
#include "chain/chain.h"
#include "chain/chainparams.h"
#include "core/uint256.h"
#include "core/serialize.h"
#include "primitives/block.h"
#include "encoding/utilstrencodings.h"
#include "json/json.h"
#include "health/heartbeat.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ── Constants ─────────────────────────────────────────────────── */

#define HP_DEFAULT_HOST          "127.0.0.1"
#define HP_DEFAULT_PORT          8232
#define HP_DEFAULT_CADENCE       30
#define HP_DEFAULT_BATCH         2000
#define HP_DEFAULT_LAG           100
#define HP_MAX_BATCH             5000
#define HP_RPC_TIMEOUT_SECS      5
#define HP_RESPONSE_MAX          16384    /* a full hex-header is ~3 KB */
#define HP_MAX_HEADER_BYTES      (BLOCK_HEADER_SIZE + MAX_SOLUTION_SIZE + 8)

/* ── Global state ──────────────────────────────────────────────── */

static struct {
    pthread_mutex_t lock;       /* guards config + non-atomic fields */
    bool   initialized;
    char   rpc_host[64];
    int    rpc_port;
    char   rpc_user[64];
    char   rpc_password[128];
    int    cadence_secs;
    int    batch_size;
    int    lag_threshold;
    health_subsystem_id health_id;
    struct main_state *ms;
    const struct chain_params *params;

    /* Stats */
    _Atomic int64_t calls_total;
    _Atomic int64_t headers_added;
    _Atomic int64_t headers_rejected;
    _Atomic int64_t rpc_errors;
    _Atomic int     last_remote_height;
    _Atomic int     last_local_height;
} g_hp = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .health_id = HEALTH_INVALID_ID,
};

/* ── zclassic.conf parser (mirrors oracle service) ─────────────── */

static bool hp_parse_zclassic_conf(char *out_user, size_t user_sz,
                                   char *out_pass, size_t pass_sz,
                                   int *out_port)
{
    const char *home = getenv("HOME");
    if (!home || !home[0]) return false;

    char path[512];
    snprintf(path, sizeof(path), "%s/.zclassic/zclassic.conf", home);
    FILE *f = fopen(path, "r");
    if (!f) return false;

    char line[512];
    bool got_user = false, got_pass = false;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == ';' || *p == '\n' || *p == '\0') continue;

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;
        char *kend = eq - 1;
        while (kend > key && (*kend == ' ' || *kend == '\t')) *kend-- = '\0';
        while (*val == ' ' || *val == '\t') val++;
        char *vend = val + strlen(val);
        while (vend > val && (vend[-1] == '\n' || vend[-1] == '\r' ||
                              vend[-1] == ' '  || vend[-1] == '\t'))
            *--vend = '\0';

        if (strcmp(key, "rpcuser") == 0) {
            snprintf(out_user, user_sz, "%s", val);
            got_user = true;
        } else if (strcmp(key, "rpcpassword") == 0) {
            snprintf(out_pass, pass_sz, "%s", val);
            got_pass = true;
        } else if (strcmp(key, "rpcport") == 0 && out_port) {
            int n = atoi(val);
            if (n > 0 && n < 65536) *out_port = n;
        }
    }
    fclose(f);
    return got_user && got_pass;
}

/* ── Base64 encoder (Basic auth) ────────────────────────────────── */

static const char hp_b64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void hp_base64_encode(const unsigned char *in, size_t inlen,
                             char *out, size_t outsz)
{
    size_t i = 0, o = 0;
    while (i + 3 <= inlen && o + 4 < outsz) {
        unsigned v = (unsigned)((in[i] << 16) | (in[i+1] << 8) | in[i+2]);
        out[o++] = hp_b64_chars[(v >> 18) & 0x3f];
        out[o++] = hp_b64_chars[(v >> 12) & 0x3f];
        out[o++] = hp_b64_chars[(v >>  6) & 0x3f];
        out[o++] = hp_b64_chars[ v        & 0x3f];
        i += 3;
    }
    if (i < inlen && o + 4 < outsz) {
        unsigned v = (unsigned)(in[i] << 16);
        if (i + 1 < inlen) v |= (unsigned)(in[i+1] << 8);
        out[o++] = hp_b64_chars[(v >> 18) & 0x3f];
        out[o++] = hp_b64_chars[(v >> 12) & 0x3f];
        out[o++] = (i + 1 < inlen) ? hp_b64_chars[(v >> 6) & 0x3f] : '=';
        out[o++] = '=';
    }
    if (o < outsz) out[o] = '\0';
    else if (outsz > 0) out[outsz - 1] = '\0';
}

/* ── Minimal HTTP/1.1 JSON-RPC client ──────────────────────────────
 *
 * `resp` must be large enough for a hex-encoded header response
 * (~3 KB body + a few hundred bytes of HTTP headers). Returns true
 * if a full body was received. */

static bool hp_http_rpc_call(const char *host, int port,
                             const char *user, const char *pass,
                             const char *body_json,
                             char *resp, size_t resp_cap,
                             char *err, size_t err_sz)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(err, err_sz, "socket: %s", strerror(errno));
        return false;
    }

    struct timeval tv = { .tv_sec = HP_RPC_TIMEOUT_SECS, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        close(fd);
        snprintf(err, err_sz, "bad host: %s", host);
        return false;
    }
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        snprintf(err, err_sz, "connect %s:%d: %s",
                 host, port, strerror(errno));
        close(fd);
        return false;
    }

    char userpass[256];
    snprintf(userpass, sizeof(userpass), "%s:%s",
             user ? user : "", pass ? pass : "");
    char b64[384];
    hp_base64_encode((const unsigned char *)userpass, strlen(userpass),
                     b64, sizeof(b64));

    char req[1024];
    int reqlen = snprintf(req, sizeof(req),
        "POST / HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Authorization: Basic %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        host, port, b64, strlen(body_json), body_json);
    if (reqlen < 0 || (size_t)reqlen >= sizeof(req)) {
        close(fd);
        snprintf(err, err_sz, "request too large");
        return false;
    }

    ssize_t sent = 0;
    while (sent < reqlen) {
        ssize_t n = send(fd, req + sent, (size_t)(reqlen - sent), 0);
        if (n <= 0) {
            snprintf(err, err_sz, "send: %s", strerror(errno));
            close(fd);
            return false;
        }
        sent += n;
    }

    size_t total = 0;
    while (total + 1 < resp_cap) {
        ssize_t n = recv(fd, resp + total, resp_cap - total - 1, 0);
        if (n < 0) {
            snprintf(err, err_sz, "recv: %s", strerror(errno));
            close(fd);
            return false;
        }
        if (n == 0) break;
        total += (size_t)n;
    }
    resp[total] = '\0';
    close(fd);

    if (total == 0) {
        snprintf(err, err_sz, "empty response");
        return false;
    }
    return true;
}

/* Parse a JSON-RPC response body, returning the `.result` value:
 *   - If string: copies up to out_sz-1 chars into out_str, returns 1.
 *   - If integer: writes the integer into *out_int, returns 2.
 *   - Otherwise: writes error message into err, returns 0. */
static int hp_parse_rpc_result(const char *raw,
                               char *out_str, size_t out_sz,
                               int64_t *out_int,
                               char *err, size_t err_sz)
{
    const char *body = strstr(raw, "\r\n\r\n");
    if (!body) {
        snprintf(err, err_sz, "no http body separator");
        return 0;
    }
    body += 4;

    struct json_value v = {0};
    if (!json_read(&v, body, strlen(body))) {
        snprintf(err, err_sz, "json parse failed");
        json_free(&v);
        return 0;
    }
    const struct json_value *result = json_get(&v, "result");
    if (!result || (result->type != JSON_STR && result->type != JSON_INT)) {
        const struct json_value *jerr = json_get(&v, "error");
        if (jerr && jerr->type == JSON_OBJ) {
            const struct json_value *msg = json_get(jerr, "message");
            if (msg && msg->type == JSON_STR) {
                snprintf(err, err_sz, "rpc error: %s", json_get_str(msg));
                json_free(&v);
                return 0;
            }
        }
        snprintf(err, err_sz, "no .result or wrong type");
        json_free(&v);
        return 0;
    }
    int kind = 0;
    if (result->type == JSON_STR) {
        const char *s = json_get_str(result);
        size_t slen = s ? strlen(s) : 0;
        if (slen + 1 > out_sz) {
            snprintf(err, err_sz, "result string too long (%zu)", slen);
            json_free(&v);
            return 0;
        }
        memcpy(out_str, s ? s : "", slen);
        out_str[slen] = '\0';
        kind = 1;
    } else {
        if (out_int) *out_int = json_get_int(result);
        kind = 2;
    }
    json_free(&v);
    return kind;
}

/* Build a JSON-RPC body for a method that takes one int param. */
static void hp_build_rpc_body_int(char *body, size_t body_sz,
                                  const char *method, int64_t param)
{
    snprintf(body, body_sz,
        "{\"jsonrpc\":\"1.0\",\"id\":\"zcl-hp\","
        "\"method\":\"%s\",\"params\":[%lld]}",
        method, (long long)param);
}

/* Build a JSON-RPC body: getblockheader(hash, false) → hex string. */
static void hp_build_getblockheader_body(char *body, size_t body_sz,
                                         const char *hash_hex)
{
    snprintf(body, body_sz,
        "{\"jsonrpc\":\"1.0\",\"id\":\"zcl-hp\","
        "\"method\":\"getblockheader\",\"params\":[\"%s\",false]}",
        hash_hex);
}

/* Build a JSON-RPC body: getblockcount() → int. */
static void hp_build_getblockcount_body(char *body, size_t body_sz)
{
    snprintf(body, body_sz,
        "{\"jsonrpc\":\"1.0\",\"id\":\"zcl-hp\","
        "\"method\":\"getblockcount\",\"params\":[]}");
}

/* ── Remote-tip fetch ──────────────────────────────────────────── */

static bool hp_fetch_remote_tip(const char *host, int port,
                                const char *user, const char *pass,
                                int *out_height,
                                char *err, size_t err_sz)
{
    char body[128];
    hp_build_getblockcount_body(body, sizeof(body));
    char *resp = zcl_malloc(HP_RESPONSE_MAX, "hp_resp_tip");
    if (!resp) {
        snprintf(err, err_sz, "oom resp");
        return false;
    }
    bool ok = hp_http_rpc_call(host, port, user, pass, body,
                               resp, HP_RESPONSE_MAX, err, err_sz);
    if (!ok) { free(resp); return false; }
    int64_t h = 0;
    int kind = hp_parse_rpc_result(resp, NULL, 0, &h, err, err_sz);
    free(resp);
    if (kind != 2 || h < 0 || h > 0x7fffffff) {
        if (kind == 1) snprintf(err, err_sz, "tip: result not int");
        return false;
    }
    *out_height = (int)h;
    return true;
}

/* ── Header fetch + validation + insert ────────────────────────── */

static bool hp_fetch_one_header(const char *host, int port,
                                const char *user, const char *pass,
                                int height,
                                struct block_header *out_hdr,
                                char *err, size_t err_sz)
{
    char *resp = zcl_malloc(HP_RESPONSE_MAX, "hp_resp_hdr");
    if (!resp) {
        snprintf(err, err_sz, "oom resp");
        return false;
    }

    /* 1) getblockhash(height) */
    char body[256];
    hp_build_rpc_body_int(body, sizeof(body), "getblockhash", height);
    if (!hp_http_rpc_call(host, port, user, pass, body,
                          resp, HP_RESPONSE_MAX, err, err_sz)) {
        free(resp);
        return false;
    }
    char hash_hex[80] = {0};
    int kind = hp_parse_rpc_result(resp, hash_hex, sizeof(hash_hex),
                                    NULL, err, err_sz);
    if (kind != 1 || strlen(hash_hex) != 64) {
        if (kind == 1)
            snprintf(err, err_sz, "hash not 64 hex chars");
        free(resp);
        return false;
    }

    /* 2) getblockheader(hash, false) → hex header */
    hp_build_getblockheader_body(body, sizeof(body), hash_hex);
    if (!hp_http_rpc_call(host, port, user, pass, body,
                          resp, HP_RESPONSE_MAX, err, err_sz)) {
        free(resp);
        return false;
    }
    /* Allocate string buffer large enough for the longest header hex
     * (2 * HP_MAX_HEADER_BYTES + slack). */
    size_t hex_cap = HP_MAX_HEADER_BYTES * 2 + 16;
    char *hex = zcl_malloc(hex_cap, "hp_hdr_hex");
    if (!hex) {
        snprintf(err, err_sz, "oom hex");
        free(resp);
        return false;
    }
    kind = hp_parse_rpc_result(resp, hex, hex_cap, NULL, err, err_sz);
    free(resp);
    if (kind != 1) {
        free(hex);
        return false;
    }
    size_t hex_len = strlen(hex);
    if (hex_len < 280 /* 140 bytes header minimum */ || (hex_len % 2) != 0) {
        snprintf(err, err_sz, "bad hex header length %zu", hex_len);
        free(hex);
        return false;
    }

    /* 3) Hex decode + deserialize */
    unsigned char *bytes = zcl_malloc(hex_len / 2, "hp_hdr_bytes");
    if (!bytes) {
        snprintf(err, err_sz, "oom bytes");
        free(hex);
        return false;
    }
    size_t n_bytes = ParseHex(hex, bytes, hex_len / 2);
    free(hex);
    if (n_bytes < BLOCK_HEADER_SIZE) {
        snprintf(err, err_sz, "decoded header too short (%zu)", n_bytes);
        free(bytes);
        return false;
    }
    struct byte_stream s;
    stream_init_from_data(&s, bytes, n_bytes);
    block_header_init(out_hdr);
    bool deser_ok = block_header_deserialize(out_hdr, &s);
    stream_free(&s);
    free(bytes);
    if (!deser_ok) {
        snprintf(err, err_sz, "block_header_deserialize failed");
        return false;
    }
    return true;
}

/* ── Public pull-range ─────────────────────────────────────────── */

bool header_probe_pull_range(int start_height, int max_headers,
                             int *out_added)
{
    if (out_added) *out_added = 0;
    if (start_height < 0) {
        LOG_FAIL("header_probe", "pull_range: bad start_height=%d",
                 start_height);
    }

    pthread_mutex_lock(&g_hp.lock);
    if (!g_hp.initialized || !g_hp.ms || !g_hp.params) {
        pthread_mutex_unlock(&g_hp.lock);
        LOG_FAIL("header_probe", "pull_range: not initialized");
    }
    char host[64], user[64], pass[128];
    int port;
    snprintf(host, sizeof(host), "%s",
             g_hp.rpc_host[0] ? g_hp.rpc_host : HP_DEFAULT_HOST);
    port = g_hp.rpc_port ? g_hp.rpc_port : HP_DEFAULT_PORT;
    snprintf(user, sizeof(user), "%s", g_hp.rpc_user);
    snprintf(pass, sizeof(pass), "%s", g_hp.rpc_password);
    struct main_state *ms = g_hp.ms;
    const struct chain_params *params = g_hp.params;
    pthread_mutex_unlock(&g_hp.lock);

    atomic_fetch_add(&g_hp.calls_total, 1);

    /* Clamp batch size. */
    if (max_headers <= 0) max_headers = HP_DEFAULT_BATCH;
    if (max_headers > HP_MAX_BATCH) max_headers = HP_MAX_BATCH;

    /* Discover remote tip — bounds the loop and updates last_remote. */
    int remote_tip = -1;
    char err[160] = {0};
    if (!hp_fetch_remote_tip(host, port, user, pass, &remote_tip,
                             err, sizeof(err))) {
        atomic_fetch_add(&g_hp.rpc_errors, 1);
        /* Not a fatal logic failure — return true with 0 added so the
         * MCP/test callers can distinguish "RPC unreachable" via the
         * stats snapshot. */
        return true;
    }
    atomic_store(&g_hp.last_remote_height, remote_tip);

    /* Local tip (header tip is the high-water mark for headers). */
    int local_tip = 0;
    if (ms->pindex_best_header)
        local_tip = ms->pindex_best_header->nHeight;
    else
        local_tip = active_chain_height(&ms->chain_active);
    if (local_tip < 0) local_tip = 0;
    atomic_store(&g_hp.last_local_height, local_tip);

    int end_height = start_height + max_headers - 1;
    if (end_height > remote_tip) end_height = remote_tip;
    if (end_height < start_height) return true;  /* nothing to do */

    int added = 0;
    for (int h = start_height; h <= end_height; h++) {
        struct block_header hdr;
        if (!hp_fetch_one_header(host, port, user, pass, h,
                                 &hdr, err, sizeof(err))) {
            atomic_fetch_add(&g_hp.rpc_errors, 1);
            /* Stop on first RPC error — likely a transient outage. */
            break;
        }
        struct validation_state vs;
        validation_state_init(&vs);
        struct block_index *pindex = NULL;
        if (accept_block_header(&hdr, &vs, ms, params, &pindex)) {
            atomic_fetch_add(&g_hp.headers_added, 1);
            added++;
            if (pindex && pindex->nHeight > 0)
                atomic_store(&g_hp.last_local_height, pindex->nHeight);
        } else {
            atomic_fetch_add(&g_hp.headers_rejected, 1);
            /* Stop the batch on a reject — the parent of subsequent
             * headers would be unknown anyway. */
            break;
        }
    }

    if (out_added) *out_added = added;
    return true;
}

/* ── Periodic tick (heartbeat callback) ────────────────────────── */

static void hp_on_tick(void *ctx)
{
    (void)ctx;
    pthread_mutex_lock(&g_hp.lock);
    struct main_state *ms = g_hp.ms;
    int lag_thresh = g_hp.lag_threshold > 0
                         ? g_hp.lag_threshold : HP_DEFAULT_LAG;
    int batch = g_hp.batch_size > 0 ? g_hp.batch_size : HP_DEFAULT_BATCH;
    pthread_mutex_unlock(&g_hp.lock);
    if (!ms) return;

    int local_tip = 0;
    if (ms->pindex_best_header)
        local_tip = ms->pindex_best_header->nHeight;
    else
        local_tip = active_chain_height(&ms->chain_active);
    if (local_tip < 0) local_tip = 0;

    /* Cheap getblockcount to decide whether to pull. */
    char host[64], user[64], pass[128];
    int port;
    pthread_mutex_lock(&g_hp.lock);
    snprintf(host, sizeof(host), "%s",
             g_hp.rpc_host[0] ? g_hp.rpc_host : HP_DEFAULT_HOST);
    port = g_hp.rpc_port ? g_hp.rpc_port : HP_DEFAULT_PORT;
    snprintf(user, sizeof(user), "%s", g_hp.rpc_user);
    snprintf(pass, sizeof(pass), "%s", g_hp.rpc_password);
    pthread_mutex_unlock(&g_hp.lock);

    int remote_tip = -1;
    char err[160] = {0};
    if (!hp_fetch_remote_tip(host, port, user, pass, &remote_tip,
                             err, sizeof(err))) {
        atomic_fetch_add(&g_hp.rpc_errors, 1);
        return;
    }
    atomic_store(&g_hp.last_remote_height, remote_tip);
    atomic_store(&g_hp.last_local_height, local_tip);

    if (remote_tip <= local_tip + lag_thresh) return;  /* under-lag */

    int added = 0;
    (void)header_probe_pull_range(local_tip + 1, batch, &added);
}

/* ── init / start / stop ───────────────────────────────────────── */

bool header_probe_init(const struct header_probe_config *cfg,
                       struct main_state *ms,
                       const struct chain_params *params)
{
    pthread_mutex_lock(&g_hp.lock);

    snprintf(g_hp.rpc_host, sizeof(g_hp.rpc_host), "%s",
             (cfg && cfg->rpc_host) ? cfg->rpc_host : HP_DEFAULT_HOST);
    g_hp.rpc_port = (cfg && cfg->rpc_port > 0)
                        ? cfg->rpc_port : HP_DEFAULT_PORT;
    g_hp.cadence_secs = (cfg && cfg->cadence_secs > 0)
                        ? cfg->cadence_secs : HP_DEFAULT_CADENCE;
    g_hp.batch_size = (cfg && cfg->batch_size > 0)
                        ? cfg->batch_size : HP_DEFAULT_BATCH;
    if (g_hp.batch_size > HP_MAX_BATCH) g_hp.batch_size = HP_MAX_BATCH;
    g_hp.lag_threshold = (cfg && cfg->lag_threshold > 0)
                        ? cfg->lag_threshold : HP_DEFAULT_LAG;
    g_hp.ms = ms;
    g_hp.params = params;

    if (cfg && cfg->rpc_user && cfg->rpc_user[0]) {
        snprintf(g_hp.rpc_user, sizeof(g_hp.rpc_user),
                 "%s", cfg->rpc_user);
    }
    if (cfg && cfg->rpc_password && cfg->rpc_password[0]) {
        snprintf(g_hp.rpc_password, sizeof(g_hp.rpc_password),
                 "%s", cfg->rpc_password);
    }

    bool need_user = (g_hp.rpc_user[0] == '\0');
    bool need_pass = (g_hp.rpc_password[0] == '\0');
    if (need_user || need_pass) {
        int port_from_conf = g_hp.rpc_port;
        char u[64] = {0}, p[128] = {0};
        if (hp_parse_zclassic_conf(u, sizeof(u), p, sizeof(p),
                                   &port_from_conf)) {
            if (need_user)
                snprintf(g_hp.rpc_user, sizeof(g_hp.rpc_user), "%s", u);
            if (need_pass)
                snprintf(g_hp.rpc_password, sizeof(g_hp.rpc_password),
                         "%s", p);
            if (!cfg || cfg->rpc_port <= 0)
                g_hp.rpc_port = port_from_conf;
        } else if (need_user || need_pass) {
            pthread_mutex_unlock(&g_hp.lock);
            LOG_FAIL("header_probe",
                     "no RPC credentials: pass via config or ~/.zclassic/zclassic.conf");
        }
    }

    g_hp.initialized = true;
    pthread_mutex_unlock(&g_hp.lock);
    return true;
}

bool header_probe_start(void)
{
    if (g_hp.health_id != HEALTH_INVALID_ID) return true;
    if (!g_hp.initialized) {
        LOG_FAIL("header_probe", "start: not initialized");
    }
    (void)health_start();  /* idempotent */
    int cad = g_hp.cadence_secs > 0
                  ? g_hp.cadence_secs : HP_DEFAULT_CADENCE;
    g_hp.health_id = health_register_periodic(
        "header_probe", cad, hp_on_tick, NULL);
    if (g_hp.health_id == HEALTH_INVALID_ID) {
        LOG_FAIL("header_probe", "health_register_periodic failed");
    }
    return true;
}

void header_probe_stop(void)
{
    if (g_hp.health_id == HEALTH_INVALID_ID) return;
    health_unregister(g_hp.health_id);
    g_hp.health_id = HEALTH_INVALID_ID;
}

/* ── Stats snapshot ────────────────────────────────────────────── */

void header_probe_stats_snapshot(struct header_probe_stats *out)
{
    if (!out) return;
    out->calls_total        = atomic_load(&g_hp.calls_total);
    out->headers_added      = atomic_load(&g_hp.headers_added);
    out->headers_rejected   = atomic_load(&g_hp.headers_rejected);
    out->rpc_errors         = atomic_load(&g_hp.rpc_errors);
    out->last_remote_height = atomic_load(&g_hp.last_remote_height);
    out->last_local_height  = atomic_load(&g_hp.last_local_height);
}

void header_probe_reset_for_test(void)
{
    pthread_mutex_lock(&g_hp.lock);
    if (g_hp.health_id != HEALTH_INVALID_ID) {
        health_unregister(g_hp.health_id);
        g_hp.health_id = HEALTH_INVALID_ID;
    }
    g_hp.initialized = false;
    g_hp.rpc_host[0] = '\0';
    g_hp.rpc_port = 0;
    g_hp.rpc_user[0] = '\0';
    g_hp.rpc_password[0] = '\0';
    g_hp.cadence_secs = 0;
    g_hp.batch_size = 0;
    g_hp.lag_threshold = 0;
    g_hp.ms = NULL;
    g_hp.params = NULL;
    atomic_store(&g_hp.calls_total, 0);
    atomic_store(&g_hp.headers_added, 0);
    atomic_store(&g_hp.headers_rejected, 0);
    atomic_store(&g_hp.rpc_errors, 0);
    atomic_store(&g_hp.last_remote_height, 0);
    atomic_store(&g_hp.last_local_height, 0);
    pthread_mutex_unlock(&g_hp.lock);
}

/* ── State dump (see CLAUDE.md "Adding state introspection") ───── */

bool header_probe_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out) return false;
    struct header_probe_stats s;
    header_probe_stats_snapshot(&s);

    pthread_mutex_lock(&g_hp.lock);
    bool running   = (g_hp.health_id != HEALTH_INVALID_ID);
    int cad        = g_hp.cadence_secs;
    int batch      = g_hp.batch_size;
    int lag        = g_hp.lag_threshold;
    int port       = g_hp.rpc_port;
    char host[64];
    snprintf(host, sizeof(host), "%s", g_hp.rpc_host);
    bool have_user = g_hp.rpc_user[0] != '\0';
    bool have_pass = g_hp.rpc_password[0] != '\0';
    bool initialized = g_hp.initialized;
    pthread_mutex_unlock(&g_hp.lock);

    json_push_kv_bool(out, "running",            running);
    json_push_kv_bool(out, "initialized",        initialized);
    json_push_kv_str (out, "rpc_host",           host);
    json_push_kv_int (out, "rpc_port",           port);
    json_push_kv_bool(out, "have_user",          have_user);
    json_push_kv_bool(out, "have_password",      have_pass);
    json_push_kv_int (out, "cadence_secs",       cad);
    json_push_kv_int (out, "batch_size",         batch);
    json_push_kv_int (out, "lag_threshold",      lag);
    json_push_kv_int (out, "calls_total",        s.calls_total);
    json_push_kv_int (out, "headers_added",      s.headers_added);
    json_push_kv_int (out, "headers_rejected",   s.headers_rejected);
    json_push_kv_int (out, "rpc_errors",         s.rpc_errors);
    json_push_kv_int (out, "last_remote_height", s.last_remote_height);
    json_push_kv_int (out, "last_local_height",  s.last_local_height);
    return true;
}
