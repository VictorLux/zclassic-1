/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Cutover controller — runtime control + read-only preflight for the
 * guarded Wave-S authority cutovers (header_admit / validate_headers).
 *
 *   cutovermode [stage] [mode]            — read or flip runtime cutover
 *                                           modes (shadow/authoritative)
 *   cutoverpreflight [start] [end]        — conservative ready snapshot
 *                                           across every cutover gate
 *
 * Compile-time defaults stay SHADOW so a bad flip is one RPC to revert
 * rather than a rebuild + redeploy. Authoritative is refused unless the
 * preflight `ready` boolean is true and the flip uses stage=all.
 */

#include "platform/time_compat.h"
#include "controllers/diagnostics_internal.h"

#include "json/json.h"
#include "rpc/server.h"
#include "controllers/strong_params.h"
#include "services/chain_advance_coordinator.h"
#include "services/cutover_modes.h"
#include "jobs/header_admit_stage.h"
#include "jobs/validate_headers_stage.h"
#include "services/node_health_service.h"
#include "framework/condition.h"
#include "util/log_macros.h"

#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <limits.h>

/* ── RPC: cutovermode [stage] [mode] ──────────────────────────────── */

static const char *header_admit_mode_name(header_admit_mode_t mode)
{
    return mode == HEADER_ADMIT_MODE_AUTHORITATIVE
        ? "authoritative" : "shadow";
}

static const char *validate_headers_mode_name(validate_headers_mode_t mode)
{
    return mode == VALIDATE_HEADERS_MODE_AUTHORITATIVE
        ? "authoritative" : "shadow";
}

#define CUTOVER_PREFLIGHT_MAX_TIP_ADVANCE_AGE_SECS 180
#define CUTOVER_PREFLIGHT_MAX_GUARD_POLL_SECS 5
#define CUTOVER_PREFLIGHT_MAX_GUARD_WITNESS_SECS 60
#define CUTOVER_PREFLIGHT_GUARD_NAME "cutover_no_forward_progress"

static bool parse_cutover_mode(const char *s,
                               header_admit_mode_t *ha,
                               validate_headers_mode_t *vh)
{
    if (!s || !s[0]) return false;
    if (strcasecmp(s, "shadow") == 0) {
        if (ha) *ha = HEADER_ADMIT_MODE_SHADOW;
        if (vh) *vh = VALIDATE_HEADERS_MODE_SHADOW;
        return true;
    }
    if (strcasecmp(s, "authoritative") == 0 ||
        strcasecmp(s, "auth") == 0) {
        if (ha) *ha = HEADER_ADMIT_MODE_AUTHORITATIVE;
        if (vh) *vh = VALIDATE_HEADERS_MODE_AUTHORITATIVE;
        return true;
    }
    return false;
}

static cutover_stage_mode_t cutover_stage_mode_from_header_admit(
    header_admit_mode_t mode)
{
    return mode == HEADER_ADMIT_MODE_AUTHORITATIVE
        ? CUTOVER_STAGE_MODE_AUTHORITATIVE
        : CUTOVER_STAGE_MODE_SHADOW;
}

static cutover_stage_mode_t cutover_stage_mode_from_validate_headers(
    validate_headers_mode_t mode)
{
    return mode == VALIDATE_HEADERS_MODE_AUTHORITATIVE
        ? CUTOVER_STAGE_MODE_AUTHORITATIVE
        : CUTOVER_STAGE_MODE_SHADOW;
}

static void cutover_record_mode_change(
    const struct node_health_snapshot *health)
{
    cutover_modes_record_change(health ? health->tip_height : -1,
                                health ? health->header_height : -1,
                                health ? health->peer_best_height : -1,
                                health ? health->tip_lag : -1);
}

