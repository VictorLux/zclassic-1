/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Always-on legacy mirror MONITOR. Post-B7 cutover the staged sync
 * pipeline is the authoritative block writer, so this service no
 * longer applies blocks. It only observes lag against a sibling
 * zclassicd: fetch chain-info, compute lag, evaluate the lag-SLO,
 * verify anchor/tip hashes agree, cache hashes, and surface a blocker
 * when the local tip is behind. The heartbeat, lag-SLO monitor, and
 * stats/introspection feed health, metrics, conditions, and the
 * supervisor tree.
 */

#include "platform/time_compat.h"
#include "services/legacy_mirror_sync_service.h"

#include "services/header_probe.h"
#include "services/chain_activation_controller.h"
#include "services/chain_tip.h"
#include "services/gap_fill_service.h"
#include "services/oracle_policy.h"
#include "services/snapshot_sync_service.h"
#include "services/sync_monitor.h"

#include "config/db_service.h"
#include "config/runtime.h"
#include "validation/main_state.h"
#include "validation/chainstate.h"
#include "validation/mirror_consensus.h"
#include "validation/process_block.h"
#include "chain/chain.h"
#include "chain/chainparams.h"
#include "coins/coins_view.h"
#include "core/utiltime.h"
#include "core/uint256.h"
#include "json/json.h"
#include "health/heartbeat.h"
#include "models/database.h"
#include "rpc/legacy_rpc_client.h"
#include "util/log_macros.h"
#include "event/event.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define LMS_DEFAULT_HOST       "127.0.0.1"
#define LMS_DEFAULT_PORT       8232
#define LMS_DEFAULT_CADENCE    3
#define LMS_DEFAULT_MAX_BLOCKS 64
#define LMS_DEFAULT_LAG_SLA    1
#define LMS_DEFAULT_LAG_SLA_BREACH_BLOCKS   10
#define LMS_DEFAULT_LAG_SLA_BREACH_SECS     60
#define LMS_DEFAULT_LAG_SLA_CRITICAL_BLOCKS 100
#define LMS_DEFAULT_LAG_SLA_CRITICAL_SECS   300
#define LMS_HEADER_DRAIN_BATCH              5000

static struct {
    pthread_mutex_t lock;
    pthread_mutex_t flight;
    bool initialized;
    bool enabled;
    char rpc_host[64];
    int  rpc_port;
    char rpc_user[64];
    char rpc_password[128];
    int  cadence_secs;
    int  max_blocks_tick;
    int  lag_sla;
    int  lag_sla_breach_blocks;
    int  lag_sla_breach_secs;
    int  lag_sla_critical_blocks;
    int  lag_sla_critical_secs;
    /* SLO breach episode timestamps. Set when lag first crosses threshold;
     * cleared (=0) when lag drops below. Episode = continuous span above. */
    _Atomic int64_t lag_breach_since;
    _Atomic int64_t lag_critical_since;
    /* Latched "emitted this episode" guards — once per severity per episode. */
    _Atomic int lag_breach_emitted;
    _Atomic int lag_critical_emitted;
    char datadir[1024];
    health_subsystem_id health_id;
    struct main_state *ms;
    struct coins_view_cache *coins_tip;
    const struct chain_params *params;

    _Atomic int reachable;
    _Atomic int in_flight;
    _Atomic int legacy_height;
    _Atomic int legacy_headers;
    _Atomic int local_height;
    _Atomic int best_header_height;
    _Atomic int target_height;
    _Atomic int authority_rewind_target;
    _Atomic int csr_sqlite_rc;
    _Atomic int64_t no_authorized_child_first_seen;
    _Atomic int last_advanced_height;
    _Atomic int last_progress_blocks;
    _Atomic int stuck_height;
    _Atomic unsigned int stuck_status_flags;
    _Atomic int64_t stalls_total;
    _Atomic int64_t last_catchup;
    _Atomic int64_t last_attempt;
    _Atomic int64_t catchups_total;
    _Atomic int64_t rpc_errors;
    _Atomic int64_t blocks_applied;
    _Atomic int64_t headers_added;
    char zclassic23_hash[65];
    char zclassicd_hash[65];
    char stuck_reason[64];
    char last_blocker_code[64];
    char csr_failure_reason[160];
    char last_error[160];
} g_lms = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .flight = PTHREAD_MUTEX_INITIALIZER,
    .health_id = HEALTH_INVALID_ID,
};

#ifdef ZCL_TESTING
static bool g_lms_test_fake_running;
static _Atomic int g_lms_test_catchup_enabled;
static _Atomic int g_lms_test_catchup_result;
static _Atomic int g_lms_test_catchup_clear_stuck;
static _Atomic int g_lms_test_catchup_calls;
#endif

static void lms_set_error(const char *msg)
{
    pthread_mutex_lock(&g_lms.lock);
    snprintf(g_lms.last_error, sizeof(g_lms.last_error), "%s",
             msg ? msg : "");
    pthread_mutex_unlock(&g_lms.lock);
}

static void lms_set_blocker(const char *code, const char *msg)
{
    pthread_mutex_lock(&g_lms.lock);
    snprintf(g_lms.last_blocker_code, sizeof(g_lms.last_blocker_code),
             "%s", code ? code : "");
    snprintf(g_lms.last_error, sizeof(g_lms.last_error), "%s",
             msg ? msg : "");
    pthread_mutex_unlock(&g_lms.lock);
    if (code && code[0])
        mirror_consensus_record_blocker(code);
}

static void lms_clear_csr_failure(void)
{
    atomic_store(&g_lms.csr_sqlite_rc, 0);
    pthread_mutex_lock(&g_lms.lock);
    g_lms.csr_failure_reason[0] = '\0';
    pthread_mutex_unlock(&g_lms.lock);
}

static void lms_set_stuck_reason(const char *reason)
{
    bool count_stall = reason && reason[0];
    pthread_mutex_lock(&g_lms.lock);
    if (count_stall && strcmp(g_lms.stuck_reason, reason) == 0)
        count_stall = false;
    snprintf(g_lms.stuck_reason, sizeof(g_lms.stuck_reason),
             "%s", reason ? reason : "");
    pthread_mutex_unlock(&g_lms.lock);
    if (count_stall)
        atomic_fetch_add(&g_lms.stalls_total, 1);
}

