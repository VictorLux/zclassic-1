/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

/* Stateful block-source decision runtime — the live state, persistence,
 * runtime-input builder, decision recorder, projection-deferral counter,
 * status read, and the zcl_state dumper for the block-source policy.
 * Re-homed verbatim from the dissolved chain_advance_coordinator shell
 * (B8). The pure scoring/name/plan policy lives in block_source_policy.c;
 * this file is the cohesive stateful seam split out to respect the E1
 * file-size ceiling. */

#include "services/block_source_policy.h"

#include "platform/time_compat.h"
#include "util/log_macros.h"
#include "services/legacy_mirror_sync_service.h"
#include "services/snapshot_sync_service.h"
#include "services/sync_monitor.h"
#include "models/block.h"
#include "models/database.h"
#include "net/connman.h"
#include "net/peer_lifecycle.h"
#include "event/event.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include "util/sync.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static void copy_text(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) return;
    if (!src) src = "";
    snprintf(dst, dst_len, "%s", src);
}
/* ---------------------------------------------------------------------------
 * Stateful block-source decision surface (re-homed from the dissolved
 * chain_advance_coordinator shell, B8). Behavior-preserving: the runtime-input
 * builder, decision recorder, persistence, status read, projection-deferral
 * counter, and the zcl_state dumper are moved verbatim from the old shell.
 * --------------------------------------------------------------------------- */

static struct {
    zcl_mutex_t lock;
    bool lock_init;
    struct connman *connman;
    struct main_state *main_state;
    struct node_db *node_db;
    struct cac_decision last;
    bool has_last;
    int64_t last_decision_time;
    char last_op[32];
    int64_t decisions_total;
    int64_t projection_deferred_total;
    int last_projection_deferred_height;
    int64_t last_projection_deferred_time;
    char last_projection_deferred_reason[64];
} g_bsp;

#define BSP_STATE_PREFIX "chain_advance_coordinator."
#define BSP_KEY_LAST_OP BSP_STATE_PREFIX "last_op"
#define BSP_KEY_LAST_TIME BSP_STATE_PREFIX "last_time"
#define BSP_KEY_DECISIONS_TOTAL BSP_STATE_PREFIX "decisions_total"
#define BSP_KEY_RESULT BSP_STATE_PREFIX "last_result"
#define BSP_KEY_SOURCE BSP_STATE_PREFIX "last_source"
#define BSP_KEY_ACTIVATION_ALLOWED BSP_STATE_PREFIX "last_activation_allowed"
#define BSP_KEY_MIRROR_FALLBACK_ALLOWED BSP_STATE_PREFIX "last_mirror_fallback_allowed"
#define BSP_KEY_SELECTED_SCORE BSP_STATE_PREFIX "last_selected_score"
#define BSP_KEY_REASON BSP_STATE_PREFIX "last_reason"
#define BSP_KEY_BLOCKER BSP_STATE_PREFIX "last_blocker"
#define BSP_KEY_LOCAL_HEIGHT BSP_STATE_PREFIX "last_local_height"
#define BSP_KEY_BEST_HEADER_HEIGHT BSP_STATE_PREFIX "last_best_header_height"
#define BSP_KEY_TARGET_HEIGHT BSP_STATE_PREFIX "last_target_height"
#define BSP_KEY_PROJECTION_DEFERRED_TOTAL \
    BSP_STATE_PREFIX "projection_deferred_total"
#define BSP_KEY_LAST_PROJECTION_DEFERRED_HEIGHT \
    BSP_STATE_PREFIX "last_projection_deferred_height"
#define BSP_KEY_LAST_PROJECTION_DEFERRED_TIME \
    BSP_STATE_PREFIX "last_projection_deferred_time"
#define BSP_KEY_LAST_PROJECTION_DEFERRED_REASON \
    BSP_STATE_PREFIX "last_projection_deferred_reason"
#define BSP_KEY_SOURCE_STATE BSP_STATE_PREFIX "last_source_state"
#define BSP_KEY_SOURCE_REASON BSP_STATE_PREFIX "last_source_reason"
#define BSP_KEY_SOURCE_BLOCKER BSP_STATE_PREFIX "last_source_blocker"
#define BSP_KEY_SOURCE_HEIGHT BSP_STATE_PREFIX "last_source_height"
#define BSP_KEY_SOURCE_HEALTHY BSP_STATE_PREFIX "last_source_healthy"
#define BSP_KEY_SOURCE_AVAILABLE BSP_STATE_PREFIX "last_source_available"
#define BSP_KEY_SOURCE_BLOCKED BSP_STATE_PREFIX "last_source_blocked"
#define BSP_KEY_SOURCE_PREFIX BSP_STATE_PREFIX "last_source."

static void bsp_lock_init_once(void)
{
    if (!g_bsp.lock_init) {
        zcl_mutex_init(&g_bsp.lock);
        g_bsp.lock_init = true;
    }
}

/* Classify legacy_mirror_sync_service blocker strings into the typed enum
 * reported by source scoring, status, and decision events.
 *
 * The string codes come from the legacy mirror blocker setter in
 * legacy_mirror_sync_service.c. They're stable identifiers, not free
 * text — this table is the canonical mapping. Any unknown code is
 * conservatively TRANSIENT (recoverable by default) so an un-mapped new
 * code doesn't accidentally look like a hard consensus gate. Reverse
 * risk — accidentally classifying a real consensus divergence as
 * TRANSIENT — is bounded: hash-disagreement and body-hash-mismatch are
 * the only PERMANENT entries and they are the existing codes that signal
 * consensus divergence today; any future consensus-divergence code MUST
 * be added here.
 *
 * Update sites: when the legacy mirror blocker setter gains a new code,
 * add it here. */
static enum blocker_class classify_mirror_blocker_class(const char *code)
{
    if (!code || code[0] == '\0')
        return BLOCKER_TRANSIENT;

    /* Consensus divergence — the mirror disagrees with us on a hash. */
    if (strcmp(code, "hash-disagreement") == 0 ||
        strcmp(code, "body-hash-mismatch") == 0)
        return BLOCKER_PERMANENT;

    /* Resource contention — DB writer busy or similar. RESOURCE class
     * signals "operator action may be needed". */
    if (strcmp(code, "db-writer-busy") == 0)
        return BLOCKER_RESOURCE;

    /* Waiting on a downstream subsystem (activation controller). */
    if (strcmp(code, "activation-failed") == 0)
        return BLOCKER_DEPENDENCY;

    /* Default: network/timeout/no-progress class — transient by
     * construction. */
    return BLOCKER_TRANSIENT;
}

static bool persist_text(struct node_db *ndb, const char *key, const char *val)
{
    if (!ndb || !key || !val)
        return false;
    return node_db_state_set(ndb, key, val, strlen(val) + 1);
}

static bool source_field_key(char *buf, size_t buflen,
                             enum cac_source source,
                             const char *field)
{
    if (!buf || buflen == 0 || !field)
        return false;
    int n = snprintf(buf, buflen, "%s%d.%s", BSP_KEY_SOURCE_PREFIX,
                     (int)source, field);
    return n > 0 && (size_t)n < buflen;
}

static void persist_source_int(struct node_db *ndb,
                               enum cac_source source,
                               const char *field,
                               int64_t value)
{
    char key[96];
    if (source_field_key(key, sizeof(key), source, field))
        (void)node_db_state_set_int(ndb, key, value);
}

static void restore_source_int(struct node_db *ndb,
                               enum cac_source source,
                               const char *field,
                               int64_t *out)
{
    char key[96];
    int64_t v = 0;
    if (!out)
        return;
    *out = 0;
    if (source_field_key(key, sizeof(key), source, field) &&
        node_db_state_get_int(ndb, key, &v))
        *out = v;
}