static void push_cutover_canary_state(
    struct json_value *out,
    const struct node_health_snapshot *health)
{
    int64_t current_tip = health ? health->tip_height : -1;
    struct cutover_canary_snapshot snap;
    cutover_modes_canary_snapshot(current_tip, &snap);
    const char *status = "inactive";
    if (snap.has_change) {
        if (snap.passed) {
            status = "passed";
        } else if (snap.failed) {
            status = "failed";
        } else if (snap.authoritative_active) {
            status = "pending";
        } else {
            status = "reverted";
        }
    }

    json_set_object(out);
    json_push_kv_bool(out, "has_change", snap.has_change);
    json_push_kv_bool(out, "authoritative_active", snap.authoritative_active);
    json_push_kv_str(out, "canary_status", status);
    json_push_kv_bool(out, "canary_failed", snap.failed);
    json_push_kv_int(out, "changed_at_unix", snap.changed_at_unix);
    json_push_kv_int(out, "change_height", snap.change_height);
    json_push_kv_int(out, "canary_target_height", snap.target_height);
    json_push_kv_int(out, "current_tip_height", snap.current_tip_height);
    json_push_kv_bool(out, "canary_passed", snap.passed);
    json_push_kv_int(out, "canary_elapsed_seconds", snap.elapsed_seconds);
    json_push_kv_int(out, "canary_deadline_unix", snap.deadline_unix);
    json_push_kv_int(out, "change_header_height",
                     snap.change_header_height);
    json_push_kv_int(out, "change_peer_best_height",
                     snap.change_peer_best_height);
    json_push_kv_int(out, "change_tip_lag", snap.change_tip_lag);
    json_push_kv_int(out, "watch_window_seconds",
                     snap.watch_window_seconds);
}

static void push_cutover_modes(struct json_value *result, bool changed,
                               const struct node_health_snapshot *health)
{
    struct json_value canary;
    json_init(&canary);

    json_set_object(result);
    json_push_kv_bool(result, "changed", changed);
    json_push_kv_str(result, "header_admit",
                     header_admit_mode_name(header_admit_get_mode()));
    json_push_kv_str(result, "validate_headers",
                     validate_headers_mode_name(validate_headers_get_mode()));
    if (changed) {
        json_push_kv_int(result, "changed_at_unix",
                         platform_time_wall_unix());
        if (health) {
            json_push_kv_int(result, "change_height", health->tip_height);
            json_push_kv_int(result, "canary_target_height",
                             health->tip_height >= 0
                                 ? health->tip_height + 1
                                 : 0);
            json_push_kv_int(result, "change_header_height",
                             health->header_height);
            json_push_kv_int(result, "change_peer_best_height",
                             health->peer_best_height);
            json_push_kv_int(result, "change_tip_lag", health->tip_lag);
        }
    }
    push_cutover_canary_state(&canary, health);
    json_push_kv(result, "cutover_state", &canary);
    json_free(&canary);
}

static bool cutover_stage_requests_authoritative(
    const char *stage,
    header_admit_mode_t ha_mode,
    validate_headers_mode_t vh_mode)
{
    if (strcasecmp(stage, "header_admit") == 0)
        return ha_mode == HEADER_ADMIT_MODE_AUTHORITATIVE;
    if (strcasecmp(stage, "validate_headers") == 0)
        return vh_mode == VALIDATE_HEADERS_MODE_AUTHORITATIVE;
    if (strcasecmp(stage, "all") == 0)
        return ha_mode == HEADER_ADMIT_MODE_AUTHORITATIVE ||
               vh_mode == VALIDATE_HEADERS_MODE_AUTHORITATIVE;
    return false;
}

static bool cutover_authoritative_request_is_paired(const char *stage)
{
    return stage && strcasecmp(stage, "all") == 0;
}

static bool cutover_preflight_ready_now(void)
{
    struct json_value params;
    struct json_value preflight;
    json_init(&params);
    json_init(&preflight);
    json_set_array(&params);

    bool ok = diag_rpc_cutoverpreflight(&params, false, &preflight);
    const struct json_value *ready = ok ? json_get(&preflight, "ready") : NULL;
    ok = ready && json_get_bool(ready);

    json_free(&preflight);
    json_free(&params);
    return ok;
}

bool diag_rpc_cutovermode(const struct json_value *params, bool help,
                          struct json_value *result)
{
    RPC_HELP(help, result,
        "cutovermode [stage] [mode]\n"
        "\nRead or set runtime cutover modes. stage is one of:\n"
        "  header_admit | validate_headers | all\n"
        "mode is one of:\n"
        "  shadow | authoritative\n"
        "\nAuthoritative mode is refused unless cutoverpreflight.ready is true.\n"
        "Authoritative mode must be applied with stage=all; partial stage "
        "requests are only valid for shadow reverts.\n"
        "\nExamples:\n"
        "  cutovermode\n"
        "  cutovermode all authoritative\n"
        "  cutovermode all shadow\n"
        "\nResult: { changed, header_admit, validate_headers, "
        "canary_target_height? }");

