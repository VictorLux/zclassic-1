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
#include "services/cutover_modes.h"
#include "jobs/header_admit_stage.h"
#include "jobs/validate_headers_stage.h"
#include "jobs/tip_finalize_stage.h"
#include "services/node_health_service.h"
#include "storage/utxo_projection.h"
#include "util/log_macros.h"

#include <stdint.h>
#include <string.h>
#include <strings.h>

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

static const char *tip_finalize_mode_name(tip_finalize_mode_t mode)
{
    return mode == TIP_FINALIZE_MODE_AUTHORITATIVE
        ? "authoritative" : "shadow";
}

static bool parse_cutover_mode(const char *s,
                               header_admit_mode_t *ha,
                               validate_headers_mode_t *vh,
                               tip_finalize_mode_t *tf)
{
    if (!s || !s[0]) return false;
    if (strcasecmp(s, "shadow") == 0) {
        if (ha) *ha = HEADER_ADMIT_MODE_SHADOW;
        if (vh) *vh = VALIDATE_HEADERS_MODE_SHADOW;
        if (tf) *tf = TIP_FINALIZE_MODE_SHADOW;
        return true;
    }
    if (strcasecmp(s, "authoritative") == 0 ||
        strcasecmp(s, "auth") == 0) {
        if (ha) *ha = HEADER_ADMIT_MODE_AUTHORITATIVE;
        if (vh) *vh = VALIDATE_HEADERS_MODE_AUTHORITATIVE;
        if (tf) *tf = TIP_FINALIZE_MODE_AUTHORITATIVE;
        return true;
    }
    return false;
}

static void cutover_record_mode_change(
    const struct node_health_snapshot *health)
{
    cutover_modes_record_change(health ? health->tip_height : -1,
                                health ? health->header_height : -1,
                                health ? health->peer_best_height : -1,
                                health ? health->tip_lag : -1);
}

void cutover_push_canary_state(
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
    json_push_kv_str(result, "tip_finalize",
                     tip_finalize_mode_name(tip_finalize_get_mode()));
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
    cutover_push_canary_state(&canary, health);
    json_push_kv(result, "cutover_state", &canary);
    json_free(&canary);
}

static bool cutover_stage_requests_authoritative(
    const char *stage,
    header_admit_mode_t ha_mode,
    validate_headers_mode_t vh_mode,
    tip_finalize_mode_t tf_mode)
{
    if (strcasecmp(stage, "header_admit") == 0)
        return ha_mode == HEADER_ADMIT_MODE_AUTHORITATIVE;
    if (strcasecmp(stage, "validate_headers") == 0)
        return vh_mode == VALIDATE_HEADERS_MODE_AUTHORITATIVE;
    if (strcasecmp(stage, "tip_finalize") == 0)
        return tf_mode == TIP_FINALIZE_MODE_AUTHORITATIVE;
    if (strcasecmp(stage, "all") == 0)
        return ha_mode == HEADER_ADMIT_MODE_AUTHORITATIVE ||
               vh_mode == VALIDATE_HEADERS_MODE_AUTHORITATIVE ||
               tf_mode == TIP_FINALIZE_MODE_AUTHORITATIVE;
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
        "  header_admit | validate_headers | tip_finalize | all\n"
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
    tip_finalize_mode_t tf_mode = TIP_FINALIZE_MODE_SHADOW;
    if (!parse_cutover_mode(mode_s, &ha_mode, &vh_mode, &tf_mode))
        LOG_FAIL("diag", "cutovermode: invalid mode '%s'", mode_s);

    if (strcasecmp(stage, "header_admit") != 0 &&
        strcasecmp(stage, "validate_headers") != 0 &&
        strcasecmp(stage, "tip_finalize") != 0 &&
        strcasecmp(stage, "all") != 0)
        LOG_FAIL("diag", "cutovermode: invalid stage '%s'", stage);

    bool wants_authoritative =
        cutover_stage_requests_authoritative(stage, ha_mode, vh_mode, tf_mode);
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
    } else if (strcasecmp(stage, "tip_finalize") == 0) {
        tip_finalize_set_mode(tf_mode);
    } else if (strcasecmp(stage, "all") == 0) {
        header_admit_set_mode(ha_mode);
        validate_headers_set_mode(vh_mode);
        tip_finalize_set_mode(tf_mode);
        /* B5 keystone: the UTXO projection author flips ATOMICALLY with the
         * stage=all authority flip. Going authoritative makes
         * utxo_apply_stage the single writer (forward delta emit AND the
         * stage-side reorg-unwind inverse path), while silencing the legacy
         * update_coins / disconnect_block emitters — which is precisely why
         * the stage-side inverse path (utxo_apply_reorg_unwind_if_needed)
         * must exist before this line ever fires. Flipping back to shadow
         * restores LEGACY authority symmetrically. This is reachable ONLY
         * after the cutover_preflight_ready_now() gate above; the
         * COMPILE-TIME DEFAULT stays UTXO_AUTHOR_LEGACY so a bad flip is one
         * RPC to revert, never a rebuild. */
        utxo_projection_set_author(  // one-write-path-ok:cutover-author-flip
            tf_mode == TIP_FINALIZE_MODE_AUTHORITATIVE
                ? UTXO_AUTHOR_STAGE : UTXO_AUTHOR_LEGACY);
    }

    struct node_health_snapshot health;
    node_health_collect(&health, NULL, NULL);
    cutover_record_mode_change(&health);
    push_cutover_modes(result, true, &health);
    return true;
}