static void persist_source_snapshot(struct node_db *ndb,
                                    enum cac_source source,
                                    const struct cac_source_status *s)
{
    if (!ndb || !s || source <= CAC_SOURCE_NONE || source >= CAC_SOURCE_NUM)
        return;
    char key[96];
    if (source_field_key(key, sizeof(key), source, "state"))
        (void)persist_text(ndb, key, s->state);
    if (source_field_key(key, sizeof(key), source, "selection_blocker"))
        (void)persist_text(ndb, key, s->selection_blocker);
    if (source_field_key(key, sizeof(key), source, "reason"))
        (void)persist_text(ndb, key, s->reason);
    if (source_field_key(key, sizeof(key), source, "blocker"))
        (void)persist_text(ndb, key, s->blocker);
    persist_source_int(ndb, source, "height", (int64_t)s->height);
    persist_source_int(ndb, source, "score", (int64_t)s->score);
    persist_source_int(ndb, source, "score_base", s->score_base);
    persist_source_int(ndb, source, "score_health", s->score_health);
    persist_source_int(ndb, source, "score_height", s->score_height);
    persist_source_int(ndb, source, "score_authorized",
                       s->score_authorized);
    persist_source_int(ndb, source, "score_target_lag_penalty",
                       s->score_target_lag_penalty);
    persist_source_int(ndb, source, "score_failure_penalty",
                       s->score_failure_penalty);
    persist_source_int(ndb, source, "score_mirror_gate_penalty",
                       s->score_mirror_gate_penalty);
    persist_source_int(ndb, source, "healthy", s->healthy ? 1 : 0);
    persist_source_int(ndb, source, "available", s->available ? 1 : 0);
    persist_source_int(ndb, source, "blocked", s->blocked ? 1 : 0);
    persist_source_int(ndb, source, "blocked_class",
                       (int64_t)s->blocked_class);
    persist_source_int(ndb, source, "authorized", s->authorized ? 1 : 0);
    persist_source_int(ndb, source, "selectable", s->selectable ? 1 : 0);
    persist_source_int(ndb, source, "failures", s->failures);
    persist_source_int(ndb, source, "timeouts", s->timeouts);
    persist_source_int(ndb, source, "outbound_total", s->outbound_total);
    persist_source_int(ndb, source, "inbound_total", s->inbound_total);
    persist_source_int(ndb, source, "healthy_peers", s->healthy_peers);
    persist_source_int(ndb, source, "inbound_healthy_peers",
                       s->inbound_healthy_peers);
    persist_source_int(ndb, source, "total_healthy_peers",
                       s->total_healthy_peers);
    persist_source_int(ndb, source, "connecting_peers", s->connecting_peers);
    persist_source_int(ndb, source, "handshake_incomplete",
                       s->handshake_incomplete);
    persist_source_int(ndb, source, "inbound_handshake_incomplete",
                       s->inbound_handshake_incomplete);
    persist_source_int(ndb, source, "peer_groups", s->peer_groups);
    persist_source_int(ndb, source, "max_peer_group_size",
                       s->max_peer_group_size);
    persist_source_int(ndb, source, "healthy_peer_groups",
                       s->healthy_peer_groups);
    persist_source_int(ndb, source, "healthy_max_peer_group_size",
                       s->healthy_max_peer_group_size);
    persist_source_int(ndb, source, "addnode_count", s->addnode_count);
    persist_source_int(ndb, source, "addnode_backoff_active",
                       s->addnode_backoff_active);
    persist_source_int(ndb, source, "addnode_backoff_max_sec",
                       s->addnode_backoff_max_sec);
    persist_source_int(ndb, source, "addnode_tcp_failures",
                       s->addnode_tcp_failures);
    persist_source_int(ndb, source, "addnode_protocol_failures",
                       s->addnode_protocol_failures);
    persist_source_int(ndb, source, "progress_current",
                       s->progress_current);
    persist_source_int(ndb, source, "progress_total", s->progress_total);
    persist_source_int(ndb, source, "lag", s->lag);
    persist_source_int(ndb, source, "retry_count", s->retry_count);
    persist_source_int(ndb, source, "distinct_peer_count",
                       s->distinct_peer_count);
    persist_source_int(ndb, source, "serving_peer_id", s->serving_peer_id);
}

static void restore_source_snapshot(struct node_db *ndb,
                                    enum cac_source source,
                                    struct cac_source_status *s)
{
    if (!ndb || !s || source <= CAC_SOURCE_NONE || source >= CAC_SOURCE_NUM)
        return;
    char key[96];
    char text[384] = {0};
    size_t len = 0;
    int64_t v = 0;

    s->source = source;
    if (source_field_key(key, sizeof(key), source, "state") &&
        node_db_state_get(ndb, key, text, sizeof(text) - 1, &len))
        copy_text(s->state, sizeof(s->state), text);
    text[0] = '\0';
    if (source_field_key(key, sizeof(key), source, "selection_blocker") &&
        node_db_state_get(ndb, key, text, sizeof(text) - 1, &len))
        copy_text(s->selection_blocker, sizeof(s->selection_blocker), text);
    text[0] = '\0';
    if (source_field_key(key, sizeof(key), source, "reason") &&
        node_db_state_get(ndb, key, text, sizeof(text) - 1, &len))
        copy_text(s->reason, sizeof(s->reason), text);
    text[0] = '\0';
    if (source_field_key(key, sizeof(key), source, "blocker") &&
        node_db_state_get(ndb, key, text, sizeof(text) - 1, &len))
        copy_text(s->blocker, sizeof(s->blocker), text);
    restore_source_int(ndb, source, "height", &v); s->height = (int)v;
    restore_source_int(ndb, source, "score", &v); s->score = (int)v;
    restore_source_int(ndb, source, "score_base", &v);
    s->score_base = (int)v;
    restore_source_int(ndb, source, "score_health", &v);
    s->score_health = (int)v;
    restore_source_int(ndb, source, "score_height", &v);
    s->score_height = (int)v;
    restore_source_int(ndb, source, "score_authorized", &v);
    s->score_authorized = (int)v;
    restore_source_int(ndb, source, "score_target_lag_penalty", &v);
    s->score_target_lag_penalty = (int)v;
    restore_source_int(ndb, source, "score_failure_penalty", &v);
    s->score_failure_penalty = (int)v;
    restore_source_int(ndb, source, "score_mirror_gate_penalty", &v);
    s->score_mirror_gate_penalty = (int)v;
    restore_source_int(ndb, source, "healthy", &v); s->healthy = v != 0;
    restore_source_int(ndb, source, "available", &v); s->available = v != 0;
    restore_source_int(ndb, source, "blocked", &v); s->blocked = v != 0;
    restore_source_int(ndb, source, "blocked_class", &v);
    s->blocked_class = (v >= 0 && v <= BLOCKER_RESOURCE)
                            ? (enum blocker_class)v
                            : BLOCKER_TRANSIENT;
    restore_source_int(ndb, source, "authorized", &v);
    s->authorized = v != 0;
    restore_source_int(ndb, source, "selectable", &v);
    s->selectable = v != 0;
    restore_source_int(ndb, source, "failures", &s->failures);
    restore_source_int(ndb, source, "timeouts", &s->timeouts);
    restore_source_int(ndb, source, "outbound_total", &s->outbound_total);
    restore_source_int(ndb, source, "inbound_total", &s->inbound_total);
    restore_source_int(ndb, source, "healthy_peers", &s->healthy_peers);
    restore_source_int(ndb, source, "inbound_healthy_peers",
                       &s->inbound_healthy_peers);
    restore_source_int(ndb, source, "total_healthy_peers",
                       &s->total_healthy_peers);
    restore_source_int(ndb, source, "connecting_peers", &s->connecting_peers);
    restore_source_int(ndb, source, "handshake_incomplete",
                       &s->handshake_incomplete);
    restore_source_int(ndb, source, "inbound_handshake_incomplete",
                       &s->inbound_handshake_incomplete);
    restore_source_int(ndb, source, "peer_groups", &s->peer_groups);
    restore_source_int(ndb, source, "max_peer_group_size",
                       &s->max_peer_group_size);
    restore_source_int(ndb, source, "healthy_peer_groups",
                       &s->healthy_peer_groups);
    restore_source_int(ndb, source, "healthy_max_peer_group_size",
                       &s->healthy_max_peer_group_size);
    restore_source_int(ndb, source, "addnode_count", &s->addnode_count);
    restore_source_int(ndb, source, "addnode_backoff_active",
                       &s->addnode_backoff_active);
    restore_source_int(ndb, source, "addnode_backoff_max_sec",
                       &s->addnode_backoff_max_sec);
    restore_source_int(ndb, source, "addnode_tcp_failures",
                       &s->addnode_tcp_failures);
    restore_source_int(ndb, source, "addnode_protocol_failures",
                       &s->addnode_protocol_failures);
    restore_source_int(ndb, source, "progress_current",
                       &s->progress_current);
    restore_source_int(ndb, source, "progress_total", &s->progress_total);
    restore_source_int(ndb, source, "lag", &s->lag);
    restore_source_int(ndb, source, "retry_count", &s->retry_count);
    restore_source_int(ndb, source, "distinct_peer_count",
                       &s->distinct_peer_count);
    restore_source_int(ndb, source, "serving_peer_id", &s->serving_peer_id);
}