    const struct json_value *stage_v = json_at(params, 0);
    const struct json_value *mode_v = json_at(params, 1);
    const char *stage = stage_v ? json_get_str(stage_v) : NULL;
    const char *mode_s = mode_v ? json_get_str(mode_v) : NULL;
    if (!stage || !stage[0]) {
        push_cutover_modes(result, false, NULL);
        return true;
    }
    if (!mode_s || !mode_s[0])
        LOG_FAIL("diag", "cutovermode: missing mode for stage '%s'", stage);

    header_admit_mode_t ha_mode = HEADER_ADMIT_MODE_SHADOW;
    validate_headers_mode_t vh_mode = VALIDATE_HEADERS_MODE_SHADOW;
    if (!parse_cutover_mode(mode_s, &ha_mode, &vh_mode))
        LOG_FAIL("diag", "cutovermode: invalid mode '%s'", mode_s);

    if (strcasecmp(stage, "header_admit") != 0 &&
        strcasecmp(stage, "validate_headers") != 0 &&
        strcasecmp(stage, "all") != 0)
        LOG_FAIL("diag", "cutovermode: invalid stage '%s'", stage);

    bool wants_authoritative =
        cutover_stage_requests_authoritative(stage, ha_mode, vh_mode);
    if (wants_authoritative &&
        !cutover_authoritative_request_is_paired(stage))
        LOG_FAIL("diag",
                 "cutovermode: authoritative flip must use stage=all");

    if (wants_authoritative &&
        !cutover_preflight_ready_now())
        LOG_FAIL("diag",
                 "cutovermode: authoritative flip refused; "
                 "cutoverpreflight.ready is false");

    if (strcasecmp(stage, "header_admit") == 0) {
        header_admit_set_mode(ha_mode);
    } else if (strcasecmp(stage, "validate_headers") == 0) {
        validate_headers_set_mode(vh_mode);
    } else if (strcasecmp(stage, "all") == 0) {
        cutover_modes_set_header_pipeline(
            cutover_stage_mode_from_header_admit(ha_mode),
            cutover_stage_mode_from_validate_headers(vh_mode));
    }

    struct node_health_snapshot health;
    node_health_collect(&health, NULL, NULL);
    cutover_record_mode_change(&health);
    push_cutover_modes(result, true, &health);
    return true;
}

/* ── RPC: cutoverpreflight [start_height] [end_height] ────────────── */

static const char *
header_admit_diff_status_rpc_name(enum header_admit_diff_status s)
{
    switch (s) {
    case HEADER_ADMIT_DIFF_CONVERGED:   return "CONVERGED";
    case HEADER_ADMIT_DIFF_DIVERGENT:   return "DIVERGENT";
    case HEADER_ADMIT_DIFF_LOG_AHEAD:   return "LOG_AHEAD";
    case HEADER_ADMIT_DIFF_CHAIN_AHEAD: return "CHAIN_AHEAD";
    case HEADER_ADMIT_DIFF_EMPTY:       return "EMPTY";
    case HEADER_ADMIT_DIFF_NOT_READY:   return "NOT_READY";
    }
    return "UNKNOWN";
}

static int64_t json_obj_int_or(const struct json_value *obj,
                               const char *key,
                               int64_t fallback)
{
    const struct json_value *v = json_get(obj, key);
    return v ? json_get_int(v) : fallback;
}

static void push_validate_headers_window_json(
    struct json_value *vh,
    const struct validate_headers_window_report *r)
{
    if (!vh || !r) return;
    json_push_kv_bool(vh, "window_available", r->available);
    json_push_kv_bool(vh, "window_complete", r->complete);
    json_push_kv_int(vh, "window_start_height", r->start_height);
    json_push_kv_int(vh, "window_end_height", r->end_height);
    json_push_kv_int(vh, "window_expected_count", r->expected_count);
    json_push_kv_int(vh, "window_checked_count", r->checked_count);
    json_push_kv_int(vh, "window_failed_count", r->failed_count);
    json_push_kv_int(vh, "window_first_failed_height",
                     r->first_failed_height);
    json_push_kv_str(vh, "window_first_fail_reason",
                     r->first_fail_reason);
}