static void lms_clear_blocker(void)
{
    pthread_mutex_lock(&g_lms.lock);
    g_lms.last_blocker_code[0] = '\0';
    g_lms.last_error[0] = '\0';
    pthread_mutex_unlock(&g_lms.lock);
    /* mirror_consensus_clear_blocker was deleted in F-1e — the
     * mirror.* blocker IDs that wired to the typed blocker registry
     * via mirror_consensus_record_blocker auto-expire via
     * BLOCKER_TRANSIENT classification. The cryptographic-mismatch
     * (PERMANENT) blockers were never intended to auto-clear. */
}

static int lms_env_int(const char *name, int fallback, int min, int max)
{
    const char *s = getenv(name);
    if (!s || !s[0]) return fallback;
    int n = atoi(s);
    if (n < min) return fallback;
    if (n > max) return max;
    return n;
}

static bool lms_env_disabled(void)
{
    const char *s = getenv("ZCL_MIRROR_SYNC");
    return s && (!strcmp(s, "0") || !strcasecmp(s, "false") ||
                 !strcasecmp(s, "off") || !strcasecmp(s, "no"));
}

static bool lms_parse_int_result(const char *raw, const char *key,
                                 int *out, char *err, size_t err_sz)
{
    const char *body = legacy_rpc_http_body(raw);
    if (!body) {
        snprintf(err, err_sz, "no http body separator");
        return false;
    }
    struct json_value v = {0};
    if (!json_read(&v, body, strlen(body))) {
        snprintf(err, err_sz, "json parse failed");
        json_free(&v);
        return false;
    }
    const struct json_value *result = json_get(&v, "result");
    const struct json_value *field = key && result ? json_get(result, key)
                                                   : result;
    if (!field || field->type != JSON_INT) {
        snprintf(err, err_sz, "missing int result%s%s",
                 key ? "." : "", key ? key : "");
        json_free(&v);
        return false;
    }
    int64_t n = json_get_int(field);
    if (n < 0 || n > 0x7fffffff) {
        snprintf(err, err_sz, "height out of range");
        json_free(&v);
        return false;
    }
    *out = (int)n;
    json_free(&v);
    return true;
}

static bool lms_parse_hash_result(const char *raw, char out_hex[65],
                                  char *err, size_t err_sz)
{
    const char *body = legacy_rpc_http_body(raw);
    if (!body) {
        snprintf(err, err_sz, "no http body separator");
        return false;
    }
    struct json_value v = {0};
    if (!json_read(&v, body, strlen(body))) {
        snprintf(err, err_sz, "json parse failed");
        json_free(&v);
        return false;
    }
    const struct json_value *result = json_get(&v, "result");
    if (!result || result->type != JSON_STR) {
        snprintf(err, err_sz, "missing string result");
        json_free(&v);
        return false;
    }
    const char *s = json_get_str(result);
    if (!s || strlen(s) != 64) {
        snprintf(err, err_sz, "hash result is not 64 hex chars");
        json_free(&v);
        return false;
    }
    memcpy(out_hex, s, 64);
    out_hex[64] = '\0';
    json_free(&v);
    return true;
}

static bool lms_rpc_call(const char *method_params, char **out_resp,
                         char *err, size_t err_sz)
{
    char host[64], user[64], pass[128];
    int port;
    pthread_mutex_lock(&g_lms.lock);
    snprintf(host, sizeof(host), "%s", g_lms.rpc_host);
    port = g_lms.rpc_port;
    snprintf(user, sizeof(user), "%s", g_lms.rpc_user);
    snprintf(pass, sizeof(pass), "%s", g_lms.rpc_password);
    pthread_mutex_unlock(&g_lms.lock);

    return legacy_rpc_call(host, port, user, pass, method_params,
                           out_resp, err, err_sz);
}

static bool lms_fetch_chain_info(int *out_blocks, int *out_headers,
                                 char *err, size_t err_sz)
{
    char *resp = NULL;
    const char *body =
        "{\"jsonrpc\":\"1.0\",\"id\":\"zcl-mirror\","
        "\"method\":\"getblockchaininfo\",\"params\":[]}";
    if (!lms_rpc_call(body, &resp, err, err_sz))
        return false;
    int blocks = -1, headers = -1;
    bool ok_b = lms_parse_int_result(resp, "blocks", &blocks, err, err_sz);
    bool ok_h = lms_parse_int_result(resp, "headers", &headers, err, err_sz);
    free(resp);
    if (!ok_b) return false;
    if (!ok_h) headers = blocks;
    *out_blocks = blocks;
    *out_headers = headers;
    return true;
}

static bool lms_fetch_hash(int height, char out_hex[65],
                           char *err, size_t err_sz)
{
    char body[160];
    snprintf(body, sizeof(body),
        "{\"jsonrpc\":\"1.0\",\"id\":\"zcl-mirror\","
        "\"method\":\"getblockhash\",\"params\":[%d]}", height);
    char *resp = NULL;
    if (!lms_rpc_call(body, &resp, err, err_sz))
        return false;
    bool ok = lms_parse_hash_result(resp, out_hex, err, err_sz);
    free(resp);
    return ok;
}

static bool lms_local_hash_at(int height, char out_hex[65])
{
    out_hex[0] = '\0';
    struct main_state *ms = g_lms.ms;
    if (!ms || height < 0) return false;
    zcl_mutex_lock(&ms->cs_main);
    struct block_index *bi = active_chain_at(&ms->chain_active, height);
    if (!bi) {
        bi = active_chain_tip(&ms->chain_active);
        int steps = 0;
        while (bi && bi->nHeight > height && steps <= 2050) {
            bi = bi->pprev;
            steps++;
        }
        if (bi && bi->nHeight != height)
            bi = NULL;
    }
    if (bi && bi->phashBlock)
        uint256_get_hex(bi->phashBlock, out_hex);
    zcl_mutex_unlock(&ms->cs_main);
    return out_hex[0] != '\0';
}

static bool lms_verify_anchor(int height)
{
    if (height < 0) return true;
    char local[65], remote[65], err[160] = {0};
    if (!lms_local_hash_at(height, local))
        return true;
    if (!lms_fetch_hash(height, remote, err, sizeof(err))) {
        atomic_fetch_add(&g_lms.rpc_errors, 1);
        lms_set_blocker("rpc-unreachable", err);
        return false;
    }
    if (strcasecmp(local, remote) != 0) {
        oracle_policy_record_disagreement(height, local, remote);
        lms_set_blocker("hash-disagreement", "legacy hash disagreement");
        return false;
    }
    return true;
}

