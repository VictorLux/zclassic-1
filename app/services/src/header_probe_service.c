/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Header Probe Service. See header for the high-level rationale.
 *
 * Layout:
 *   1. Config + creds
 *   2. Shared legacy JSON-RPC transport wrappers
 *   3. Build JSON-RPC requests
 *   4. header_probe_pull_range() — fetch + validate + insert
 *   5. header_probe_tick_once() — scheduler-independent Job body
 *   6. init + stats snapshot + dump_state_json
 *
 * Threading: the service creates no background work. The supervised
 * header_probe_poll Job owns cadence and calls header_probe_tick_once().
 */

#include "services/header_probe_service.h"

#include "platform/clock.h"
#include "services/header_admit_inbox.h"
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
#include "rpc/legacy_rpc_client.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* ── Constants ─────────────────────────────────────────────────── */

#define HP_DEFAULT_HOST          "127.0.0.1"
#define HP_DEFAULT_PORT          8232
#define HP_DEFAULT_CADENCE       30
#define HP_DEFAULT_BATCH         2000
#define HP_DEFAULT_LAG           100
#define HP_MAX_BATCH             5000
#define HP_RESPONSE_MAX          16384    /* a full hex-header is ~3 KB */
#define HP_MAX_HEADER_BYTES      (BLOCK_HEADER_SIZE + MAX_SOLUTION_SIZE + 8)

/* JSON-RPC batch size — number of {getblockhash,...} or {getblockheader,...}
 * items posted in a single HTTP request. zclassicd accepts JSON-RPC array
 * bodies and replies with one result per element in order. With N=128 we
 * cut N×2 round-trips (today: 27 500 RTTs for 13 750 blocks) down to
 * 2*ceil(N/128) (today: 215 RTTs). Cap chosen to keep response < ~600 KB
 * (each verbose=false header hex is ~3 KB on this chain). */
#define HP_RPC_BATCH             128

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
};

/* ── Shared legacy JSON-RPC transport wrappers ─────────────────── */