static void cutover_preflight_push_blocker(struct json_value *blockers,
                                           const char *reason)
{
    struct json_value v;
    json_init(&v);
    json_set_str(&v, reason);
    json_push_back(blockers, &v);
    json_free(&v);
}

static bool cutover_preflight_tail_window(
    int64_t start_i,
    int64_t end_i,
    const struct header_admit_diff_report *rep,
    int32_t *start_out,
    int32_t *end_out)
{
    if (start_i != -1 || end_i != -1 || !rep || !start_out || !end_out)
        return false;
    if (rep->log_max_height < 0 || rep->chain_tip_height < 0)
        return false;

    int32_t end = rep->log_max_height < rep->chain_tip_height
        ? rep->log_max_height : rep->chain_tip_height;
    if (end < 0)
        return false;

    int32_t start = 0;
    if (end >= HEADER_ADMIT_DIFF_MAX_RANGE)
        start = end - HEADER_ADMIT_DIFF_MAX_RANGE + 1;
    if (start == rep->start_height && end == rep->end_height)
        return false;

    *start_out = start;
    *end_out = end;
    return true;
}

static bool cutover_preflight_operator_needed_blocks(
    const struct node_health_snapshot *health)
{
    if (!health || !health->operator_needed)
        return false;
    return strstr(health->operator_needed_detail,
                  "peer_floor_violated") == NULL;
}

static bool push_cutover_live_gate_json(struct json_value *live,
                                        struct node_health_snapshot *out)
{
    struct node_health_snapshot health;
    node_health_collect(&health, NULL, NULL);
    if (out)
        *out = health;

    bool tip_recent =
        health.tip_advance_age_seconds >= 0 &&
        health.tip_advance_age_seconds <=
            CUTOVER_PREFLIGHT_MAX_TIP_ADVANCE_AGE_SECS;
    bool headers_in_range =
        health.header_height <= health.tip_height + 1;
    bool operator_needed_blocks =
        cutover_preflight_operator_needed_blocks(&health);
    bool cutover_ready =
        health.synced &&
        health.has_peers &&
        health.tip_lag == 0 &&
        tip_recent &&
        headers_in_range &&
        !health.tip_stale &&
        !operator_needed_blocks &&
        strcmp(health.mirror_lag_breach_severity, "fatal") != 0;

    json_set_object(live);
    json_push_kv_bool(live, "healthy", health.healthy);
    json_push_kv_bool(live, "cutover_ready", cutover_ready);
    json_push_kv_bool(live, "synced", health.synced);
    json_push_kv_bool(live, "has_peers", health.has_peers);
    json_push_kv_bool(live, "tip_recent", tip_recent);
    json_push_kv_bool(live, "headers_in_range", headers_in_range);
    json_push_kv_bool(live, "operator_needed_blocks_cutover",
                      operator_needed_blocks);
    json_push_kv_bool(live, "operator_needed", health.operator_needed);
    json_push_kv_str(live, "operator_needed_detail",
                     health.operator_needed_detail);
    json_push_kv_int(live, "peer_count", (int64_t)health.peer_count);
    json_push_kv_int(live, "tip_height", health.tip_height);
    json_push_kv_int(live, "canary_target_height",
                     health.tip_height >= 0 ? health.tip_height + 1 : 0);
    json_push_kv_int(live, "header_height", health.header_height);
    json_push_kv_int(live, "peer_best_height", health.peer_best_height);
    json_push_kv_int(live, "tip_lag", health.tip_lag);
    json_push_kv_int(live, "tip_advance_age_seconds",
                     health.tip_advance_age_seconds);
    json_push_kv_str(live, "degraded_reason", health.degraded_reason);
    json_push_kv_str(live, "mirror_lag_breach_severity",
                     health.mirror_lag_breach_severity);

    return cutover_ready;
}