static bool lms_verify_after_tip(int height)
{
    char local[65], remote[65], err[160] = {0};
    if (!lms_local_hash_at(height, local)) {
        lms_set_error("local tip hash unavailable after catchup");
        return false;
    }
    if (!lms_fetch_hash(height, remote, err, sizeof(err))) {
        atomic_fetch_add(&g_lms.rpc_errors, 1);
        lms_set_blocker("rpc-unreachable", err);
        return false;
    }
    if (strcasecmp(local, remote) != 0) {
        oracle_policy_record_disagreement(height, local, remote);
        lms_set_blocker("hash-disagreement", "post-catchup tip disagreement");
        return false;
    }
    return true;
}

static void lms_cache_hashes(const char *local, const char *remote)
{
    pthread_mutex_lock(&g_lms.lock);
    if (local)
        snprintf(g_lms.zclassic23_hash, sizeof(g_lms.zclassic23_hash),
                 "%s", local);
    if (remote)
        snprintf(g_lms.zclassicd_hash, sizeof(g_lms.zclassicd_hash),
                 "%s", remote);
    pthread_mutex_unlock(&g_lms.lock);
}

static int lms_header_probe_last_local_height(void)
{
    struct json_value dump;
    json_init(&dump);
    int h = -1;
    if (header_probe_dump_state_json(&dump, NULL)) {
        const struct json_value *v = json_get(&dump, "last_local_height");
        if (v && v->type == JSON_INT)
            h = (int)json_get_int(v);
    }
    json_free(&dump);
    return h;
}

static void lms_refresh_local_heights(int *out_local, int *out_header)
{
    int local = -1, hdr = -1;
    struct main_state *ms = g_lms.ms;
    if (ms) {
        zcl_mutex_lock(&ms->cs_main);
        local = active_chain_height(&ms->chain_active);
        hdr = ms->pindex_best_header ? ms->pindex_best_header->nHeight
                                      : local;
        zcl_mutex_unlock(&ms->cs_main);
    }
    {
        int hp_height = lms_header_probe_last_local_height();
        if (hp_height > hdr)
            hdr = hp_height;
    }
    atomic_store(&g_lms.local_height, local);
    atomic_store(&g_lms.best_header_height, hdr);
    if (out_local) *out_local = local;
    if (out_header) *out_header = hdr;
}

static void lms_record_stuck_status(int height)
{
    unsigned int flags = 0;
    if (height >= 0 && g_lms.ms) {
        char remote[65] = {0};
        char err[160] = {0};
        if (lms_fetch_hash(height, remote, err, sizeof(err))) {
            struct uint256 hash;
            uint256_set_hex(&hash, remote);
            zcl_mutex_lock(&g_lms.ms->cs_main);
            struct block_index *bi =
                block_map_find(&g_lms.ms->map_block_index, &hash);
            if (!bi) {
                lms_set_stuck_reason("no-authorized-child");
            } else {
                flags = bi->nStatus;
                if (!(bi->nStatus & BLOCK_HAVE_DATA))
                    lms_set_stuck_reason("missing-have-data");
                else if (bi->nStatus & BLOCK_FAILED_MASK)
                    lms_set_stuck_reason("failed-mask");
                else
                    lms_set_stuck_reason("activation-state");
            }
            zcl_mutex_unlock(&g_lms.ms->cs_main);
        }
    }
    atomic_store(&g_lms.stuck_height, height);
    atomic_store(&g_lms.stuck_status_flags, flags);
}

static const char *lms_state_name(const struct legacy_mirror_sync_stats *s)
{
    if (!s || !s->enabled || !s->running)
        return "blocked";
    if (s->activation_blocker[0] || s->last_blocker_code[0] ||
        s->csr_sqlite_rc != 0)
        return "blocked";
    /* Concurrent redundancy: when lag breaches SLO and zclassicd is
     * reachable, the mirror is actively pulling regardless of whether
     * local recovery is "exhausted". Surface this as a distinct state
     * so operators see redundancy engaged, not gated. */
    if (s->reachable &&
        s->lag_sla_breach_blocks > 0 &&
        s->lag >= s->lag_sla_breach_blocks)
        return "concurrent_catchup";
    if (s->mirror_repair_gated_by_local_retries)
        return "gated_by_local_retries";
    if (s->reachable && s->lag < 0)
        return "observing";
    if (s->in_flight || s->last_progress_blocks > 0 || s->lag > 1)
        return "catching_up";
    if (s->reachable && s->lag <= 1)
        return "healthy";
    return "observing";
}

static bool lms_blocker_cleared_by_catchup(const char *code, int lag)
{
    return code && strcmp(code, "activation-no-progress") == 0 && lag <= 0;
}

/* Track lag-SLO breach episodes and emit EV_LAG_SLO_BREACH at most
 * once per (episode, severity). Called from the catchup tick after
 * each fresh lag reading; safe under the single-flight lock.
 *
 * Severity ladder:
 *   under breach              → severity=none, clear timers
 *   lag ≥ breach_blocks       → start/continue breach timer
 *      ≥ breach_secs sustained → severity=critical (one emit)
 *   lag ≥ critical_blocks     → start/continue critical timer
 *      ≥ critical_secs sustained → severity=fatal (one emit; node_health flips) */
static void lms_evaluate_lag_slo(int lag, int legacy_height, int local_height,
                                 int64_t now)
{
    int breach_blocks   = g_lms.lag_sla_breach_blocks;
    int breach_secs     = g_lms.lag_sla_breach_secs;
    int critical_blocks = g_lms.lag_sla_critical_blocks;
    int critical_secs   = g_lms.lag_sla_critical_secs;

    if (breach_blocks <= 0) {
        atomic_store(&g_lms.lag_breach_since, 0);
        atomic_store(&g_lms.lag_critical_since, 0);
        atomic_store(&g_lms.lag_breach_emitted, 0);
        atomic_store(&g_lms.lag_critical_emitted, 0);
        return;
    }

    if (lag < breach_blocks) {
        /* Recovered. Reset both timers + emission latches. */
        atomic_store(&g_lms.lag_breach_since, 0);
        atomic_store(&g_lms.lag_critical_since, 0);
        atomic_store(&g_lms.lag_breach_emitted, 0);
        atomic_store(&g_lms.lag_critical_emitted, 0);
        return;
    }

    int64_t since = atomic_load(&g_lms.lag_breach_since);
    if (since == 0) {
        atomic_store(&g_lms.lag_breach_since, now);
        since = now;
    }
    int64_t breach_for = now - since;

    if (breach_for >= breach_secs &&
        !atomic_exchange(&g_lms.lag_breach_emitted, 1)) {
        event_emitf(EV_LAG_SLO_BREACH, 0,
                    "lag=%d legacy_height=%d local_height=%d "
                    "since=%llds severity=critical",
                    lag, legacy_height, local_height,
                    (long long)breach_for);
    }

    if (critical_blocks > 0 && lag >= critical_blocks) {
        int64_t csince = atomic_load(&g_lms.lag_critical_since);
        if (csince == 0) {
            atomic_store(&g_lms.lag_critical_since, now);
            csince = now;
        }
        int64_t crit_for = now - csince;
        if (crit_for >= critical_secs &&
            !atomic_exchange(&g_lms.lag_critical_emitted, 1)) {
            event_emitf(EV_LAG_SLO_BREACH, 0,
                        "lag=%d legacy_height=%d local_height=%d "
                        "since=%llds severity=fatal",
                        lag, legacy_height, local_height,
                        (long long)crit_for);
        }
    } else {
        atomic_store(&g_lms.lag_critical_since, 0);
        atomic_store(&g_lms.lag_critical_emitted, 0);
    }
}