static void persist_decision(struct node_db *ndb,
                             const char *op,
                             const struct cac_decision *d,
                             int64_t when,
                             int64_t total)
{
    if (!ndb || !d)
        return;

    (void)persist_text(ndb, BSP_KEY_LAST_OP, op ? op : "unknown");
    (void)node_db_state_set_int(ndb, BSP_KEY_LAST_TIME, when);
    (void)node_db_state_set_int(ndb, BSP_KEY_DECISIONS_TOTAL, total);
    (void)node_db_state_set_int(ndb, BSP_KEY_RESULT, (int64_t)d->result);
    (void)node_db_state_set_int(ndb, BSP_KEY_SOURCE,
                                (int64_t)d->selected_source);
    (void)node_db_state_set_int(ndb, BSP_KEY_ACTIVATION_ALLOWED,
                                d->activation_allowed ? 1 : 0);
    (void)node_db_state_set_int(ndb, BSP_KEY_MIRROR_FALLBACK_ALLOWED,
                                d->mirror_fallback_allowed ? 1 : 0);
    (void)node_db_state_set_int(ndb, BSP_KEY_SELECTED_SCORE,
                                (int64_t)d->selected_score);
    (void)persist_text(ndb, BSP_KEY_REASON, d->reason);
    (void)persist_text(ndb, BSP_KEY_BLOCKER, d->blocker);
    (void)node_db_state_set_int(ndb, BSP_KEY_LOCAL_HEIGHT,
                                (int64_t)d->local_height);
    (void)node_db_state_set_int(ndb, BSP_KEY_BEST_HEADER_HEIGHT,
                                (int64_t)d->best_header_height);
    (void)node_db_state_set_int(ndb, BSP_KEY_TARGET_HEIGHT,
                                (int64_t)d->target_height);

    for (int i = 1; i < CAC_SOURCE_NUM; i++)
        persist_source_snapshot(ndb, (enum cac_source)i, &d->sources[i]);

    if (d->selected_source > CAC_SOURCE_NONE &&
        d->selected_source < CAC_SOURCE_NUM) {
        const struct cac_source_status *s = &d->sources[d->selected_source];
        (void)persist_text(ndb, BSP_KEY_SOURCE_STATE, s->state);
        (void)persist_text(ndb, BSP_KEY_SOURCE_REASON, s->reason);
        (void)persist_text(ndb, BSP_KEY_SOURCE_BLOCKER, s->blocker);
        (void)node_db_state_set_int(ndb, BSP_KEY_SOURCE_HEIGHT,
                                    (int64_t)s->height);
        (void)node_db_state_set_int(ndb, BSP_KEY_SOURCE_HEALTHY,
                                    s->healthy ? 1 : 0);
        (void)node_db_state_set_int(ndb, BSP_KEY_SOURCE_AVAILABLE,
                                    s->available ? 1 : 0);
        (void)node_db_state_set_int(ndb, BSP_KEY_SOURCE_BLOCKED,
                                    s->blocked ? 1 : 0);
    }
}

static void restore_decision(struct node_db *ndb)
{
    if (!ndb)
        return;

    struct cac_decision d;
    memset(&d, 0, sizeof(d));
    char op[32] = {0};
    char reason[sizeof(d.reason)] = {0};
    char blocker[sizeof(d.blocker)] = {0};
    char source_state[sizeof(d.sources[0].state)] = {0};
    char source_reason[sizeof(d.sources[0].reason)] = {0};
    char source_block_text[sizeof(d.sources[0].blocker)] = {0};
    size_t len = 0;
    int64_t v = 0;
    int64_t total = 0;
    int64_t when = 0;

    if (!node_db_state_get(ndb, BSP_KEY_LAST_OP, op, sizeof(op) - 1, &len))
        return;
    if (!node_db_state_get_int(ndb, BSP_KEY_RESULT, &v))
        return;
    d.result = (enum cac_decision_result)v;
    if (!node_db_state_get_int(ndb, BSP_KEY_SOURCE, &v))
        return;
    d.selected_source = (enum cac_source)v;
    if (node_db_state_get_int(ndb, BSP_KEY_ACTIVATION_ALLOWED, &v))
        d.activation_allowed = v != 0;
    if (node_db_state_get_int(ndb, BSP_KEY_MIRROR_FALLBACK_ALLOWED, &v))
        d.mirror_fallback_allowed = v != 0;
    if (node_db_state_get_int(ndb, BSP_KEY_SELECTED_SCORE, &v))
        d.selected_score = (int)v;
    if (node_db_state_get(ndb, BSP_KEY_REASON, reason,
                          sizeof(reason) - 1, &len))
        copy_text(d.reason, sizeof(d.reason), reason);
    if (node_db_state_get(ndb, BSP_KEY_BLOCKER, blocker,
                          sizeof(blocker) - 1, &len))
        copy_text(d.blocker, sizeof(d.blocker), blocker);
    if (node_db_state_get_int(ndb, BSP_KEY_LOCAL_HEIGHT, &v))
        d.local_height = (int)v;
    if (node_db_state_get_int(ndb, BSP_KEY_BEST_HEADER_HEIGHT, &v))
        d.best_header_height = (int)v;
    if (node_db_state_get_int(ndb, BSP_KEY_TARGET_HEIGHT, &v))
        d.target_height = (int)v;
    for (int i = 1; i < CAC_SOURCE_NUM; i++)
        restore_source_snapshot(ndb, (enum cac_source)i, &d.sources[i]);
    if (d.selected_source > CAC_SOURCE_NONE &&
        d.selected_source < CAC_SOURCE_NUM) {
        struct cac_source_status *s = &d.sources[d.selected_source];
        s->source = d.selected_source;
        s->score = d.selected_score;
        if (node_db_state_get(ndb, BSP_KEY_SOURCE_STATE, source_state,
                              sizeof(source_state) - 1, &len))
            copy_text(s->state, sizeof(s->state), source_state);
        if (node_db_state_get(ndb, BSP_KEY_SOURCE_REASON, source_reason,
                              sizeof(source_reason) - 1, &len))
            copy_text(s->reason, sizeof(s->reason), source_reason);
        if (node_db_state_get(ndb, BSP_KEY_SOURCE_BLOCKER, source_block_text,
                              sizeof(source_block_text) - 1, &len))
            copy_text(s->blocker, sizeof(s->blocker), source_block_text);
        if (node_db_state_get_int(ndb, BSP_KEY_SOURCE_HEIGHT, &v))
            s->height = (int)v;
        if (node_db_state_get_int(ndb, BSP_KEY_SOURCE_HEALTHY, &v))
            s->healthy = v != 0;
        if (node_db_state_get_int(ndb, BSP_KEY_SOURCE_AVAILABLE, &v))
            s->available = v != 0;
        if (node_db_state_get_int(ndb, BSP_KEY_SOURCE_BLOCKED, &v))
            s->blocked = v != 0;
    }
    (void)node_db_state_get_int(ndb, BSP_KEY_LAST_TIME, &when);
    (void)node_db_state_get_int(ndb, BSP_KEY_DECISIONS_TOTAL, &total);

    bsp_lock_init_once();
    zcl_mutex_lock(&g_bsp.lock);
    g_bsp.last = d;
    g_bsp.has_last = true;
    g_bsp.last_decision_time = when;
    g_bsp.decisions_total = total > 0 ? total : 1;
    copy_text(g_bsp.last_op, sizeof(g_bsp.last_op), op);
    zcl_mutex_unlock(&g_bsp.lock);
}