static bool push_cutover_chain_advance_gate_json(struct json_value *out)
{
    struct cac_decision d;
    chain_advance_coordinator_get_status(&d);

    bool source_ready = false;
    if (d.selected_source > CAC_SOURCE_NONE &&
        d.selected_source < CAC_SOURCE_NUM) {
        const struct cac_source_status *s = &d.sources[d.selected_source];
        source_ready =
            s->available && s->healthy && s->selectable && !s->blocked &&
            s->selection_blocker[0] == '\0';
    }
    bool ready = node_health_chain_advance_synced(&d);
    bool projection_ready = d.projection_lag >= 0 && d.projection_lag <= 1;
    const char *not_ready_reason = "";
    int64_t target_gap = 0;
    if (d.local_height >= 0 && d.target_height >= 0 &&
        d.target_height > d.local_height)
        target_gap = (int64_t)d.target_height - d.local_height;
    if (!ready) {
        if (d.result != CAC_DECISION_USE_SOURCE)
            not_ready_reason = "decision_not_use_source";
        else if (d.selected_source <= CAC_SOURCE_NONE ||
                 d.selected_source >= CAC_SOURCE_NUM)
            not_ready_reason = "selected_source_invalid";
        else if (d.blocker[0] != '\0')
            not_ready_reason = "blocker_present";
        else if (d.local_height < 0 || d.target_height < 0)
            not_ready_reason = "invalid_heights";
        else if (d.local_height + 1 < d.target_height)
            not_ready_reason = "target_height_gap";
        else if (!source_ready)
            not_ready_reason = "source_not_ready";
        else
            not_ready_reason = "unknown";
    }

    json_set_object(out);
    json_push_kv_bool(out, "ready", ready);
    json_push_kv_str(out, "not_ready_reason", not_ready_reason);
    json_push_kv_str(out, "decision",
                     cac_decision_result_name(d.result));
    json_push_kv_str(out, "selected_source",
                     cac_source_name(d.selected_source));
    json_push_kv_str(out, "selected_source_trust",
                     cac_source_trust_name(d.selected_source));
    json_push_kv_str(out, "authority", "local_consensus_validation");
    json_push_kv_bool(out, "source_ready", source_ready);
    json_push_kv_bool(out, "activation_allowed", d.activation_allowed);
    json_push_kv_int(out, "local_height", (int64_t)d.local_height);
    json_push_kv_int(out, "target_height", (int64_t)d.target_height);
    json_push_kv_int(out, "target_gap", target_gap);
    json_push_kv_int(out, "best_header_height",
                     (int64_t)d.best_header_height);
    json_push_kv_int(out, "projection_height",
                     (int64_t)d.projection_height);
    json_push_kv_int(out, "projection_lag", d.projection_lag);
    json_push_kv_bool(out, "projection_ready", projection_ready);
    json_push_kv_str(out, "projection_gate", "diagnostic_only");
    json_push_kv_bool(out, "projection_deferred",
                      d.projection_deferred);
    json_push_kv_str(out, "projection_state", d.projection_state);
    json_push_kv_str(out, "reason", d.reason);
    json_push_kv_str(out, "blocker", d.blocker);
    if (d.selected_source > CAC_SOURCE_NONE &&
        d.selected_source < CAC_SOURCE_NUM) {
        const struct cac_source_status *s = &d.sources[d.selected_source];
        json_push_kv_str(out, "selected_source_state", s->state);
        json_push_kv_str(out, "selected_source_reason", s->reason);
        json_push_kv_str(out, "selected_source_blocker", s->blocker);
        json_push_kv_str(out, "selected_source_selection_blocker",
                         s->selection_blocker);
        json_push_kv_bool(out, "selected_source_available",
                          s->available);
        json_push_kv_bool(out, "selected_source_healthy", s->healthy);
        json_push_kv_bool(out, "selected_source_selectable",
                          s->selectable);
        json_push_kv_bool(out, "selected_source_blocked", s->blocked);
        json_push_kv_int(out, "selected_source_height",
                         (int64_t)s->height);
    }
    return ready;
}