static void lms_observe_local_primary(int local, int legacy_blocks)
{
    atomic_store(&g_lms.target_height, legacy_blocks);
    atomic_store(&g_lms.last_progress_blocks, 0);
    atomic_store(&g_lms.last_advanced_height, local);
    atomic_store(&g_lms.no_authorized_child_first_seen, 0);
    lms_set_error("local sync primary; mirror observing");
}

static void lms_mark_success(int local, int progress)
{
    if (progress < 0)
        progress = 0;
    atomic_store(&g_lms.last_progress_blocks, progress);
    atomic_store(&g_lms.last_catchup, (int64_t)platform_time_wall_time_t());
    atomic_store(&g_lms.last_advanced_height, local);
    atomic_store(&g_lms.stuck_height, 0);
    atomic_store(&g_lms.stuck_status_flags, 0);
    lms_set_stuck_reason("");
    lms_clear_csr_failure();
    lms_clear_blocker();
}

bool legacy_mirror_sync_request_catchup(const char *reason)
{
    (void)reason;
#ifdef ZCL_TESTING
    if (atomic_load(&g_lms_test_catchup_enabled)) {
        atomic_fetch_add(&g_lms_test_catchup_calls, 1);
        if (atomic_load(&g_lms_test_catchup_clear_stuck)) {
            atomic_store(&g_lms.stuck_height, 0);
            atomic_store(&g_lms.stuck_status_flags, 0);
            pthread_mutex_lock(&g_lms.lock);
            g_lms.stuck_reason[0] = '\0';
            pthread_mutex_unlock(&g_lms.lock);
        }
        return atomic_load(&g_lms_test_catchup_result) != 0;
    }
#endif
    if (!g_lms.initialized || !g_lms.enabled)
        return true;
    if (pthread_mutex_trylock(&g_lms.flight) != 0)
        return true;

    atomic_store(&g_lms.in_flight, 1);
    atomic_store(&g_lms.last_attempt, (int64_t)platform_time_wall_time_t());

    bool ok = true;
    int legacy_blocks = -1, legacy_headers = -1;
    char err[160] = {0};
    if (!lms_fetch_chain_info(&legacy_blocks, &legacy_headers,
                              err, sizeof(err))) {
        atomic_store(&g_lms.reachable, 0);
        atomic_fetch_add(&g_lms.rpc_errors, 1);
        lms_set_blocker("rpc-unreachable", err);
        ok = false;
        goto out;
    }
    atomic_store(&g_lms.reachable, 1);
    atomic_store(&g_lms.legacy_height, legacy_blocks);
    atomic_store(&g_lms.legacy_headers, legacy_headers);

    int local = -1, hdr = -1;
    lms_refresh_local_heights(&local, &hdr);
    int lag = legacy_blocks - local;

    /* SLO evaluation runs every tick regardless of gating — the loud
     * half of the redundancy guarantee. Severity is emitted once per
     * episode (latched), cleared when lag drops back below threshold. */
    lms_evaluate_lag_slo(lag, legacy_blocks, local, (int64_t)platform_time_wall_time_t());

    {
        char local_hash[65] = {0}, remote_hash[65] = {0};
        char hash_err[160] = {0};
        if (lms_local_hash_at(local, local_hash))
            lms_cache_hashes(local_hash, NULL);
        if (legacy_blocks >= 0 &&
            lms_fetch_hash(legacy_blocks, remote_hash,
                           hash_err, sizeof(hash_err))) {
            lms_cache_hashes(NULL, remote_hash);
        }
    }

    if (legacy_blocks < local) {
        atomic_store(&g_lms.target_height, legacy_blocks);
        lms_mark_success(local, 0);
        lms_set_error("mirror behind local; observing");
        ok = true;
        goto out;
    }

    if (!lms_verify_anchor(local)) {
        ok = false;
        goto out;
    }
    if (lag <= g_lms.lag_sla) {
        atomic_store(&g_lms.target_height, legacy_blocks);
        if (local == legacy_blocks && !lms_verify_after_tip(local)) {
            ok = false;
            goto out;
        }
        int prev_advanced = atomic_load(&g_lms.last_advanced_height);
        if (local >= prev_advanced)
            atomic_fetch_add(&g_lms.catchups_total, 1);
        lms_mark_success(local, local - prev_advanced);
        ok = true;
        goto out;
    }
    if (lag > 4) {
        int sample = local + lag / 2;
        if (sample < legacy_blocks && !lms_verify_anchor(sample)) {
            ok = false;
            goto out;
        }
    }

    /* Monitor-only: post-B7 the stage pipeline is the authoritative
     * block writer. The mirror no longer applies blocks; it observes
     * the lag, records the stuck status when behind, and lets the
     * native pipeline advance the tip. */
    atomic_store(&g_lms.target_height, legacy_blocks);
    lms_record_stuck_status(local + 1);
    lms_observe_local_primary(local, legacy_blocks);
    ok = true;
    goto out;

out:
    lms_refresh_local_heights(NULL, NULL);
    atomic_store(&g_lms.in_flight, 0);
    pthread_mutex_unlock(&g_lms.flight);
    return ok;
}

static void lms_on_tick(void *ctx)
{
    (void)ctx;
    (void)legacy_mirror_sync_request_catchup("heartbeat");
}

