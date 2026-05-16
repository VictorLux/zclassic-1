/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * zclassicd Oracle Service. See header for the high-level rationale.
 *
 * Layout:
 *   1. Config + creds (parse zclassic.conf)
 *   2. zclassicd_oracle_probe() — synchronous probe of one height
 *   3. on_tick() — periodic heartbeat callback (random-height fan-out)
 *   4. init/start/stop + stats snapshot + dump_state_json
 *
 * Threading: the only background work is the heartbeat sweeper, which
 * is owned by lib/health/heartbeat.c. No new pthreads are created here.
 */

#include "services/zclassicd_oracle_service.h"
#include "services/oracle_policy.h"

#include "validation/main_state.h"
#include "validation/chainstate.h"
#include "chain/chain.h"
#include "core/uint256.h"
#include "core/random.h"
#include "controllers/wallet_helpers.h"
#include "json/json.h"
#include "event/event.h"
#include "health/heartbeat.h"
#include "rpc/legacy_rpc_client.h"
#include "util/log_macros.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>

/* ── Constants ─────────────────────────────────────────────────── */

#define ORACLE_DEFAULT_HOST          "127.0.0.1"
#define ORACLE_DEFAULT_PORT          8232
#define ORACLE_DEFAULT_CADENCE       60
#define ORACLE_DEFAULT_HEIGHTS_TICK  3
#define ORACLE_TIP_SAFETY_MARGIN     100  /* avoid races at the tip */

/* ── Global state ──────────────────────────────────────────────── */

static struct {
    pthread_mutex_t lock;       /* guards config + stats */
    bool   initialized;
    char   rpc_host[64];
    int    rpc_port;
    char   rpc_user[64];
    char   rpc_password[128];
    int    cadence_secs;
    int    heights_per_tick;
    health_subsystem_id health_id;

    /* Stats */
    _Atomic int64_t probes_total;
    _Atomic int64_t probes_agree;
    _Atomic int64_t probes_disagree;
    _Atomic int64_t rpc_errors;
    _Atomic int64_t last_probe_unix_us;
    _Atomic int     last_probed_height;
} g_oracle = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .health_id = HEALTH_INVALID_ID,
};

/* Split HTTP head/body and parse JSON-RPC result. Returns the hex
 * string from .result on success. On error, writes a message to err.
 * out_hex must be >= 65 bytes. */
static bool parse_rpc_hex_result(const char *raw, char *out_hex,
                                 char *err, size_t err_sz)
{
    const char *body = strstr(raw, "\r\n\r\n");
    if (!body) {
        snprintf(err, err_sz, "no http body separator");
        return false;
    }
    body += 4;

    struct json_value v = {0};
    if (!json_read(&v, body, strlen(body))) {
        snprintf(err, err_sz, "json parse failed");
        json_free(&v);
        return false;
    }
    const struct json_value *result = json_get(&v, "result");
    if (!result || result->type != JSON_STR) {
        /* Try error.message for diagnostics */
        const struct json_value *jerr = json_get(&v, "error");
        if (jerr && jerr->type == JSON_OBJ) {
            const struct json_value *msg = json_get(jerr, "message");
            if (msg && msg->type == JSON_STR) {
                snprintf(err, err_sz, "rpc error: %s", json_get_str(msg));
                json_free(&v);
                return false;
            }
        }
        snprintf(err, err_sz, "no .result or not a string");
        json_free(&v);
        return false;
    }
    const char *s = json_get_str(result);
    size_t slen = s ? strlen(s) : 0;
    if (slen != 64) {
        snprintf(err, err_sz, "result not 64 hex chars (got %zu)", slen);
        json_free(&v);
        return false;
    }
    memcpy(out_hex, s, 64);
    out_hex[64] = '\0';
    json_free(&v);
    return true;
}

/* ── Our-side block lookup ─────────────────────────────────────── */

static bool our_hash_at_height(int height, char out_hex[65])
{
    struct main_state *ms = wallet_rpc_main_state();
    if (!ms) {
        out_hex[0] = '\0';
        return false;
    }
    struct block_index *bi = active_chain_at(&ms->chain_active, height);
    if (!bi || !bi->phashBlock) {
        out_hex[0] = '\0';
        return false;
    }
    uint256_get_hex(bi->phashBlock, out_hex);
    return true;
}