static bool push_cutover_guard_gate_json(struct json_value *guard)
{
    struct condition_runtime_snapshot snap;
    bool registered = condition_engine_get_registered_snapshot(
        CUTOVER_PREFLIGHT_GUARD_NAME, &snap);

    json_set_object(guard);
    json_push_kv_str(guard, "name", CUTOVER_PREFLIGHT_GUARD_NAME);
    json_push_kv_bool(guard, "registered", registered);
    json_push_kv_int(guard, "max_tip_advance_age_seconds",
                     CUTOVER_PREFLIGHT_MAX_TIP_ADVANCE_AGE_SECS);
    json_push_kv_int(guard, "max_poll_secs",
                     CUTOVER_PREFLIGHT_MAX_GUARD_POLL_SECS);
    json_push_kv_int(guard, "max_witness_window_secs",
                     CUTOVER_PREFLIGHT_MAX_GUARD_WITNESS_SECS);
    if (!registered) {
        json_push_kv_bool(guard, "ready", false);
        json_push_kv_str(guard, "severity", "unknown");
        json_push_kv_bool(guard, "config_ready", false);
        json_push_kv_bool(guard, "state_ready", false);
        json_push_kv_bool(guard, "currently_active", false);
        json_push_kv_bool(guard, "operator_needed_emitted", false);
        json_push_kv_int(guard, "attempts", 0);
        json_push_kv_str(guard, "last_outcome", "unknown");
        json_push_kv_int(guard, "cleared_count", 0);
        json_push_kv_int(guard, "poll_secs", 0);
        json_push_kv_int(guard, "backoff_secs", 0);
        json_push_kv_int(guard, "max_attempts", 0);
        json_push_kv_int(guard, "witness_window_secs", 0);
        return false;
    }

    bool config_ready =
        snap.severity == COND_CRITICAL &&
        snap.poll_secs > 0 &&
        snap.poll_secs <= CUTOVER_PREFLIGHT_MAX_GUARD_POLL_SECS &&
        snap.max_attempts == 1 &&
        snap.witness_window_secs > 0 &&
        snap.witness_window_secs <=
            CUTOVER_PREFLIGHT_MAX_GUARD_WITNESS_SECS;
    bool state_ready =
        !snap.currently_active &&
        !snap.operator_needed_emitted &&
        snap.attempts == 0 &&
        snap.last_outcome != COND_REMEDY_UNWITNESSED;

    json_push_kv_bool(guard, "ready", config_ready && state_ready);
    json_push_kv_str(guard, "severity",
                     condition_severity_name(snap.severity));
    json_push_kv_bool(guard, "config_ready", config_ready);
    json_push_kv_bool(guard, "state_ready", state_ready);
    json_push_kv_bool(guard, "currently_active",
                      snap.currently_active);
    json_push_kv_bool(guard, "operator_needed_emitted",
                      snap.operator_needed_emitted);
    json_push_kv_int(guard, "attempts", snap.attempts);
    json_push_kv_str(guard, "last_outcome",
                     condition_remedy_result_name(snap.last_outcome));
    json_push_kv_int(guard, "cleared_count", snap.cleared_count);
    json_push_kv_int(guard, "poll_secs", snap.poll_secs);
    json_push_kv_int(guard, "backoff_secs", snap.backoff_secs);
    json_push_kv_int(guard, "max_attempts", snap.max_attempts);
    json_push_kv_int(guard, "witness_window_secs",
                     snap.witness_window_secs);
    return config_ready && state_ready;
}

bool diag_rpc_cutoverpreflight(const struct json_value *params, bool help,
                               struct json_value *result)
{
    RPC_HELP(help, result,
        "cutoverpreflight [start_height] [end_height]\n"
        "\nRead-only C-3 preflight snapshot: runtime cutover modes, "
        "cutover-specific live progress, chain-advance source selection, "
        "header_admit shadow-vs-active-chain diff, validate_headers "
        "persisted window/cursor coverage, and a conservative ready boolean "
        "gated by the cutover no-progress guard.\n"
        "\nHeights default to the most recent header_admit diff window. "
        "Result: { ready, blockers, live, chain_advance, chain_evidence, "
        "guard, modes, header_admit_diff, validate_headers }");

    const struct json_value *start_v = json_at(params, 0);
    const struct json_value *end_v = json_at(params, 1);
    int64_t start_i = start_v ? json_get_int(start_v) : -1;
    int64_t end_i = end_v ? json_get_int(end_v) : -1;
    if (start_i < -1) start_i = -1;
    if (end_i < -1) end_i = -1;
    if (start_i > INT32_MAX) start_i = INT32_MAX;
    if (end_i > INT32_MAX) end_i = INT32_MAX;