bool legacy_mirror_sync_init(const struct legacy_mirror_sync_config *cfg,
                             struct main_state *ms,
                             struct coins_view_cache *coins_tip,
                             const struct chain_params *params,
                             const char *datadir)
{
    pthread_mutex_lock(&g_lms.lock);

    snprintf(g_lms.rpc_host, sizeof(g_lms.rpc_host), "%s",
             (cfg && cfg->rpc_host) ? cfg->rpc_host : LMS_DEFAULT_HOST);
    g_lms.rpc_port = (cfg && cfg->rpc_port > 0)
                         ? cfg->rpc_port : LMS_DEFAULT_PORT;
    g_lms.cadence_secs = (cfg && cfg->cadence_secs > 0)
                         ? cfg->cadence_secs : LMS_DEFAULT_CADENCE;
    g_lms.max_blocks_tick = (cfg && cfg->max_blocks_tick > 0)
                         ? cfg->max_blocks_tick : LMS_DEFAULT_MAX_BLOCKS;
    g_lms.lag_sla = (cfg && cfg->lag_sla >= 0)
                         ? cfg->lag_sla : LMS_DEFAULT_LAG_SLA;
    g_lms.cadence_secs = lms_env_int("ZCL_MIRROR_CADENCE_SECS",
                                     g_lms.cadence_secs, 1, 300);
    g_lms.max_blocks_tick = lms_env_int("ZCL_MIRROR_MAX_BLOCKS_PER_TICK",
                                        g_lms.max_blocks_tick, 1, 20000);
    g_lms.lag_sla = lms_env_int("ZCL_MIRROR_LAG_SLA",
                                g_lms.lag_sla, 0, 10000);

    g_lms.lag_sla_breach_blocks =
        (cfg && cfg->lag_sla_breach_blocks > 0)
        ? cfg->lag_sla_breach_blocks
        : LMS_DEFAULT_LAG_SLA_BREACH_BLOCKS;
    g_lms.lag_sla_breach_blocks =
        lms_env_int("ZCL_MIRROR_LAG_SLA_BREACH_BLOCKS",
                    g_lms.lag_sla_breach_blocks, 1, 100000);

    g_lms.lag_sla_breach_secs =
        (cfg && cfg->lag_sla_breach_secs > 0)
        ? cfg->lag_sla_breach_secs
        : LMS_DEFAULT_LAG_SLA_BREACH_SECS;
    g_lms.lag_sla_breach_secs =
        lms_env_int("ZCL_MIRROR_LAG_SLA_BREACH_SECS",
                    g_lms.lag_sla_breach_secs, 1, 86400);

    g_lms.lag_sla_critical_blocks =
        (cfg && cfg->lag_sla_critical_blocks > 0)
        ? cfg->lag_sla_critical_blocks
        : LMS_DEFAULT_LAG_SLA_CRITICAL_BLOCKS;
    g_lms.lag_sla_critical_blocks =
        lms_env_int("ZCL_MIRROR_LAG_SLA_CRITICAL_BLOCKS",
                    g_lms.lag_sla_critical_blocks, 1, 1000000);

    g_lms.lag_sla_critical_secs =
        (cfg && cfg->lag_sla_critical_secs > 0)
        ? cfg->lag_sla_critical_secs
        : LMS_DEFAULT_LAG_SLA_CRITICAL_SECS;
    g_lms.lag_sla_critical_secs =
        lms_env_int("ZCL_MIRROR_LAG_SLA_CRITICAL_SECS",
                    g_lms.lag_sla_critical_secs, 1, 86400);

    g_lms.ms = ms;
    g_lms.coins_tip = coins_tip;
    g_lms.params = params;
    snprintf(g_lms.datadir, sizeof(g_lms.datadir), "%s",
             datadir ? datadir : "");

    if (cfg && cfg->rpc_user && cfg->rpc_user[0])
        snprintf(g_lms.rpc_user, sizeof(g_lms.rpc_user), "%s",
                 cfg->rpc_user);
    if (cfg && cfg->rpc_password && cfg->rpc_password[0])
        snprintf(g_lms.rpc_password, sizeof(g_lms.rpc_password), "%s",
                 cfg->rpc_password);

    bool need_user = (g_lms.rpc_user[0] == '\0');
    bool need_pass = (g_lms.rpc_password[0] == '\0');
    if (need_user || need_pass) {
        int conf_port = g_lms.rpc_port;
        char u[64] = {0}, p[128] = {0};
        if (legacy_rpc_parse_conf(u, sizeof(u), p, sizeof(p),
                                  &conf_port)) {
            if (need_user)
                snprintf(g_lms.rpc_user, sizeof(g_lms.rpc_user), "%s", u);
            if (need_pass)
                snprintf(g_lms.rpc_password, sizeof(g_lms.rpc_password),
                         "%s", p);
            if (!cfg || cfg->rpc_port <= 0)
                g_lms.rpc_port = conf_port;
        }
    }

    bool have_creds = g_lms.rpc_user[0] && g_lms.rpc_password[0];
    g_lms.enabled = (cfg ? cfg->enabled : true) && have_creds &&
                    !lms_env_disabled();
    mirror_consensus_set_enabled(g_lms.enabled);
    g_lms.initialized = true;
    pthread_mutex_unlock(&g_lms.lock);

    if (!have_creds) {
        lms_set_error("no zclassicd RPC credentials");
        return false;
    }
    return true;
}

void legacy_mirror_sync_reload_from_env(void)
{
    if (!g_lms.initialized)
        return;
    pthread_mutex_lock(&g_lms.lock);
    g_lms.cadence_secs = lms_env_int("ZCL_MIRROR_CADENCE_SECS",
                                     g_lms.cadence_secs, 1, 300);
    g_lms.max_blocks_tick = lms_env_int("ZCL_MIRROR_MAX_BLOCKS_PER_TICK",
                                        g_lms.max_blocks_tick, 1, 20000);
    g_lms.lag_sla = lms_env_int("ZCL_MIRROR_LAG_SLA",
                                g_lms.lag_sla, 0, 10000);
    g_lms.lag_sla_breach_blocks =
        lms_env_int("ZCL_MIRROR_LAG_SLA_BREACH_BLOCKS",
                    g_lms.lag_sla_breach_blocks, 1, 100000);
    g_lms.lag_sla_breach_secs =
        lms_env_int("ZCL_MIRROR_LAG_SLA_BREACH_SECS",
                    g_lms.lag_sla_breach_secs, 1, 86400);
    g_lms.lag_sla_critical_blocks =
        lms_env_int("ZCL_MIRROR_LAG_SLA_CRITICAL_BLOCKS",
                    g_lms.lag_sla_critical_blocks, 1, 1000000);
    g_lms.lag_sla_critical_secs =
        lms_env_int("ZCL_MIRROR_LAG_SLA_CRITICAL_SECS",
                    g_lms.lag_sla_critical_secs, 1, 86400);
    pthread_mutex_unlock(&g_lms.lock);
}