static int our_tip_height(void)
{
    struct main_state *ms = wallet_rpc_main_state();
    if (!ms) return -1; /* raw-return-ok:pre-boot-sentinel */
    return active_chain_height(&ms->chain_active);
}

/* ── Public probe ──────────────────────────────────────────────── */

bool zclassicd_oracle_probe(int height,
                            struct zclassicd_oracle_probe_result *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->height = height;

    if (height < 0) {
        out->error = true;
        snprintf(out->error_msg, sizeof(out->error_msg),
                 "negative height %d", height);
        return false;
    }

    /* Snapshot config */
    char host[64], user[64], pass[128];
    int port;
    pthread_mutex_lock(&g_oracle.lock);
    snprintf(host, sizeof(host), "%s",
             g_oracle.rpc_host[0] ? g_oracle.rpc_host : ORACLE_DEFAULT_HOST);
    port = g_oracle.rpc_port ? g_oracle.rpc_port : ORACLE_DEFAULT_PORT;
    snprintf(user, sizeof(user), "%s", g_oracle.rpc_user);
    snprintf(pass, sizeof(pass), "%s", g_oracle.rpc_password);
    pthread_mutex_unlock(&g_oracle.lock);

    /* Build JSON-RPC body */
    char body[256];
    snprintf(body, sizeof(body),
        "{\"jsonrpc\":\"1.0\",\"id\":\"zcl-oracle\","
        "\"method\":\"getblockhash\",\"params\":[%d]}", height);

    char *resp = NULL;
    if (!legacy_rpc_call(host, port, user, pass, body, &resp,
                         out->error_msg, sizeof(out->error_msg))) {
        out->error = true;
        atomic_fetch_add(&g_oracle.rpc_errors, 1);
        atomic_store(&g_oracle.last_probed_height, height);
        struct timeval tv;
        gettimeofday(&tv, NULL);
        atomic_store(&g_oracle.last_probe_unix_us,
                     (int64_t)tv.tv_sec * 1000000 + tv.tv_usec);
        return true;
    }

    if (!parse_rpc_hex_result(resp, out->their_hash,
                              out->error_msg, sizeof(out->error_msg))) {
        free(resp);
        out->error = true;
        atomic_fetch_add(&g_oracle.rpc_errors, 1);
        return true;
    }
    free(resp);

    /* Our-side lookup */
    out->our_have_block = our_hash_at_height(height, out->our_hash);
    out->match = out->our_have_block &&
                 strcasecmp(out->our_hash, out->their_hash) == 0;

    /* Stats */
    atomic_fetch_add(&g_oracle.probes_total, 1);
    atomic_store(&g_oracle.last_probed_height, height);
    struct timeval tv;
    gettimeofday(&tv, NULL);
    atomic_store(&g_oracle.last_probe_unix_us,
                 (int64_t)tv.tv_sec * 1000000 + tv.tv_usec);
    if (out->our_have_block) {
        if (out->match) {
            atomic_fetch_add(&g_oracle.probes_agree, 1);
            event_emitf(EV_ORACLE_AGREE, 0,
                        "h=%d hash=%s", height, out->their_hash);
        } else {
            atomic_fetch_add(&g_oracle.probes_disagree, 1);
            event_emitf(EV_ORACLE_DISAGREE, 0,
                        "h=%d our=%s their=%s",
                        height, out->our_hash, out->their_hash);
            /* T2.1: feed the policy state machine. It decides whether
             * to halt new block acceptance or panic. */
            oracle_policy_record_disagreement(height,
                                              out->our_hash,
                                              out->their_hash);
        }
    }
    /* If we don't have the block locally, neither agree nor disagree;
     * still counted in probes_total. */
    return true;
}

/* ── Periodic tick (heartbeat callback) ────────────────────────── */