    struct header_admit_diff_report rep;
    if (!header_admit_stage_diff((int32_t)start_i, (int32_t)end_i, &rep))
        LOG_FAIL("diag", "cutoverpreflight: header_admit diff failed");
    int32_t tail_start = 0;
    int32_t tail_end = 0;
    if (cutover_preflight_tail_window(start_i, end_i, &rep,
                                      &tail_start, &tail_end) &&
        !header_admit_stage_diff(tail_start, tail_end, &rep))
        LOG_FAIL("diag", "cutoverpreflight: header_admit tail diff failed");

    struct json_value modes;
    struct json_value live;
    struct json_value chain_advance;
    struct json_value chain_evidence;
    struct json_value guard;
    struct json_value canary;
    struct json_value diff;
    struct json_value vh;
    struct json_value blockers;
    json_init(&modes);
    json_init(&live);
    json_init(&chain_advance);
    json_init(&chain_evidence);
    json_init(&guard);
    json_init(&canary);
    json_init(&diff);
    json_init(&vh);
    json_init(&blockers);
    json_set_object(result);
    json_set_object(&modes);
    json_set_object(&diff);
    json_set_array(&blockers);

    bool vh_ok = validate_headers_stage_dump_state_json(&vh, NULL);
    if (!vh_ok)
        json_set_object(&vh);
    bool ce_ok =
        diag_chain_evidence_dump_state_json(&chain_evidence, NULL);
    if (!ce_ok)
        json_set_object(&chain_evidence);

    const char *ha_mode = header_admit_mode_name(header_admit_get_mode());
    const char *vh_mode = validate_headers_mode_name(validate_headers_get_mode());
    json_push_kv_str(&modes, "header_admit", ha_mode);
    json_push_kv_str(&modes, "validate_headers", vh_mode);
    struct node_health_snapshot live_health;
    bool live_ready = push_cutover_live_gate_json(&live, &live_health);
    bool chain_advance_ready =
        push_cutover_chain_advance_gate_json(&chain_advance);
    push_cutover_canary_state(&canary, &live_health);
    bool guard_ready = push_cutover_guard_gate_json(&guard);

    json_push_kv_str(&diff, "status",
                     header_admit_diff_status_rpc_name(rep.status));
    json_push_kv_int(&diff, "start_height", rep.start_height);
    json_push_kv_int(&diff, "end_height", rep.end_height);
    json_push_kv_int(&diff, "checked_count", rep.checked_count);
    json_push_kv_int(&diff, "match_count", rep.match_count);
    json_push_kv_int(&diff, "mismatch_count", rep.mismatch_count);
    json_push_kv_int(&diff, "missing_in_log_count",
                     rep.missing_in_log_count);
    json_push_kv_int(&diff, "missing_in_chain_count",
                     rep.missing_in_chain_count);
    json_push_kv_int(&diff, "first_divergent_height",
                     rep.first_divergent_height);
    json_push_kv_int(&diff, "log_max_height", rep.log_max_height);
    json_push_kv_int(&diff, "chain_tip_height", rep.chain_tip_height);
    json_push_kv_int(&diff, "cursor", rep.cursor);
    int64_t ha_persisted_cursor = (int64_t)header_admit_stage_cursor();
    json_push_kv_int(&diff, "persisted_cursor", ha_persisted_cursor);
    int64_t required_ha_cursor =
        (rep.chain_tip_height >= 0) ? ((int64_t)rep.chain_tip_height + 1) : 0;
    int64_t ha_cursor_lag =
        (ha_persisted_cursor >= 0 && ha_persisted_cursor < required_ha_cursor)
            ? (required_ha_cursor - ha_persisted_cursor) : 0;
    int64_t ha_log_tip_lag =
        (rep.log_max_height >= 0 && rep.log_max_height < rep.chain_tip_height)
            ? ((int64_t)rep.chain_tip_height - rep.log_max_height) : 0;
    json_push_kv_int(&diff, "required_cursor", required_ha_cursor);
    json_push_kv_int(&diff, "cursor_lag", ha_cursor_lag);
    json_push_kv_int(&diff, "log_tip_lag", ha_log_tip_lag);