bool legacy_mirror_sync_start(void)
{
    if (!g_lms.initialized) {
        fprintf(stderr, "[legacy_mirror] start before init\n");
        return false;
    }
    if (!g_lms.enabled)
        return true;
    if (g_lms.health_id != HEALTH_INVALID_ID)
        return true;
    (void)health_start();
    int cad = g_lms.cadence_secs > 0 ? g_lms.cadence_secs
                                     : LMS_DEFAULT_CADENCE;
    g_lms.health_id = health_register_periodic("legacy_mirror", cad,
                                               lms_on_tick, NULL);
    if (g_lms.health_id == HEALTH_INVALID_ID) {
        fprintf(stderr, "[legacy_mirror] health_register_periodic failed\n");
        return false;
    }
    return true;
}

void legacy_mirror_sync_stop(void)
{
    if (g_lms.health_id == HEALTH_INVALID_ID)
        return;
    health_unregister(g_lms.health_id);
    g_lms.health_id = HEALTH_INVALID_ID;
}

void legacy_mirror_sync_stats_snapshot(
    struct legacy_mirror_sync_stats *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    lms_refresh_local_heights(NULL, NULL);
    pthread_mutex_lock(&g_lms.lock);
    out->enabled = g_lms.enabled;
    out->running = g_lms.health_id != HEALTH_INVALID_ID;
    snprintf(out->zclassicd_hash, sizeof(out->zclassicd_hash), "%s",
             g_lms.zclassicd_hash);
    snprintf(out->last_error, sizeof(out->last_error), "%s",
             g_lms.last_error);
    snprintf(out->last_blocker_code, sizeof(out->last_blocker_code), "%s",
             g_lms.last_blocker_code);
    snprintf(out->csr_failure_reason, sizeof(out->csr_failure_reason), "%s",
             g_lms.csr_failure_reason);
    snprintf(out->stuck_reason, sizeof(out->stuck_reason), "%s",
             g_lms.stuck_reason);
    pthread_mutex_unlock(&g_lms.lock);
    out->reachable = atomic_load(&g_lms.reachable) != 0;
    out->in_flight = atomic_load(&g_lms.in_flight) != 0;
    out->legacy_height = atomic_load(&g_lms.legacy_height);
    out->legacy_headers = atomic_load(&g_lms.legacy_headers);
    out->local_height = atomic_load(&g_lms.local_height);
    out->best_header_height = atomic_load(&g_lms.best_header_height);
    if (!lms_local_hash_at(out->local_height, out->zclassic23_hash)) {
        pthread_mutex_lock(&g_lms.lock);
        snprintf(out->zclassic23_hash, sizeof(out->zclassic23_hash), "%s",
                 g_lms.zclassic23_hash);
        pthread_mutex_unlock(&g_lms.lock);
    }
    out->lag = out->legacy_height - out->local_height;
    out->target_height = atomic_load(&g_lms.target_height);
    out->authority_rewind_target =
        atomic_load(&g_lms.authority_rewind_target);
    out->csr_sqlite_rc = atomic_load(&g_lms.csr_sqlite_rc);
    out->last_advanced_height = atomic_load(&g_lms.last_advanced_height);
    out->last_progress_blocks = atomic_load(&g_lms.last_progress_blocks);
    {
        struct watchdog_local_recovery_stats lr;
        sync_monitor_get_local_recovery_stats(&lr);
        out->local_recovery_active = lr.active;
        out->mirror_repair_gated_by_local_retries =
            lr.mirror_repair_gated;
        out->local_retries_exhausted = lr.retries_exhausted;
        out->local_missing_height = lr.missing_height;
        out->local_retry_count = lr.retry_count;
        out->local_distinct_peer_count = lr.distinct_peer_count;
        out->local_peer_rotation_count = lr.peer_rotation_count;
    }
    out->stuck_height = atomic_load(&g_lms.stuck_height);
    out->stuck_status_flags = atomic_load(&g_lms.stuck_status_flags);
    out->stalls_total = atomic_load(&g_lms.stalls_total);
    out->last_catchup = atomic_load(&g_lms.last_catchup);
    out->last_attempt = atomic_load(&g_lms.last_attempt);
    out->catchups_total = atomic_load(&g_lms.catchups_total);
    out->rpc_errors = atomic_load(&g_lms.rpc_errors);
    out->blocks_applied = atomic_load(&g_lms.blocks_applied);
    out->headers_added = atomic_load(&g_lms.headers_added);
    {
        struct mirror_consensus_stats mcs;
        mirror_consensus_stats_snapshot(&mcs);
        snprintf(out->consensus_authority,
                 sizeof(out->consensus_authority), "%s",
                 "local_consensus_validation");
        snprintf(out->candidate_trust,
                 sizeof(out->candidate_trust), "%s",
                 "bounded_advisory_fallback");
        out->override_active = mcs.override_active;
        out->overrides_total = mcs.overrides_total;
        out->unsafe_overrides_total = mcs.unsafe_overrides_total;
        out->blockers_total = mcs.blockers_total;
        out->last_override_height = mcs.last_override_height;
        out->last_override_safe = mcs.last_override_safe;
        snprintf(out->last_override_reason,
                 sizeof(out->last_override_reason), "%s",
                 mcs.last_override_reason);
        snprintf(out->last_override_scope,
                 sizeof(out->last_override_scope), "%s",
                 mcs.last_override_scope);
        snprintf(out->activation_blocker,
                 sizeof(out->activation_blocker), "%s",
                 mcs.activation_blocker);
    }
    if (lms_blocker_cleared_by_catchup(out->activation_blocker, out->lag))
        out->activation_blocker[0] = '\0';
    if (lms_blocker_cleared_by_catchup(out->last_blocker_code, out->lag))
        out->last_blocker_code[0] = '\0';
    out->lag_sla_breach_blocks   = g_lms.lag_sla_breach_blocks;
    out->lag_sla_breach_secs     = g_lms.lag_sla_breach_secs;
    out->lag_sla_critical_blocks = g_lms.lag_sla_critical_blocks;
    out->lag_sla_critical_secs   = g_lms.lag_sla_critical_secs;
    out->lag_breach_since        = atomic_load(&g_lms.lag_breach_since);
    out->lag_critical_since      = atomic_load(&g_lms.lag_critical_since);
    {
        int64_t now = (int64_t)platform_time_wall_time_t();
        out->lag_breach_seconds =
            out->lag_breach_since > 0 && now >= out->lag_breach_since
                ? now - out->lag_breach_since : 0;
        out->lag_critical_seconds =
            out->lag_critical_since > 0 && now >= out->lag_critical_since
                ? now - out->lag_critical_since : 0;
    }
    /* severity ladder: clear < breach < critical < fatal */
    const char *sev = "none";
    if (out->lag_critical_since > 0 &&
        out->lag_critical_seconds >= out->lag_sla_critical_secs)
        sev = "fatal";
    else if (out->lag_critical_since > 0)
        sev = "critical";
    else if (out->lag_breach_since > 0 &&
             out->lag_breach_seconds >= out->lag_sla_breach_secs)
        sev = "critical";
    else if (out->lag_breach_since > 0)
        sev = "warn";
    snprintf(out->lag_breach_severity, sizeof(out->lag_breach_severity),
             "%s", sev);
    snprintf(out->state, sizeof(out->state), "%s", lms_state_name(out));
}