static void restore_projection_deferral(struct node_db *ndb)
{
    if (!ndb)
        return;

    int64_t total = 0;
    int64_t height = 0;
    int64_t when = 0;
    char reason[64] = {0};
    size_t len = 0;

    (void)node_db_state_get_int(ndb, BSP_KEY_PROJECTION_DEFERRED_TOTAL,
                                &total);
    (void)node_db_state_get_int(ndb,
                                BSP_KEY_LAST_PROJECTION_DEFERRED_HEIGHT,
                                &height);
    (void)node_db_state_get_int(ndb, BSP_KEY_LAST_PROJECTION_DEFERRED_TIME,
                                &when);
    (void)node_db_state_get(ndb, BSP_KEY_LAST_PROJECTION_DEFERRED_REASON,
                            reason, sizeof(reason) - 1, &len);

    bsp_lock_init_once();
    zcl_mutex_lock(&g_bsp.lock);
    g_bsp.projection_deferred_total = total;
    g_bsp.last_projection_deferred_height = (int)height;
    g_bsp.last_projection_deferred_time = when;
    copy_text(g_bsp.last_projection_deferred_reason,
              sizeof(g_bsp.last_projection_deferred_reason),
              reason);
    zcl_mutex_unlock(&g_bsp.lock);
}

static void enrich_projection_deferral(struct cac_decision *d)
{
    if (!d)
        return;

    bsp_lock_init_once();
    zcl_mutex_lock(&g_bsp.lock);
    d->projection_deferred_total = g_bsp.projection_deferred_total;
    d->last_projection_deferred_height =
        g_bsp.last_projection_deferred_height;
    d->last_projection_deferred_time = g_bsp.last_projection_deferred_time;
    copy_text(d->last_projection_deferred_reason,
              sizeof(d->last_projection_deferred_reason),
              g_bsp.last_projection_deferred_reason);
    zcl_mutex_unlock(&g_bsp.lock);
}

static void record_decision(const char *op, const struct cac_decision *d)
{
    if (!d) return;
    struct node_db *ndb = NULL;
    int64_t when = (int64_t)platform_time_wall_time_t();
    int64_t total = 0;
    char op_copy[32];
    copy_text(op_copy, sizeof(op_copy), op ? op : "unknown");

    bsp_lock_init_once();
    zcl_mutex_lock(&g_bsp.lock);
    g_bsp.last = *d;
    g_bsp.has_last = true;
    g_bsp.last_decision_time = when;
    copy_text(g_bsp.last_op, sizeof(g_bsp.last_op), op_copy);
    g_bsp.decisions_total++;
    total = g_bsp.decisions_total;
    ndb = g_bsp.node_db;
    zcl_mutex_unlock(&g_bsp.lock);

    persist_decision(ndb, op_copy, d, when, total);
}

static void emit_decision_event(const char *op,
                                const char *action_key,
                                bool action_value,
                                const struct cac_decision *d,
                                const char *extra_fmt,
                                ...)
{
    if (!d) return;
    (void)action_key;

    char extra[256] = {0};
    if (extra_fmt && *extra_fmt) {
        va_list ap;
        va_start(ap, extra_fmt);
        vsnprintf(extra, sizeof(extra), extra_fmt, ap);
        va_end(ap);
    }

    const struct cac_source_status *s = NULL;
    if (d->selected_source > CAC_SOURCE_NONE &&
        d->selected_source < CAC_SOURCE_NUM)
        s = &d->sources[d->selected_source];
    const char *selection_blocker =
        s && s->selection_blocker[0] ? s->selection_blocker : "-";

    const char *source_name = cac_source_name(d->selected_source);
    const char *trust_name = cac_source_trust_name(d->selected_source);
    const char *decision_name = cac_decision_result_name(d->result);

    event_emitf(EV_CHAIN_ADVANCE_DECISION, 0,
                "op=%s ok=%s authority=local_consensus_validation "
                "source=%s trust=%s decision=%s score=%d "
                "reason=%s lh=%d th=%d sh=%d sel=%s sb=%s%s%s",
                op ? op : "unknown",
                action_value ? "true" : "false",
                source_name,
                trust_name,
                decision_name,
                d->selected_score,
                d->reason,
                d->local_height,
                d->target_height,
                s ? s->height : 0,
                s && s->selectable ? "true" : "false",
                selection_blocker,
                extra[0] ? " " : "",
                extra);
}

static const char *cac_source_class_name(enum cac_source source)
{
    switch (source) {
        case CAC_SOURCE_NONE:             return "none";
        case CAC_SOURCE_P2P:              return "native_p2p";
        case CAC_SOURCE_SNAPSHOT:         return "snapshot";
        case CAC_SOURCE_LOCAL_IMPORT:     return "local_import";
        case CAC_SOURCE_ZCLASSICD_MIRROR: return "legacy_advisory";
        case CAC_SOURCE_NUM:              break;
    }
    return "unknown";
}

bool block_source_policy_peer_floor_recovery_needed(
    int healthy_outbound,
    int min_healthy,
    int local_height,
    int peer_height,
    struct cac_decision *out)
{
    struct cac_plan_input in;
    struct cac_decision local_out;
    struct cac_decision *decision = out ? out : &local_out;
    int target_height = peer_height > local_height ? peer_height
                                                   : local_height;

    memset(&in, 0, sizeof(in));
    in.local_height = local_height;
    in.best_header_height = local_height;
    in.target_height = target_height;

    struct cac_source_status *p2p = &in.sources[CAC_SOURCE_P2P];
    p2p->source = CAC_SOURCE_P2P;
    p2p->available = healthy_outbound > 0;
    p2p->healthy = min_healthy > 0 && healthy_outbound >= min_healthy;
    p2p->height = peer_height;
    p2p->healthy_peers = healthy_outbound;
    p2p->progress_current = healthy_outbound;
    p2p->progress_total = min_healthy;
    copy_text(p2p->state, sizeof(p2p->state),
              p2p->healthy ? "healthy" : "peer_floor");
    snprintf(p2p->reason, sizeof(p2p->reason),
             "healthy_outbound=%d min_healthy=%d",
             healthy_outbound, min_healthy);
    if (healthy_outbound < min_healthy) {
        snprintf(p2p->blocker, sizeof(p2p->blocker), "peer_floor");
    }

    block_source_policy_plan(&in, decision);
    record_decision("peer_floor", decision);

    bool recover = healthy_outbound < min_healthy &&
                   decision->selected_source != CAC_SOURCE_P2P;
    const char *p2p_blocker =
        decision->sources[CAC_SOURCE_P2P].selection_blocker[0] ?
        decision->sources[CAC_SOURCE_P2P].selection_blocker : "-";
    emit_decision_event(
        "peer_floor", "recover", recover, decision,
        "healthy=%d min=%d local=%d peer=%d p2psb=%s",
        healthy_outbound, min_healthy, local_height, peer_height,
        p2p_blocker);
    return recover;
}