    bool header_caught_up =
        required_ha_cursor > 0 &&
        ha_persisted_cursor >= required_ha_cursor &&
        ha_cursor_lag == 0 &&
        ha_log_tip_lag == 0;
    bool header_ready =
        rep.status == HEADER_ADMIT_DIFF_CONVERGED &&
        rep.mismatch_count == 0 &&
        rep.missing_in_chain_count == 0 &&
        header_caught_up;
    int64_t vh_cursor = (int64_t)validate_headers_stage_cursor();
    json_push_kv_int(&vh, "persisted_cursor", vh_cursor);
    int64_t required_vh_cursor =
        (rep.end_height >= 0) ? ((int64_t)rep.end_height + 1) : 0;
    int64_t vh_cursor_lag =
        (vh_cursor >= 0 && vh_cursor < required_vh_cursor)
            ? (required_vh_cursor - vh_cursor) : 0;
    json_push_kv_int(&vh, "required_cursor", required_vh_cursor);
    json_push_kv_int(&vh, "cursor_lag", vh_cursor_lag);

    struct validate_headers_window_report vh_window;
    int64_t vh_window_start = rep.start_height;
    int64_t vh_window_end = rep.end_height;
    validate_headers_stage_window_report(vh_window_start, vh_window_end,
                                         &vh_window);
    push_validate_headers_window_json(&vh, &vh_window);

    bool validate_no_failures =
        json_obj_int_or(&vh, "failed_total", 1) == 0 &&
        json_obj_int_or(&vh, "failure_log_count", 1) == 0;
    json_push_kv_bool(&vh, "no_failures", validate_no_failures);
    bool validate_clean = vh_ok &&
        validate_no_failures &&
        json_obj_int_or(&vh, "error_count", 1) == 0 &&
        vh_window.available &&
        vh_window.complete &&
        vh_window.failed_count == 0;
    bool validate_caught_up =
        required_vh_cursor > 0 &&
        vh_cursor >= required_vh_cursor &&
        vh_cursor_lag == 0;
    bool validate_ready = validate_clean && validate_caught_up;
    bool modes_ready =
        strcmp(ha_mode, "shadow") == 0 &&
        strcmp(vh_mode, "shadow") == 0;

    if (!live_ready)
        cutover_preflight_push_blocker(&blockers, "live_health_not_ready");
    if (!chain_advance_ready)
        cutover_preflight_push_blocker(&blockers,
                                       "chain_advance_not_ready");
    if (!guard_ready)
        cutover_preflight_push_blocker(&blockers,
                                       condition_engine_has_registered(
                                           CUTOVER_PREFLIGHT_GUARD_NAME)
                                           ? "cutover_guard_not_ready"
                                           : "cutover_guard_not_registered");
    if (!header_ready)
        cutover_preflight_push_blocker(
            &blockers,
            header_caught_up ? "header_admit_diff_not_converged"
                             : "header_admit_cursor_lag");
    if (!validate_ready)
        cutover_preflight_push_blocker(
            &blockers,
            !validate_no_failures ? "validate_headers_failures_present" :
            validate_clean ? "validate_headers_cursor_lag" :
                             "validate_headers_window_not_clean");
    if (!modes_ready)
        cutover_preflight_push_blocker(&blockers,
                                       "cutover_modes_not_shadow");

    json_push_kv_bool(result, "ready",
                      live_ready && chain_advance_ready && guard_ready &&
                      header_ready && validate_ready && modes_ready);
    json_push_kv(result, "blockers", &blockers);
    json_push_kv(result, "live", &live);
    json_push_kv(result, "chain_advance", &chain_advance);
    json_push_kv(result, "chain_evidence", &chain_evidence);
    json_push_kv(result, "guard", &guard);
    json_push_kv(result, "cutover_state", &canary);
    json_push_kv(result, "modes", &modes);
    json_push_kv(result, "header_admit_diff", &diff);
    json_push_kv(result, "validate_headers", &vh);

    json_free(&blockers);
    json_free(&modes);
    json_free(&live);
    json_free(&chain_advance);
    json_free(&chain_evidence);
    json_free(&guard);
    json_free(&canary);
    json_free(&diff);
    json_free(&vh);
    return true;
}