bool legacy_mirror_sync_dump_state_json(struct json_value *out,
                                        const char *key)
{
    (void)key;
    if (!out) return false;
    struct legacy_mirror_sync_stats s;
    legacy_mirror_sync_stats_snapshot(&s);
    json_push_kv_bool(out, "mirror_enabled", s.enabled);
    json_push_kv_str(out, "state", s.state);
    json_push_kv_bool(out, "mirror_running", s.running);
    json_push_kv_bool(out, "running", s.running);
    json_push_kv_bool(out, "reachable", s.reachable);
    json_push_kv_bool(out, "mirror_reachable", s.reachable);
    json_push_kv_bool(out, "in_flight", s.in_flight);
    json_push_kv_int(out, "zclassic23_height", s.local_height);
    json_push_kv_str(out, "zclassic23_hash", s.zclassic23_hash);
    json_push_kv_int(out, "zclassicd_height", s.legacy_height);
    json_push_kv_str(out, "zclassicd_hash", s.zclassicd_hash);
    json_push_kv_int(out, "legacy_height", s.legacy_height);
    json_push_kv_int(out, "legacy_headers", s.legacy_headers);
    json_push_kv_int(out, "local_height", s.local_height);
    json_push_kv_int(out, "best_header_height", s.best_header_height);
    json_push_kv_int(out, "lag", s.lag);
    json_push_kv_str(out, "candidate_source", "legacy_advisory");
    json_push_kv_str(out, "candidate_trust", s.candidate_trust);
    json_push_kv_int(out, "candidate_lag", s.lag);
    json_push_kv_str(out, "candidate_blocker",
                     s.activation_blocker[0] ? s.activation_blocker
                                             : s.last_blocker_code);
    json_push_kv_int(out, "target_height", s.target_height);
    json_push_kv_int(out, "authority_rewind_target",
                     s.authority_rewind_target);
    json_push_kv_int(out, "last_advanced_height", s.last_advanced_height);
    json_push_kv_int(out, "last_progress_blocks", s.last_progress_blocks);
    json_push_kv_bool(out, "local_recovery_active",
                      s.local_recovery_active);
    json_push_kv_bool(out, "legacy_advisory_gated_by_native_retries",
                      s.mirror_repair_gated_by_local_retries);
    json_push_kv_bool(out, "mirror_repair_gated_by_local_retries",
                      s.mirror_repair_gated_by_local_retries);
    json_push_kv_bool(out, "local_retries_exhausted",
                      s.local_retries_exhausted);
    json_push_kv_int(out, "local_missing_height", s.local_missing_height);
    json_push_kv_int(out, "local_retry_count", s.local_retry_count);
    json_push_kv_int(out, "local_distinct_peer_count",
                     s.local_distinct_peer_count);
    json_push_kv_int(out, "local_peer_rotation_count",
                     s.local_peer_rotation_count);
    json_push_kv_int(out, "stuck_height", s.stuck_height);
    json_push_kv_int(out, "stuck_status_flags", s.stuck_status_flags);
    json_push_kv_str(out, "stuck_reason", s.stuck_reason);
    json_push_kv_int(out, "stalls_total", s.stalls_total);
    json_push_kv_int(out, "last_catchup", s.last_catchup);
    json_push_kv_int(out, "last_attempt", s.last_attempt);
    json_push_kv_int(out, "catchups_total", s.catchups_total);
    json_push_kv_int(out, "rpc_errors", s.rpc_errors);
    json_push_kv_int(out, "blocks_applied", s.blocks_applied);
    json_push_kv_int(out, "headers_added", s.headers_added);
    json_push_kv_str(out, "consensus_authority", s.consensus_authority);
    json_push_kv_bool(out, "override_active", s.override_active);
    json_push_kv_int(out, "overrides_total", s.overrides_total);
    json_push_kv_int(out, "unsafe_overrides_total",
                     s.unsafe_overrides_total);
    json_push_kv_int(out, "blockers_total", s.blockers_total);
    json_push_kv_int(out, "last_override_height", s.last_override_height);
    json_push_kv_bool(out, "last_override_safe", s.last_override_safe);
    json_push_kv_str(out, "last_override_reason", s.last_override_reason);
    json_push_kv_str(out, "last_override_scope", s.last_override_scope);
    json_push_kv_str(out, "activation_blocker", s.activation_blocker);
    json_push_kv_str(out, "last_blocker_code", s.last_blocker_code);
    json_push_kv_int(out, "csr_sqlite_rc", s.csr_sqlite_rc);
    json_push_kv_str(out, "csr_failure_reason", s.csr_failure_reason);
    json_push_kv_str(out, "last_error", s.last_error);
    json_push_kv_int(out, "lag_sla_breach_blocks", s.lag_sla_breach_blocks);
    json_push_kv_int(out, "lag_sla_breach_secs", s.lag_sla_breach_secs);
    json_push_kv_int(out, "lag_sla_critical_blocks",
                     s.lag_sla_critical_blocks);
    json_push_kv_int(out, "lag_sla_critical_secs", s.lag_sla_critical_secs);
    json_push_kv_int(out, "lag_breach_since", s.lag_breach_since);
    json_push_kv_int(out, "lag_breach_seconds", s.lag_breach_seconds);
    json_push_kv_int(out, "lag_critical_since", s.lag_critical_since);
    json_push_kv_int(out, "lag_critical_seconds", s.lag_critical_seconds);
    json_push_kv_str(out, "lag_breach_severity", s.lag_breach_severity);
    return true;
}