bool block_source_policy_snapshot_offer_allowed(
    int local_height,
    int snapshot_height,
    int peer_tip_height,
    bool offer_valid,
    const char *reason,
    struct cac_decision *out)
{
    struct cac_plan_input in;
    struct cac_decision local_out;
    struct cac_decision *decision = out ? out : &local_out;
    int target_height = peer_tip_height > snapshot_height ? peer_tip_height
                                                          : snapshot_height;

    memset(&in, 0, sizeof(in));
    in.local_height = local_height;
    in.best_header_height = local_height;
    in.target_height = target_height > local_height ? target_height
                                                    : local_height;

    struct cac_source_status *snap = &in.sources[CAC_SOURCE_SNAPSHOT];
    snap->source = CAC_SOURCE_SNAPSHOT;
    snap->available = true;
    snap->healthy = offer_valid;
    snap->authorized = offer_valid;
    snap->blocked = !offer_valid;
    snap->height = snapshot_height;
    snap->progress_current = snapshot_height;
    snap->progress_total = peer_tip_height;
    copy_text(snap->state, sizeof(snap->state),
              offer_valid ? "offer_valid" : "offer_rejected");
    snprintf(snap->reason, sizeof(snap->reason), "%s",
             reason && *reason ? reason : "snapshot_offer");
    if (!offer_valid) {
        snprintf(snap->blocker, sizeof(snap->blocker), "%s",
                 reason && *reason ? reason : "snapshot_offer_rejected");
    }

    block_source_policy_plan(&in, decision);
    record_decision("snapshot_offer", decision);

    bool allowed = offer_valid &&
                   decision->selected_source == CAC_SOURCE_SNAPSHOT;
    emit_decision_event(
        "snapshot_offer", "allowed", allowed, decision,
        "local=%d snapshot=%d peer_tip=%d",
        local_height, snapshot_height, peer_tip_height);
    return allowed;
}

bool block_source_policy_local_header_refill_needed(
    int local_height,
    int missing_height,
    int peer_height,
    int eligible_peers,
    int retry_count,
    bool retries_exhausted,
    struct cac_decision *out)
{
    struct cac_plan_input in;
    struct cac_decision local_out;
    struct cac_decision *decision = out ? out : &local_out;
    int target_height = peer_height > missing_height ? peer_height
                                                     : missing_height;

    memset(&in, 0, sizeof(in));
    in.local_height = local_height;
    in.best_header_height = local_height;
    in.target_height = target_height > local_height ? target_height
                                                    : local_height;
    in.local_recovery_active = true;
    in.local_retries_exhausted = retries_exhausted;

    struct cac_source_status *li = &in.sources[CAC_SOURCE_LOCAL_IMPORT];
    li->source = CAC_SOURCE_LOCAL_IMPORT;
    li->available = missing_height == local_height + 1;
    li->healthy = eligible_peers > 0;
    li->height = peer_height;
    li->progress_current = local_height;
    li->progress_total = missing_height;
    li->retry_count = retry_count;
    li->distinct_peer_count = eligible_peers;
    copy_text(li->state, sizeof(li->state),
              li->healthy ? "refill_ready" : "waiting_for_peer");
    snprintf(li->reason, sizeof(li->reason),
             "missing_height=%d eligible_peers=%d retry=%d",
             missing_height, eligible_peers, retry_count);
    if (!li->available || !li->healthy) {
        snprintf(li->blocker, sizeof(li->blocker),
                 "local_header_refill_no_peer");
    }

    block_source_policy_plan(&in, decision);
    record_decision("local_header_refill", decision);

    bool proceed = decision->result != CAC_DECISION_BLOCKED;
    emit_decision_event(
        "local_header_refill", "proceed", proceed, decision,
        "local=%d missing=%d peer=%d eligible=%d retry=%d",
        local_height, missing_height, peer_height, eligible_peers,
        retry_count);
    return proceed;
}

void block_source_policy_init(struct connman *cm,
                              struct main_state *ms,
                              struct node_db *ndb)
{
    bsp_lock_init_once();
    zcl_mutex_lock(&g_bsp.lock);
    g_bsp.connman = cm;
    g_bsp.main_state = ms;
    g_bsp.node_db = ndb;
    zcl_mutex_unlock(&g_bsp.lock);
    restore_decision(ndb);
    restore_projection_deferral(ndb);
}

static int runtime_local_height(struct main_state *ms)
{
    if (!ms) return -1; /* raw-return-ok:sentinel */
    return active_chain_height(&ms->chain_active);
}

static int runtime_best_header_height(struct main_state *ms)
{
    if (!ms || !ms->pindex_best_header) return -1; /* raw-return-ok:sentinel */
    return ms->pindex_best_header->nHeight;
}

static bool p2p_minimum_viable(const struct cac_plan_input *in,
                               const struct cac_source_status *p2p,
                               const struct connman_outbound_health *ph)
{
    if (!in || !p2p || !ph)
        return false;
    if (ph->healthy >= 3)
        return true;
    if (ph->healthy < 2)
        return false;
    if (ph->healthy_ipv4_group_count < 2)
        return false;
    if (p2p->height < 0)
        return false;
    if (p2p->lag > 1)
        return false;
    if (in->local_height >= 0) {
        int64_t local_gap = (int64_t)in->local_height - p2p->height;
        if (local_gap <= 0)
            return true;
        if (local_gap > 1)
            return false;
        return ph->inbound_healthy > 0;
    }
    return true;
}