static void zclassicd_oracle_on_tick(void *ctx)
{
    (void)ctx;
    int tip = our_tip_height();
    int margin = ORACLE_TIP_SAFETY_MARGIN;
    /* Tests can lower the margin so a tiny synthetic chain still
     * exercises the periodic-tick path. Production code never sets
     * this env var. */
    const char *m = getenv("ZCL_ORACLE_TIP_MARGIN");
    if (m && m[0]) {
        int n = atoi(m);
        if (n >= 0) margin = n;
    }
    int max_h = tip - margin;
    if (max_h <= 0) return;  /* not synced yet */

    int n = g_oracle.heights_per_tick > 0
                ? g_oracle.heights_per_tick : ORACLE_DEFAULT_HEIGHTS_TICK;
    for (int i = 0; i < n; i++) {
        int h = GetRandInt(max_h + 1);
        struct zclassicd_oracle_probe_result r;
        (void)zclassicd_oracle_probe(h, &r);
    }
}

/* ── init / start / stop ───────────────────────────────────────── */

bool zclassicd_oracle_init(const struct zclassicd_oracle_config *cfg)
{
    pthread_mutex_lock(&g_oracle.lock);

    snprintf(g_oracle.rpc_host, sizeof(g_oracle.rpc_host), "%s",
             (cfg && cfg->rpc_host) ? cfg->rpc_host : ORACLE_DEFAULT_HOST);
    g_oracle.rpc_port = (cfg && cfg->rpc_port > 0)
                            ? cfg->rpc_port : ORACLE_DEFAULT_PORT;
    g_oracle.cadence_secs = (cfg && cfg->cadence_secs > 0)
                            ? cfg->cadence_secs : ORACLE_DEFAULT_CADENCE;
    g_oracle.heights_per_tick = (cfg && cfg->heights_per_tick > 0)
                            ? cfg->heights_per_tick : ORACLE_DEFAULT_HEIGHTS_TICK;

    /* Credentials: caller-provided beats zclassic.conf. */
    if (cfg && cfg->rpc_user && cfg->rpc_user[0]) {
        snprintf(g_oracle.rpc_user, sizeof(g_oracle.rpc_user),
                 "%s", cfg->rpc_user);
    }
    if (cfg && cfg->rpc_password && cfg->rpc_password[0]) {
        snprintf(g_oracle.rpc_password, sizeof(g_oracle.rpc_password),
                 "%s", cfg->rpc_password);
    }

    bool need_user = (g_oracle.rpc_user[0] == '\0');
    bool need_pass = (g_oracle.rpc_password[0] == '\0');
    if (need_user || need_pass) {
        int port_from_conf = g_oracle.rpc_port;
        char u[64] = {0}, p[128] = {0};
        if (legacy_rpc_parse_conf(u, sizeof(u), p, sizeof(p),
                                  &port_from_conf)) {
            if (need_user)
                snprintf(g_oracle.rpc_user, sizeof(g_oracle.rpc_user),
                         "%s", u);
            if (need_pass)
                snprintf(g_oracle.rpc_password,
                         sizeof(g_oracle.rpc_password), "%s", p);
            /* Don't override an explicitly-provided port. */
            if (!cfg || cfg->rpc_port <= 0)
                g_oracle.rpc_port = port_from_conf;
        } else if (need_user || need_pass) {
            pthread_mutex_unlock(&g_oracle.lock);
            LOG_FAIL("oracle",
                     "no RPC credentials: pass via config or ~/.zclassic/zclassic.conf");
        }
    }

    g_oracle.initialized = true;
    pthread_mutex_unlock(&g_oracle.lock);

    /* T2.1: ensure the policy module is ready before any disagreement
     * can be recorded. Idempotent — safe even if init runs multiple
     * times. */
    oracle_policy_init(NULL);
    return true;
}

bool zclassicd_oracle_start(void)
{
    if (g_oracle.health_id != HEALTH_INVALID_ID) return true;
    if (!g_oracle.initialized) {
        if (!zclassicd_oracle_init(NULL)) {
            LOG_FAIL("oracle", "init failed");
        }
    }
    (void)health_start();  /* idempotent */
    int cad = g_oracle.cadence_secs > 0
                  ? g_oracle.cadence_secs : ORACLE_DEFAULT_CADENCE;
    g_oracle.health_id = health_register_periodic(
        "oracle.zclassicd", cad, zclassicd_oracle_on_tick, NULL);
    if (g_oracle.health_id == HEALTH_INVALID_ID) {
        LOG_FAIL("oracle", "health_register_periodic failed");
    }
    return true;
}