void legacy_mirror_sync_reset_for_test(void)
{
#ifdef ZCL_TESTING
    if (g_lms_test_fake_running) {
        g_lms.health_id = HEALTH_INVALID_ID;
        g_lms_test_fake_running = false;
    }
#endif
    legacy_mirror_sync_stop();
    pthread_mutex_lock(&g_lms.lock);
    g_lms.initialized = false;
    g_lms.enabled = false;
    mirror_consensus_set_enabled(false);
    g_lms.rpc_host[0] = '\0';
    g_lms.rpc_port = 0;
    g_lms.rpc_user[0] = '\0';
    g_lms.rpc_password[0] = '\0';
    g_lms.datadir[0] = '\0';
    g_lms.zclassic23_hash[0] = '\0';
    g_lms.zclassicd_hash[0] = '\0';
    g_lms.stuck_reason[0] = '\0';
    g_lms.last_blocker_code[0] = '\0';
    g_lms.csr_failure_reason[0] = '\0';
    g_lms.ms = NULL;
    g_lms.coins_tip = NULL;
    g_lms.params = NULL;
    g_lms.last_error[0] = '\0';
    pthread_mutex_unlock(&g_lms.lock);
    atomic_store(&g_lms.reachable, 0);
    atomic_store(&g_lms.in_flight, 0);
    atomic_store(&g_lms.legacy_height, 0);
    atomic_store(&g_lms.legacy_headers, 0);
    atomic_store(&g_lms.local_height, 0);
    atomic_store(&g_lms.best_header_height, 0);
    atomic_store(&g_lms.target_height, 0);
    atomic_store(&g_lms.authority_rewind_target, 0);
    atomic_store(&g_lms.csr_sqlite_rc, 0);
    atomic_store(&g_lms.last_advanced_height, 0);
    atomic_store(&g_lms.last_progress_blocks, 0);
    atomic_store(&g_lms.stuck_height, 0);
    atomic_store(&g_lms.stuck_status_flags, 0);
    atomic_store(&g_lms.stalls_total, 0);
    atomic_store(&g_lms.last_catchup, 0);
    atomic_store(&g_lms.last_attempt, 0);
    atomic_store(&g_lms.catchups_total, 0);
    atomic_store(&g_lms.rpc_errors, 0);
    atomic_store(&g_lms.blocks_applied, 0);
    atomic_store(&g_lms.headers_added, 0);
    atomic_store(&g_lms.lag_breach_since, 0);
    atomic_store(&g_lms.lag_critical_since, 0);
    atomic_store(&g_lms.lag_breach_emitted, 0);
    atomic_store(&g_lms.lag_critical_emitted, 0);
#ifdef ZCL_TESTING
    atomic_store(&g_lms_test_catchup_enabled, 0);
    atomic_store(&g_lms_test_catchup_result, 0);
    atomic_store(&g_lms_test_catchup_clear_stuck, 0);
    atomic_store(&g_lms_test_catchup_calls, 0);
#endif
    mirror_consensus_reset_for_test();
}

#ifdef ZCL_TESTING
void legacy_mirror_sync_test_set_stats(
    const struct legacy_mirror_sync_stats *stats,
    struct main_state *ms)
{
    if (!stats)
        return;

    pthread_mutex_lock(&g_lms.lock);
    g_lms.initialized = true;
    g_lms.enabled = stats->enabled;
    g_lms.health_id = stats->running ? (health_subsystem_id)1
                                     : HEALTH_INVALID_ID;
    g_lms_test_fake_running = stats->running;
    g_lms.ms = ms;
    snprintf(g_lms.zclassic23_hash, sizeof(g_lms.zclassic23_hash), "%s",
             stats->zclassic23_hash);
    snprintf(g_lms.zclassicd_hash, sizeof(g_lms.zclassicd_hash), "%s",
             stats->zclassicd_hash);
    snprintf(g_lms.stuck_reason, sizeof(g_lms.stuck_reason), "%s",
             stats->stuck_reason);
    snprintf(g_lms.last_blocker_code, sizeof(g_lms.last_blocker_code),
             "%s", stats->last_blocker_code);
    snprintf(g_lms.csr_failure_reason, sizeof(g_lms.csr_failure_reason),
             "%s", stats->csr_failure_reason);
    snprintf(g_lms.last_error, sizeof(g_lms.last_error), "%s",
             stats->last_error);
    pthread_mutex_unlock(&g_lms.lock);

    atomic_store(&g_lms.reachable, stats->reachable ? 1 : 0);
    atomic_store(&g_lms.in_flight, stats->in_flight ? 1 : 0);
    atomic_store(&g_lms.legacy_height, stats->legacy_height);
    atomic_store(&g_lms.legacy_headers, stats->legacy_headers);
    atomic_store(&g_lms.local_height, stats->local_height);
    atomic_store(&g_lms.best_header_height, stats->best_header_height);
    atomic_store(&g_lms.target_height, stats->target_height);
    atomic_store(&g_lms.authority_rewind_target,
                 stats->authority_rewind_target);
    atomic_store(&g_lms.csr_sqlite_rc, stats->csr_sqlite_rc);
    atomic_store(&g_lms.last_advanced_height,
                 stats->last_advanced_height);
    atomic_store(&g_lms.last_progress_blocks,
                 stats->last_progress_blocks);
    atomic_store(&g_lms.stuck_height, stats->stuck_height);
    atomic_store(&g_lms.stuck_status_flags, stats->stuck_status_flags);
    atomic_store(&g_lms.stalls_total, stats->stalls_total);
    atomic_store(&g_lms.last_catchup, stats->last_catchup);
    atomic_store(&g_lms.last_attempt, stats->last_attempt);
    atomic_store(&g_lms.catchups_total, stats->catchups_total);
    atomic_store(&g_lms.rpc_errors, stats->rpc_errors);
    atomic_store(&g_lms.blocks_applied, stats->blocks_applied);
    atomic_store(&g_lms.headers_added, stats->headers_added);
}

void legacy_mirror_sync_test_set_catchup_result(bool enabled,
                                                bool result,
                                                bool clear_stuck)
{
    atomic_store(&g_lms_test_catchup_enabled, enabled ? 1 : 0);
    atomic_store(&g_lms_test_catchup_result, result ? 1 : 0);
    atomic_store(&g_lms_test_catchup_clear_stuck, clear_stuck ? 1 : 0);
    atomic_store(&g_lms_test_catchup_calls, 0);
}

int legacy_mirror_sync_test_catchup_calls(void)
{
    return atomic_load(&g_lms_test_catchup_calls);
}
#endif