static void build_runtime_input(struct cac_plan_input *in)
{
    memset(in, 0, sizeof(*in));
    in->local_height = -1;
    in->best_header_height = -1;
    in->target_height = -1;

    bsp_lock_init_once();
    zcl_mutex_lock(&g_bsp.lock);
    struct connman *cm = g_bsp.connman;
    struct main_state *ms = g_bsp.main_state;
    struct node_db *ndb = g_bsp.node_db;
    zcl_mutex_unlock(&g_bsp.lock);

    in->local_height = runtime_local_height(ms);
    in->best_header_height = runtime_best_header_height(ms);

    struct peer_lifecycle_summary pls;
    memset(&pls, 0, sizeof(pls));
    peer_lifecycle_get_summary(&pls);

    struct cac_source_status *p2p = &in->sources[CAC_SOURCE_P2P];
    struct connman_outbound_health ph;
    memset(&ph, 0, sizeof(ph));
    if (cm)
        connman_get_outbound_health(cm, &ph);
    p2p->source = CAC_SOURCE_P2P;
    p2p->available = pls.handshake_complete > 0 ||
                     ph.healthy > 0;
    p2p->height = cm ? connman_max_peer_height(cm) : -1;
    p2p->lag = (in->best_header_height > p2p->height && p2p->height >= 0)
        ? (int64_t)(in->best_header_height - p2p->height)
        : 0;
    p2p->healthy = p2p_minimum_viable(in, p2p, &ph);
    p2p->timeouts = pls.timeout;
    p2p->failures = pls.rejected;
    p2p->outbound_total = (int64_t)ph.outbound_total;
    p2p->inbound_total = (int64_t)ph.inbound_total;
    p2p->healthy_peers = (int64_t)ph.healthy;
    p2p->inbound_healthy_peers = (int64_t)ph.inbound_healthy;
    p2p->total_healthy_peers = (int64_t)(ph.healthy + ph.inbound_healthy);
    p2p->connecting_peers = (int64_t)ph.connecting;
    p2p->handshake_incomplete = (int64_t)ph.handshake_incomplete;
    p2p->inbound_handshake_incomplete =
        (int64_t)ph.inbound_handshake_incomplete;
    p2p->peer_groups = (int64_t)ph.ipv4_group_count;
    p2p->max_peer_group_size = (int64_t)ph.ipv4_max_group_size;
    p2p->healthy_peer_groups = (int64_t)ph.healthy_ipv4_group_count;
    p2p->healthy_max_peer_group_size =
        (int64_t)ph.healthy_ipv4_max_group_size;
    p2p->addnode_count = (int64_t)ph.addnode_count;
    p2p->addnode_backoff_active = (int64_t)ph.addnode_backoff_active;
    p2p->addnode_backoff_max_sec = (int64_t)ph.addnode_backoff_max_sec;
    p2p->addnode_tcp_failures = ph.addnode_tcp_failures;
    p2p->addnode_protocol_failures = ph.addnode_protocol_failures;
    p2p->progress_current = pls.handshake_complete;
    p2p->progress_total = pls.attempted;
    copy_text(p2p->state, sizeof(p2p->state),
              p2p->healthy ? "healthy" :
              (p2p->available ? "degraded" : "unavailable"));
    snprintf(p2p->reason, sizeof(p2p->reason),
             "peer_height=%d header_height=%d stale_lag=%lld "
             "handshakes=%lld healthy=%zu inbound_healthy=%zu "
             "total_healthy=%zu outbound=%zu inbound=%zu connecting=%zu "
             "groups=%zu max_group=%zu healthy_groups=%zu "
             "healthy_max_group=%zu ideal_floor=3 backoff=%zu/%zu max=%d "
             "tcp_fail=%lld proto_fail=%lld",
             p2p->height,
             in->best_header_height,
             (long long)p2p->lag,
             (long long)pls.handshake_complete,
             ph.healthy,
             ph.inbound_healthy,
             ph.healthy + ph.inbound_healthy,
             ph.outbound_total,
             ph.inbound_total,
             ph.connecting,
             ph.ipv4_group_count,
             ph.ipv4_max_group_size,
             ph.healthy_ipv4_group_count,
             ph.healthy_ipv4_max_group_size,
             ph.addnode_backoff_active,
             ph.addnode_count,
             ph.addnode_backoff_max_sec,
             (long long)ph.addnode_tcp_failures,
             (long long)ph.addnode_protocol_failures);
    if (ph.outbound_total > 0 && ph.healthy == 0)
        copy_text(p2p->blocker, sizeof(p2p->blocker), "no_healthy_outbound");
    else if (p2p->available && !p2p->healthy)
        copy_text(p2p->blocker, sizeof(p2p->blocker), "peer_floor");

    struct watchdog_local_recovery_stats wr;
    memset(&wr, 0, sizeof(wr));
    sync_monitor_get_local_recovery_stats(&wr);
    in->local_recovery_active = wr.active;
    in->local_retries_exhausted = wr.retries_exhausted;

    struct cac_source_status *li = &in->sources[CAC_SOURCE_LOCAL_IMPORT];
    li->source = CAC_SOURCE_LOCAL_IMPORT;
    li->available = wr.active;
    li->healthy = wr.active && wr.distinct_peer_count > 0;
    li->height = wr.missing_height > 0 ? wr.missing_height - 1
                                       : in->local_height;
    li->progress_current = li->height;
    li->progress_total = wr.missing_height;
    li->retry_count = wr.retry_count;
    li->distinct_peer_count = wr.distinct_peer_count;
    copy_text(li->state, sizeof(li->state),
              wr.mode[0] ? wr.mode : (wr.active ? "active" : "idle"));
    snprintf(li->reason, sizeof(li->reason),
             "mode=%s retries=%d distinct_peers=%d",
             wr.mode, wr.retry_count, wr.distinct_peer_count);

    struct snapshot_sync_service *ssvc = app_runtime_snapshot_sync();
    struct snapsync_status sstat;
    memset(&sstat, 0, sizeof(sstat));
    if (ssvc)
        snapsync_get_status_snapshot(ssvc, &sstat);
    struct cac_source_status *snap = &in->sources[CAC_SOURCE_SNAPSHOT];
    snap->source = CAC_SOURCE_SNAPSHOT;
    snap->available = ssvc && sstat.state != SNAPSYNC_IDLE;
    snap->healthy = ssvc &&
        (sstat.state == SNAPSYNC_NEGOTIATING ||
         sstat.state == SNAPSYNC_RECEIVING ||
         sstat.state == SNAPSYNC_VERIFYING ||
         sstat.state == SNAPSYNC_COMPLETE);
    snap->authorized = sstat.state == SNAPSYNC_COMPLETE;
    snap->blocked = sstat.state == SNAPSYNC_FAILED;
    snap->height = sstat.offered_height;
    snap->progress_current = sstat.staged_row_count;
    snap->progress_total = (int64_t)sstat.offered_count;
    snap->serving_peer_id = (int64_t)sstat.serving_peer_id;
    copy_text(snap->state, sizeof(snap->state),
              snapsync_state_name(sstat.state));
    if (snap->blocked)
        copy_text(snap->blocker, sizeof(snap->blocker), "snapshot_failed");
    snprintf(snap->reason, sizeof(snap->reason),
             "state=%s peer=%u staged=%lld offered=%llu",
             snapsync_state_name(sstat.state),
             sstat.serving_peer_id,
             (long long)sstat.staged_row_count,
             (unsigned long long)sstat.offered_count);
    if (snap->height > in->target_height)
        in->target_height = snap->height;

    struct legacy_mirror_sync_stats msnap;
    memset(&msnap, 0, sizeof(msnap));
    legacy_mirror_sync_stats_snapshot(&msnap);
    struct cac_source_status *mir = &in->sources[CAC_SOURCE_ZCLASSICD_MIRROR];
    mir->source = CAC_SOURCE_ZCLASSICD_MIRROR;
    mir->available = msnap.enabled && msnap.reachable;
    mir->healthy = msnap.enabled && msnap.reachable && msnap.lag >= 0 &&
                   (msnap.state[0] == '\0' ||
                    strcmp(msnap.state, "blocked") != 0);
    mir->authorized = true;
    mir->height = msnap.legacy_height;
    mir->failures = msnap.rpc_errors;
    mir->progress_current = msnap.blocks_applied;
    mir->progress_total = msnap.target_height;
    mir->lag = msnap.lag;
    mir->retry_count = msnap.local_retry_count;
    mir->distinct_peer_count = msnap.local_distinct_peer_count;
    copy_text(mir->state, sizeof(mir->state),
              msnap.state[0] ? msnap.state :
              (mir->available ? "healthy" : "unavailable"));
    mir->blocked = msnap.activation_blocker[0] != '\0' ||
                   strcmp(msnap.state, "blocked") == 0;
    if (mir->blocked) {
        copy_text(mir->blocker, sizeof(mir->blocker),
                  msnap.activation_blocker[0] ? msnap.activation_blocker
                                               : msnap.last_blocker_code);
        mir->blocked_class = classify_mirror_blocker_class(mir->blocker);
    } else {
        mir->blocked_class = BLOCKER_TRANSIENT;
    }
    snprintf(mir->reason, sizeof(mir->reason),
             "state=%s lag=%d local_retries_exhausted=%s",
             msnap.state, msnap.lag,
             msnap.local_retries_exhausted ? "true" : "false");
    in->mirror_lag_sla_breach_blocks = msnap.lag_sla_breach_blocks;
    if (msnap.target_height > in->target_height)
        in->target_height = msnap.target_height;
    if (msnap.legacy_height > in->target_height)
        in->target_height = msnap.legacy_height;

    if (p2p->height > in->target_height)
        in->target_height = p2p->height;
    if (in->best_header_height > in->target_height)
        in->target_height = in->best_header_height;
    if (in->target_height < in->local_height)
        in->target_height = in->local_height;

    in->projection_height = -1;
    in->projection_lag = -1;
    in->projection_deferred = false;
    copy_text(in->projection_state, sizeof(in->projection_state), "unknown");
    if (ndb && ndb->open) {
        int projection_height = db_block_max_height(ndb);
        in->projection_height = projection_height;
        if (projection_height >= 0) {
            int projection_basis = in->local_height;
            if (in->target_height > projection_basis)
                projection_basis = in->target_height;
            if (projection_basis < 0)
                projection_basis = projection_height;
            in->projection_lag =
                projection_basis > projection_height
                    ? (int64_t)(projection_basis - projection_height)
                    : 0;
            in->projection_deferred = in->projection_lag > 0;
            copy_text(in->projection_state, sizeof(in->projection_state),
                      in->projection_deferred ? "deferred" : "current");
        } else {
            in->projection_deferred = in->local_height > 0;
            copy_text(in->projection_state, sizeof(in->projection_state),
                      in->projection_deferred ? "missing" : "empty");
        }
    }
}