void zclassicd_oracle_stop(void)
{
    if (g_oracle.health_id == HEALTH_INVALID_ID) return;
    health_unregister(g_oracle.health_id);
    g_oracle.health_id = HEALTH_INVALID_ID;
}

/* ── Stats snapshot ────────────────────────────────────────────── */

void zclassicd_oracle_stats_snapshot(struct zclassicd_oracle_stats *out)
{
    if (!out) return;
    out->probes_total       = atomic_load(&g_oracle.probes_total);
    out->probes_agree       = atomic_load(&g_oracle.probes_agree);
    out->probes_disagree    = atomic_load(&g_oracle.probes_disagree);
    out->rpc_errors         = atomic_load(&g_oracle.rpc_errors);
    out->last_probe_unix_us = atomic_load(&g_oracle.last_probe_unix_us);
    out->last_probed_height = atomic_load(&g_oracle.last_probed_height);
}

void zclassicd_oracle_reset_for_test(void)
{
    pthread_mutex_lock(&g_oracle.lock);
    if (g_oracle.health_id != HEALTH_INVALID_ID) {
        health_unregister(g_oracle.health_id);
        g_oracle.health_id = HEALTH_INVALID_ID;
    }
    g_oracle.initialized = false;
    g_oracle.rpc_host[0] = '\0';
    g_oracle.rpc_port = 0;
    g_oracle.rpc_user[0] = '\0';
    g_oracle.rpc_password[0] = '\0';
    g_oracle.cadence_secs = 0;
    g_oracle.heights_per_tick = 0;
    atomic_store(&g_oracle.probes_total, 0);
    atomic_store(&g_oracle.probes_agree, 0);
    atomic_store(&g_oracle.probes_disagree, 0);
    atomic_store(&g_oracle.rpc_errors, 0);
    atomic_store(&g_oracle.last_probe_unix_us, 0);
    atomic_store(&g_oracle.last_probed_height, 0);
    pthread_mutex_unlock(&g_oracle.lock);
}

/* ── State dump (see CLAUDE.md "Adding state introspection") ───── */

bool zclassicd_oracle_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out) return false;
    struct zclassicd_oracle_stats s;
    zclassicd_oracle_stats_snapshot(&s);

    pthread_mutex_lock(&g_oracle.lock);
    bool running   = (g_oracle.health_id != HEALTH_INVALID_ID);
    int cad        = g_oracle.cadence_secs;
    int hpt        = g_oracle.heights_per_tick;
    int port       = g_oracle.rpc_port;
    char host[64];
    snprintf(host, sizeof(host), "%s", g_oracle.rpc_host);
    bool have_user = g_oracle.rpc_user[0] != '\0';
    bool have_pass = g_oracle.rpc_password[0] != '\0';
    pthread_mutex_unlock(&g_oracle.lock);

    json_push_kv_bool(out, "running",         running);
    json_push_kv_bool(out, "initialized",     g_oracle.initialized);
    json_push_kv_str (out, "rpc_host",        host);
    json_push_kv_int (out, "rpc_port",        port);
    json_push_kv_bool(out, "have_user",       have_user);
    json_push_kv_bool(out, "have_password",   have_pass);
    json_push_kv_int (out, "cadence_secs",    cad);
    json_push_kv_int (out, "heights_per_tick",hpt);
    json_push_kv_int (out, "probes_total",    s.probes_total);
    json_push_kv_int (out, "probes_agree",    s.probes_agree);
    json_push_kv_int (out, "probes_disagree", s.probes_disagree);
    json_push_kv_int (out, "rpc_errors",      s.rpc_errors);
    json_push_kv_int (out, "last_probe_unix_us", s.last_probe_unix_us);
    json_push_kv_int (out, "last_probed_height", s.last_probed_height);
    return true;
}