static bool hp_http_rpc_call(const char *host, int port,
                             const char *user, const char *pass,
                             const char *body_json,
                             char *resp, size_t resp_cap,
                             char *err, size_t err_sz)
{
    char *raw = NULL;
    if (!legacy_rpc_call(host, port, user, pass, body_json,
                         &raw, err, err_sz)) {
        return false;
    }

    size_t n = strlen(raw);
    if (n + 1 > resp_cap) {
        snprintf(err, err_sz, "response too large (%zu > %zu)", n, resp_cap);
        free(raw);
        return false;
    }
    memcpy(resp, raw, n + 1);
    free(raw);
    return true;
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
    bool parsed = legacy_rpc_parse_result_int(resp, &h, err, err_sz);
    free(resp);
    if (!parsed || h < 0 || h > 0x7fffffff) {
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
    if (!legacy_rpc_parse_result_string(resp, hash_hex, sizeof(hash_hex),
                                        err, err_sz)) {
        free(resp);
        return false;
    }
    if (strlen(hash_hex) != 64) {
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
    bool parsed_hex = legacy_rpc_parse_result_string(resp, hex, hex_cap,
                                                     err, err_sz);
    free(resp);
    if (!parsed_hex) {
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

/* Fetch N consecutive headers starting at from_h using 2 batched
 * JSON-RPC arrays: getblockhash × N, then getblockheader × N. Returns
 * the count of successfully-deserialized headers in *out_count. On
 * RPC or parse failure returns false; on per-header deserialize
 * failure populates whatever headers parsed successfully and returns
 * true with *out_count < n. */
static bool hp_fetch_headers_batch(const char *host, int port,
                                    const char *user, const char *pass,
                                    int from_h, int n,
                                    struct block_header *out,
                                    int *out_count,
                                    char *err, size_t err_sz)
{
    *out_count = 0;
    if (n <= 0 || n > HP_RPC_BATCH) {
        snprintf(err, err_sz, "bad batch size %d", n);
        return false;
    }

    /* ── Batch 1: getblockhash([h..h+n-1]) ───────────────────────── */
    /* Body cap: each item ~95 chars; +brackets +commas. */
    size_t body1_cap = (size_t)n * 96 + 16;
    char *body1 = zcl_malloc(body1_cap, "hp_batch_body1");
    if (!body1) {
        snprintf(err, err_sz, "oom body1");
        return false;
    }
    size_t off = 0;
    body1[off++] = '[';
    for (int i = 0; i < n; i++) {
        int w = snprintf(body1 + off, body1_cap - off,
            "%s{\"jsonrpc\":\"1.0\",\"id\":%d,\"method\":\"getblockhash\","
            "\"params\":[%d]}",
            i ? "," : "", i, from_h + i);
        if (w < 0 || (size_t)w >= body1_cap - off) {
            free(body1);
            snprintf(err, err_sz, "body1 overflow at i=%d", i);
            return false;
        }
        off += (size_t)w;
    }
    if (off + 2 >= body1_cap) {
        free(body1);
        snprintf(err, err_sz, "body1 trailer overflow");
        return false;
    }
    body1[off++] = ']';
    body1[off]   = '\0';

    char *resp1 = NULL;
    if (!legacy_rpc_call(host, port, user, pass, body1,
                         &resp1, err, err_sz)) {
        free(body1);
        return false;
    }
    free(body1);

    /* Each hash hex is 64 chars + NUL. Reserve 80 per slot for slack. */
    enum { HP_HASH_SLOT = 80 };
    char *hashes = zcl_calloc((size_t)n, HP_HASH_SLOT, "hp_batch_hashes");
    if (!hashes) {
        free(resp1);
        snprintf(err, err_sz, "oom hashes");
        return false;
    }
    bool ok = legacy_rpc_parse_result_string_array(resp1, n, hashes,
                                                   HP_HASH_SLOT,
                                                   err, err_sz);
    free(resp1);
    if (!ok) {
        free(hashes);
        return false;
    }
    for (int i = 0; i < n; i++) {
        const char *h_str = hashes + (size_t)i * HP_HASH_SLOT;
        if (strlen(h_str) != 64) {
            snprintf(err, err_sz,
                     "hash[%d] not 64 hex chars (got %zu)",
                     i, strlen(h_str));
            free(hashes);
            return false;
        }
    }

    /* ── Batch 2: getblockheader(hash[i], false) ─────────────────── */
    /* Per-item template body grows by hash length (64) + method name
     * (15) + JSON-RPC envelope (~40) + id digits + slack. 200 chars
     * per item is a safe upper bound for batch sizes up to 1024. */
    size_t body2_cap = (size_t)n * 200 + 16;
    char *body2 = zcl_malloc(body2_cap, "hp_batch_body2");
    if (!body2) {
        free(hashes);
        snprintf(err, err_sz, "oom body2");
        return false;
    }
    off = 0;
    body2[off++] = '[';
    for (int i = 0; i < n; i++) {
        const char *h_str = hashes + (size_t)i * HP_HASH_SLOT;
        int w = snprintf(body2 + off, body2_cap - off,
            "%s{\"jsonrpc\":\"1.0\",\"id\":%d,"
            "\"method\":\"getblockheader\","
            "\"params\":[\"%s\",false]}",
            i ? "," : "", i, h_str);
        if (w < 0 || (size_t)w >= body2_cap - off) {
            free(hashes); free(body2);
            snprintf(err, err_sz, "body2 overflow at i=%d", i);
            return false;
        }
        off += (size_t)w;
    }
    free(hashes);
    if (off + 2 >= body2_cap) {
        free(body2);
        snprintf(err, err_sz, "body2 trailer overflow");
        return false;
    }
    body2[off++] = ']';
    body2[off]   = '\0';

    char *resp2 = NULL;
    if (!legacy_rpc_call(host, port, user, pass, body2,
                         &resp2, err, err_sz)) {
        free(body2);
        return false;
    }
    free(body2);

    /* Each header hex is up to 2 * HP_MAX_HEADER_BYTES + slack. */
    const size_t hex_slot = (size_t)HP_MAX_HEADER_BYTES * 2 + 16;
    char *hexes = zcl_calloc((size_t)n, hex_slot, "hp_batch_hexes");
    if (!hexes) {
        free(resp2);
        snprintf(err, err_sz, "oom hexes");
        return false;
    }
    ok = legacy_rpc_parse_result_string_array(resp2, n, hexes, hex_slot,
                                              err, err_sz);
    free(resp2);
    if (!ok) {
        free(hexes);
        return false;
    }

    /* ── Per-item: hex decode + deserialize. ────────────────────── */
    int parsed = 0;
    for (int i = 0; i < n; i++) {
        const char *hex = hexes + (size_t)i * hex_slot;
        size_t hex_len = strlen(hex);
        if (hex_len < 280 || (hex_len % 2) != 0) {
            snprintf(err, err_sz, "header[%d]: bad hex length %zu",
                     i, hex_len);
            break;
        }
        unsigned char *bytes = zcl_malloc(hex_len / 2, "hp_batch_bytes");
        if (!bytes) {
            snprintf(err, err_sz, "header[%d]: oom decode", i);
            break;
        }
        size_t nbytes = ParseHex(hex, bytes, hex_len / 2);
        if (nbytes < BLOCK_HEADER_SIZE) {
            free(bytes);
            snprintf(err, err_sz, "header[%d]: short decoded len %zu",
                     i, nbytes);
            break;
        }
        struct byte_stream s;
        stream_init_from_data(&s, bytes, nbytes);
        block_header_init(&out[i]);
        bool deser_ok = block_header_deserialize(&out[i], &s);
        stream_free(&s);
        free(bytes);
        if (!deser_ok) {
            snprintf(err, err_sz, "header[%d]: deserialize failed", i);
            break;
        }
        parsed++;
    }
    free(hexes);
    *out_count = parsed;
    return true;
}

static void hp_publish_header_admit(const struct block_index *pindex)
{
    if (!pindex || !pindex->phashBlock)
        return;

    struct header_admit_msg msg = {
        .height = pindex->nHeight,
        .hash = *pindex->phashBlock,
        .peer_id = 0,
        .observed_unix = clock_now_wall_ms() / 1000,
    };
    if (!mailbox_header_admit_push(&msg)) {
        fprintf(stderr,  // obs-ok:header-probe-header-admit-inbox-full
                "[header_probe] header_admit inbox full; drop height=%d\n",
                pindex->nHeight);
    }
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
    int h = start_height;
    /* Batched fast path: fetch HP_RPC_BATCH headers per pair of RPCs
     * via JSON-RPC array. ~100× fewer round-trips than the single-call
     * path when zclassicd is on the same host. Per-item deserialize +
     * accept still validates PoW + chain link locally. */
    struct block_header *hbuf =
        zcl_malloc(sizeof(*hbuf) * HP_RPC_BATCH, "hp_pullrange_hbuf");
    if (!hbuf) {
        LOG_FAIL("header_probe", "pull_range: oom hbuf");
    }

    while (h <= end_height) {
        int n = end_height - h + 1;
        if (n > HP_RPC_BATCH) n = HP_RPC_BATCH;

        int parsed = 0;
        if (!hp_fetch_headers_batch(host, port, user, pass,
                                     h, n, hbuf, &parsed,
                                     err, sizeof(err))) {
            atomic_fetch_add(&g_hp.rpc_errors, 1);
            /* Batch failed — fall back to single-call for this one
             * header so we still make some progress and surface a
             * precise error message. */
            struct block_header hdr;
            if (!hp_fetch_one_header(host, port, user, pass, h,
                                      &hdr, err, sizeof(err))) {
                atomic_fetch_add(&g_hp.rpc_errors, 1);
                break;
            }
            struct validation_state vs;
            validation_state_init(&vs);
            struct block_index *pindex = NULL;
            if (accept_block_header(&hdr, &vs, ms, params, &pindex)) {
                atomic_fetch_add(&g_hp.headers_added, 1);
                added++;
                hp_publish_header_admit(pindex);
                if (pindex && pindex->nHeight > 0)
                    atomic_store(&g_hp.last_local_height,
                                 pindex->nHeight);
                h++;
                continue;
            }
            atomic_fetch_add(&g_hp.headers_rejected, 1);
            break;
        }

        bool reject = false;
        for (int i = 0; i < parsed; i++) {
            struct validation_state vs;
            validation_state_init(&vs);
            struct block_index *pindex = NULL;
            if (accept_block_header(&hbuf[i], &vs, ms, params, &pindex)) {
                atomic_fetch_add(&g_hp.headers_added, 1);
                added++;
                hp_publish_header_admit(pindex);
                if (pindex && pindex->nHeight > 0)
                    atomic_store(&g_hp.last_local_height,
                                 pindex->nHeight);
            } else {
                atomic_fetch_add(&g_hp.headers_rejected, 1);
                reject = true;
                break;
            }
        }
        if (reject) break;
        if (parsed < n) {
            /* Partial batch — surface per-item decode/deserialize
             * failures as RPC errors so callers see the same
             * "something went wrong" signal as the single-call path. */
            atomic_fetch_add(&g_hp.rpc_errors, (int64_t)(n - parsed));
            break;
        }
        h += parsed;
    }
    free(hbuf);

    if (out_added) *out_added = added;
    return true;
}

/* ── Boot-time blocking range pull (T1.1) ──────────────────────────
 *
 * Repeatedly calls header_probe_pull_range until we reach remote tip
 * or pull_range stops adding rows. Used by local_chain_ingest's
 * phase-3 prelude so block_index covers anchor+1..remote_tip BEFORE
 * the per-block walk starts — removing the prior dependency on P2P
 * headers arriving first.
 *
 * Bounded retries on transient zero-add: 3 strikes and we bail to let
 * the caller fall back to P2P. */
bool header_probe_pull_range_blocking(int from_height,
                                      int *out_total_added,
                                      int *out_remote_tip)
{
    if (out_total_added) *out_total_added = 0;
    if (out_remote_tip)  *out_remote_tip  = -1;
    if (from_height < 0) {
        LOG_FAIL("header_probe", "blocking: bad from_height=%d", from_height);
    }

    pthread_mutex_lock(&g_hp.lock);
    bool inited = g_hp.initialized && g_hp.ms && g_hp.params;
    char host[64], user[64], pass[128];
    int port = 0;
    if (inited) {
        snprintf(host, sizeof(host), "%s",
                 g_hp.rpc_host[0] ? g_hp.rpc_host : HP_DEFAULT_HOST);
        port = g_hp.rpc_port ? g_hp.rpc_port : HP_DEFAULT_PORT;
        snprintf(user, sizeof(user), "%s", g_hp.rpc_user);
        snprintf(pass, sizeof(pass), "%s", g_hp.rpc_password);
    }
    pthread_mutex_unlock(&g_hp.lock);
    if (!inited) {
        LOG_FAIL("header_probe", "blocking: not initialized");
    }

    int remote_tip = -1;
    char err[160] = {0};
    if (!hp_fetch_remote_tip(host, port, user, pass, &remote_tip,
                             err, sizeof(err))) {
        atomic_fetch_add(&g_hp.rpc_errors, 1);
        return false;
    }
    atomic_store(&g_hp.last_remote_height, remote_tip);
    if (out_remote_tip) *out_remote_tip = remote_tip;

    int cursor = from_height;
    int total = 0;
    int zero_streak = 0;
    while (cursor <= remote_tip) {
        int want = remote_tip - cursor + 1;
        if (want > HP_MAX_BATCH) want = HP_MAX_BATCH;
        int added = 0;
        if (!header_probe_pull_range(cursor, want, &added)) {
            break;
        }
        if (added == 0) {
            if (++zero_streak >= 3) break;
            continue;
        }
        zero_streak = 0;
        cursor += added;
        total += added;
    }
    if (out_total_added) *out_total_added = total;
    return cursor > remote_tip;
}

/* ── Poll tick body used by the supervised header_probe_poll Job ── */

void header_probe_tick_once(void)
{
    pthread_mutex_lock(&g_hp.lock);
    bool inited = g_hp.initialized;
    struct main_state *ms = g_hp.ms;
    int lag_thresh = g_hp.lag_threshold > 0
                         ? g_hp.lag_threshold : HP_DEFAULT_LAG;
    int batch = g_hp.batch_size > 0 ? g_hp.batch_size : HP_DEFAULT_BATCH;
    char host[64], user[64], pass[128];
    int port;
    snprintf(host, sizeof(host), "%s",
             g_hp.rpc_host[0] ? g_hp.rpc_host : HP_DEFAULT_HOST);
    port = g_hp.rpc_port ? g_hp.rpc_port : HP_DEFAULT_PORT;
    snprintf(user, sizeof(user), "%s", g_hp.rpc_user);
    snprintf(pass, sizeof(pass), "%s", g_hp.rpc_password);
    pthread_mutex_unlock(&g_hp.lock);
    if (!inited || !ms) return;

    int local_tip = 0;
    if (ms->pindex_best_header)
        local_tip = ms->pindex_best_header->nHeight;
    else
        local_tip = active_chain_height(&ms->chain_active);
    if (local_tip < 0) local_tip = 0;

    /* Cheap getblockcount to decide whether to pull. */
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

/* ── init ──────────────────────────────────────────────────────── */

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
        if (legacy_rpc_parse_conf(u, sizeof(u), p, sizeof(p),
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

    json_push_kv_bool(out, "running",            initialized);
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