void block_source_policy_get_status(struct cac_decision *out)
{
    if (!out) return;
    struct cac_plan_input in;
    build_runtime_input(&in);
    block_source_policy_plan(&in, out);
    enrich_projection_deferral(out);
}

void block_source_policy_note_projection_deferred(int height,
                                                  const char *reason)
{
    struct node_db *ndb = NULL;
    int64_t total = 0;
    int64_t when = (int64_t)platform_time_wall_time_t();
    char reason_copy[64];

    copy_text(reason_copy, sizeof(reason_copy),
              reason && *reason ? reason : "unknown");

    bsp_lock_init_once();
    zcl_mutex_lock(&g_bsp.lock);
    g_bsp.projection_deferred_total++;
    total = g_bsp.projection_deferred_total;
    g_bsp.last_projection_deferred_height = height;
    g_bsp.last_projection_deferred_time = when;
    copy_text(g_bsp.last_projection_deferred_reason,
              sizeof(g_bsp.last_projection_deferred_reason),
              reason_copy);
    ndb = g_bsp.node_db;
    zcl_mutex_unlock(&g_bsp.lock);

    if (ndb) {
        (void)node_db_state_set_int(ndb, BSP_KEY_PROJECTION_DEFERRED_TOTAL,
                                    total);
        (void)node_db_state_set_int(
            ndb, BSP_KEY_LAST_PROJECTION_DEFERRED_HEIGHT,
            (int64_t)height);
        (void)node_db_state_set_int(ndb, BSP_KEY_LAST_PROJECTION_DEFERRED_TIME,
                                    when);
        (void)persist_text(ndb, BSP_KEY_LAST_PROJECTION_DEFERRED_REASON,
                           reason_copy);
    }

    event_emitf(EV_CHAIN_ADVANCE_DECISION, 0,
                "op=projection_deferred reason=%s h=%d total=%lld "
                "authority=local_consensus_validation",
                reason_copy, height, (long long)total);
}

static void source_to_json(const struct cac_source_status *s,
                           struct json_value *out)
{
    json_set_object(out);
    json_push_kv_str(out, "source", cac_source_name(s->source));
    json_push_kv_str(out, "source_class", cac_source_class_name(s->source));
    json_push_kv_str(out, "candidate_source", cac_source_class_name(s->source));
    json_push_kv_str(out, "trust", cac_source_trust_name(s->source));
    json_push_kv_str(out, "candidate_trust", cac_source_trust_name(s->source));
    json_push_kv_bool(out, "available", s->available);
    json_push_kv_bool(out, "healthy", s->healthy);
    json_push_kv_bool(out, "blocked", s->blocked);
    json_push_kv_str(out, "blocked_class",
                     blocker_class_name(s->blocked_class));
    json_push_kv_bool(out, "authorized", s->authorized);
    json_push_kv_bool(out, "selectable", s->selectable);
    json_push_kv_str(out, "selection_blocker", s->selection_blocker);
    json_push_kv_int(out, "height", (int64_t)s->height);
    json_push_kv_int(out, "score", (int64_t)s->score);
    json_push_kv_int(out, "score_base", s->score_base);
    json_push_kv_int(out, "score_health", s->score_health);
    json_push_kv_int(out, "score_height", s->score_height);
    json_push_kv_int(out, "score_authorized", s->score_authorized);
    json_push_kv_int(out, "score_target_lag_penalty",
                     s->score_target_lag_penalty);
    json_push_kv_int(out, "score_failure_penalty",
                     s->score_failure_penalty);
    json_push_kv_int(out, "score_mirror_gate_penalty",
                     s->score_mirror_gate_penalty);
    json_push_kv_int(out, "failures", s->failures);
    json_push_kv_int(out, "timeouts", s->timeouts);
    json_push_kv_int(out, "outbound_total", s->outbound_total);
    json_push_kv_int(out, "inbound_total", s->inbound_total);
    json_push_kv_int(out, "healthy_peers", s->healthy_peers);
    json_push_kv_int(out, "inbound_healthy_peers",
                     s->inbound_healthy_peers);
    json_push_kv_int(out, "total_healthy_peers", s->total_healthy_peers);
    json_push_kv_int(out, "connecting_peers", s->connecting_peers);
    json_push_kv_int(out, "handshake_incomplete",
                     s->handshake_incomplete);
    json_push_kv_int(out, "inbound_handshake_incomplete",
                     s->inbound_handshake_incomplete);
    json_push_kv_int(out, "peer_groups", s->peer_groups);
    json_push_kv_int(out, "max_peer_group_size", s->max_peer_group_size);
    json_push_kv_int(out, "healthy_peer_groups", s->healthy_peer_groups);
    json_push_kv_int(out, "healthy_max_peer_group_size",
                     s->healthy_max_peer_group_size);
    json_push_kv_int(out, "addnode_count", s->addnode_count);
    json_push_kv_int(out, "addnode_backoff_active",
                     s->addnode_backoff_active);
    json_push_kv_int(out, "addnode_backoff_max_sec",
                     s->addnode_backoff_max_sec);
    json_push_kv_int(out, "addnode_tcp_failures",
                     s->addnode_tcp_failures);
    json_push_kv_int(out, "addnode_protocol_failures",
                     s->addnode_protocol_failures);
    json_push_kv_int(out, "progress_current", s->progress_current);
    json_push_kv_int(out, "progress_total", s->progress_total);
    json_push_kv_int(out, "lag", s->lag);
    json_push_kv_int(out, "candidate_lag", s->lag);
    json_push_kv_int(out, "retry_count", s->retry_count);
    json_push_kv_int(out, "distinct_peer_count", s->distinct_peer_count);
    json_push_kv_int(out, "serving_peer_id", s->serving_peer_id);
    json_push_kv_str(out, "state", s->state);
    json_push_kv_str(out, "reason", s->reason);
    json_push_kv_str(out, "blocker", s->blocker);
    json_push_kv_str(out, "candidate_blocker",
                     s->blocker[0] ? s->blocker : s->selection_blocker);
}

static void decision_to_json(const struct cac_decision *d,
                             struct json_value *out)
{
    json_set_object(out);
    if (!d) return;
    json_push_kv_str(out, "decision",
                     cac_decision_result_name(d->result));
    json_push_kv_str(out, "selected_source",
                     cac_source_name(d->selected_source));
    json_push_kv_str(out, "candidate_source",
                     cac_source_class_name(d->selected_source));
    json_push_kv_str(out, "selected_source_trust",
                     cac_source_trust_name(d->selected_source));
    json_push_kv_str(out, "candidate_trust",
                     cac_source_trust_name(d->selected_source));
    json_push_kv_str(out, "authority", "local_consensus_validation");
    json_push_kv_bool(out, "activation_allowed", d->activation_allowed);
    json_push_kv_bool(out, "mirror_fallback_allowed",
                      d->mirror_fallback_allowed);
    json_push_kv_int(out, "local_height", (int64_t)d->local_height);
    json_push_kv_int(out, "best_header_height",
                     (int64_t)d->best_header_height);
    json_push_kv_int(out, "target_height", (int64_t)d->target_height);
    json_push_kv_int(out, "projection_height",
                     (int64_t)d->projection_height);
    json_push_kv_int(out, "projection_lag", d->projection_lag);
    json_push_kv_bool(out, "projection_deferred",
                      d->projection_deferred);
    json_push_kv_str(out, "projection_state", d->projection_state);
    json_push_kv_int(out, "projection_deferred_total",
                     d->projection_deferred_total);
    json_push_kv_int(out, "last_projection_deferred_height",
                     d->last_projection_deferred_height);
    json_push_kv_int(out, "last_projection_deferred_time",
                     d->last_projection_deferred_time);
    json_push_kv_str(out, "last_projection_deferred_reason",
                     d->last_projection_deferred_reason);
    json_push_kv_int(out, "selected_score", (int64_t)d->selected_score);
    json_push_kv_str(out, "reason", d->reason);
    json_push_kv_str(out, "blocker", d->blocker);
    if (d->selected_source > CAC_SOURCE_NONE &&
        d->selected_source < CAC_SOURCE_NUM) {
        const struct cac_source_status *s = &d->sources[d->selected_source];
        json_push_kv_str(out, "selected_source_state", s->state);
        json_push_kv_str(out, "selected_source_reason", s->reason);
        json_push_kv_str(out, "selected_source_blocker", s->blocker);
        json_push_kv_bool(out, "selected_source_selectable",
                          s->selectable);
        json_push_kv_str(out, "selected_source_selection_blocker",
                         s->selection_blocker);
        json_push_kv_int(out, "selected_source_height", s->height);
        json_push_kv_int(out, "selected_source_score_base", s->score_base);
        json_push_kv_int(out, "selected_source_score_health",
                         s->score_health);
        json_push_kv_int(out, "selected_source_score_height",
                         s->score_height);
        json_push_kv_int(out, "selected_source_score_authorized",
                         s->score_authorized);
        json_push_kv_int(out, "selected_source_score_target_lag_penalty",
                         s->score_target_lag_penalty);
        json_push_kv_int(out, "selected_source_score_failure_penalty",
                         s->score_failure_penalty);
        json_push_kv_int(out, "selected_source_score_mirror_gate_penalty",
                         s->score_mirror_gate_penalty);
        json_push_kv_bool(out, "selected_source_healthy", s->healthy);
        json_push_kv_bool(out, "selected_source_available", s->available);
        json_push_kv_bool(out, "selected_source_blocked", s->blocked);
    }

    struct json_value arr = {0};
    json_set_array(&arr);
    for (int i = 1; i < CAC_SOURCE_NUM; i++) {
        struct cac_source_status source = d->sources[i];
        struct json_value child = {0};
        if (source.source == CAC_SOURCE_NONE)
            source.source = (enum cac_source)i;
        source_to_json(&source, &child);
        json_push_back(&arr, &child);
        json_free(&child);
    }
    json_push_kv(out, "sources", &arr);
    json_free(&arr);
}

bool block_source_policy_dump_state_json(struct json_value *out,
                                         const char *key)
{
    (void)key;
    if (!out) return false;
    struct cac_decision d;
    block_source_policy_get_status(&d);

    bsp_lock_init_once();
    zcl_mutex_lock(&g_bsp.lock);
    int64_t decisions_total = g_bsp.decisions_total;
    bool has_last = g_bsp.has_last;
    int64_t last_decision_time = g_bsp.last_decision_time;
    char last_op[32];
    struct cac_decision last = g_bsp.last;
    bool has_connman = g_bsp.connman != NULL;
    bool has_main_state = g_bsp.main_state != NULL;
    bool has_node_db = g_bsp.node_db != NULL;
    copy_text(last_op, sizeof(last_op), g_bsp.last_op);
    zcl_mutex_unlock(&g_bsp.lock);

    json_set_object(out);
    json_push_kv_bool(out, "initialized",
                      has_connman && has_main_state && has_node_db);
    json_push_kv_bool(out, "has_connman", has_connman);
    json_push_kv_bool(out, "has_main_state", has_main_state);
    json_push_kv_bool(out, "has_node_db", has_node_db);
    json_push_kv_str(out, "authority", "local_consensus_validation");
    json_push_kv_str(out, "decision",
                     cac_decision_result_name(d.result));
    json_push_kv_str(out, "selected_source",
                     cac_source_name(d.selected_source));
    json_push_kv_str(out, "candidate_source",
                     cac_source_class_name(d.selected_source));
    json_push_kv_str(out, "selected_source_trust",
                     cac_source_trust_name(d.selected_source));
    json_push_kv_str(out, "candidate_trust",
                     cac_source_trust_name(d.selected_source));
    json_push_kv_bool(out, "activation_allowed", d.activation_allowed);
    json_push_kv_bool(out, "mirror_fallback_allowed",
                      d.mirror_fallback_allowed);
    json_push_kv_int(out, "local_height", (int64_t)d.local_height);
    json_push_kv_int(out, "best_header_height",
                     (int64_t)d.best_header_height);
    json_push_kv_int(out, "target_height", (int64_t)d.target_height);
    json_push_kv_int(out, "projection_height",
                     (int64_t)d.projection_height);
    json_push_kv_int(out, "projection_lag", d.projection_lag);
    json_push_kv_bool(out, "projection_deferred",
                      d.projection_deferred);
    json_push_kv_str(out, "projection_state", d.projection_state);
    json_push_kv_int(out, "projection_deferred_total",
                     d.projection_deferred_total);
    json_push_kv_int(out, "last_projection_deferred_height",
                     d.last_projection_deferred_height);
    json_push_kv_int(out, "last_projection_deferred_time",
                     d.last_projection_deferred_time);
    json_push_kv_str(out, "last_projection_deferred_reason",
                     d.last_projection_deferred_reason);
    json_push_kv_int(out, "selected_score", (int64_t)d.selected_score);
    json_push_kv_str(out, "reason", d.reason);
    json_push_kv_str(out, "blocker", d.blocker);
    json_push_kv_int(out, "decisions_total", decisions_total);
    json_push_kv_bool(out, "has_last_decision", has_last);
    if (has_last) {
        struct json_value last_json = {0};
        last.projection_deferred_total = d.projection_deferred_total;
        last.last_projection_deferred_height =
            d.last_projection_deferred_height;
        last.last_projection_deferred_time =
            d.last_projection_deferred_time;
        copy_text(last.last_projection_deferred_reason,
                  sizeof(last.last_projection_deferred_reason),
                  d.last_projection_deferred_reason);
        decision_to_json(&last, &last_json);
        json_push_kv_str(&last_json, "op", last_op);
        json_push_kv_int(&last_json, "time", last_decision_time);
        json_push_kv(out, "last_decision", &last_json);
        json_free(&last_json);
    }

    struct json_value arr = {0};
    json_set_array(&arr);
    for (int i = 1; i < CAC_SOURCE_NUM; i++) {
        struct json_value child = {0};
        source_to_json(&d.sources[i], &child);
        json_push_back(&arr, &child);
        json_free(&child);
    }
    json_push_kv(out, "sources", &arr);
    json_free(&arr);
    return true;
}

void block_source_policy_reset_for_test(void)
{
    bsp_lock_init_once();
    zcl_mutex_lock(&g_bsp.lock);
    g_bsp.connman = NULL;
    g_bsp.main_state = NULL;
    g_bsp.node_db = NULL;
    memset(&g_bsp.last, 0, sizeof(g_bsp.last));
    g_bsp.has_last = false;
    g_bsp.last_decision_time = 0;
    memset(g_bsp.last_op, 0, sizeof(g_bsp.last_op));
    g_bsp.decisions_total = 0;
    g_bsp.projection_deferred_total = 0;
    g_bsp.last_projection_deferred_height = 0;
    g_bsp.last_projection_deferred_time = 0;
    memset(g_bsp.last_projection_deferred_reason, 0,
           sizeof(g_bsp.last_projection_deferred_reason));
    zcl_mutex_unlock(&g_bsp.lock);
}
