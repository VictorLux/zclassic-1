/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "services/cutover_modes.h"

#include "platform/time_compat.h"
#include "json/json.h"

#include <stdatomic.h>
#include <string.h>

#define CUTOVER_MODE_HEADER_ADMIT      0x01u
#define CUTOVER_MODE_VALIDATE_HEADERS  0x02u
#define CUTOVER_MODE_TIP_FINALIZE      0x04u

/* CUTOVER FLIP (step 13): the staged header pipeline is now the AUTHORITATIVE
 * default. All three stages (header_admit, validate_headers, tip_finalize)
 * default to authoritative; the legacy engine is no longer the default writer.
 * `cutover_modes_revert_all_to_shadow()` / `cutover_modes_test_reset()` still
 * fall back to 0 (full SHADOW) for an explicit clean-slate baseline. */
static _Atomic unsigned g_header_pipeline_modes =
    (CUTOVER_MODE_HEADER_ADMIT | CUTOVER_MODE_VALIDATE_HEADERS |
     CUTOVER_MODE_TIP_FINALIZE);
static _Atomic int g_cutover_has_change;
static _Atomic int64_t g_cutover_change_unix;
static _Atomic int64_t g_cutover_change_height;
static _Atomic int64_t g_cutover_canary_target_height;
static _Atomic int64_t g_cutover_change_header_height;
static _Atomic int64_t g_cutover_change_peer_best_height;
static _Atomic int64_t g_cutover_change_tip_lag;

static unsigned bit_for_mode(unsigned bit, cutover_stage_mode_t mode)
{
    return mode == CUTOVER_STAGE_MODE_AUTHORITATIVE ? bit : 0u;
}

static void cutover_modes_update_one(unsigned bit, cutover_stage_mode_t mode)
{
    unsigned old_bits = atomic_load(&g_header_pipeline_modes);
    unsigned new_bits;
    do {
        new_bits = old_bits & ~bit;
        new_bits |= bit_for_mode(bit, mode);
    } while (!atomic_compare_exchange_weak(&g_header_pipeline_modes,
                                           &old_bits,
                                           new_bits));
}

void cutover_modes_set_header_admit(cutover_stage_mode_t mode)
{
    cutover_modes_update_one(CUTOVER_MODE_HEADER_ADMIT, mode);
}

void cutover_modes_set_validate_headers(cutover_stage_mode_t mode)
{
    cutover_modes_update_one(CUTOVER_MODE_VALIDATE_HEADERS, mode);
}

void cutover_modes_set_tip_finalize(cutover_stage_mode_t mode)
{
    cutover_modes_update_one(CUTOVER_MODE_TIP_FINALIZE, mode);
}

void cutover_modes_set_header_pipeline(cutover_stage_mode_t header_admit,
                                       cutover_stage_mode_t validate_headers)
{
    unsigned bits = atomic_load(&g_header_pipeline_modes);
    bits &= ~(CUTOVER_MODE_HEADER_ADMIT | CUTOVER_MODE_VALIDATE_HEADERS);
    bits |= bit_for_mode(CUTOVER_MODE_HEADER_ADMIT, header_admit);
    bits |= bit_for_mode(CUTOVER_MODE_VALIDATE_HEADERS, validate_headers);
    atomic_store(&g_header_pipeline_modes, bits);
}

void cutover_modes_revert_all_to_shadow(void)
{
    atomic_store(&g_header_pipeline_modes, 0u);
}

cutover_stage_mode_t cutover_modes_get_header_admit(void)
{
    return (atomic_load(&g_header_pipeline_modes) &
            CUTOVER_MODE_HEADER_ADMIT)
        ? CUTOVER_STAGE_MODE_AUTHORITATIVE
        : CUTOVER_STAGE_MODE_SHADOW;
}

cutover_stage_mode_t cutover_modes_get_validate_headers(void)
{
    return (atomic_load(&g_header_pipeline_modes) &
            CUTOVER_MODE_VALIDATE_HEADERS)
        ? CUTOVER_STAGE_MODE_AUTHORITATIVE
        : CUTOVER_STAGE_MODE_SHADOW;
}

cutover_stage_mode_t cutover_modes_get_tip_finalize(void)
{
    return (atomic_load(&g_header_pipeline_modes) &
            CUTOVER_MODE_TIP_FINALIZE)
        ? CUTOVER_STAGE_MODE_AUTHORITATIVE
        : CUTOVER_STAGE_MODE_SHADOW;
}

bool cutover_modes_any_authoritative_active(void)
{
    return atomic_load(&g_header_pipeline_modes) != 0;
}

void cutover_modes_record_change(int64_t height,
                                 int64_t header_height,
                                 int64_t peer_best_height,
                                 int64_t tip_lag)
{
    atomic_store(&g_cutover_has_change, 1);
    atomic_store(&g_cutover_change_unix, platform_time_wall_unix());
    atomic_store(&g_cutover_change_height, height);
    atomic_store(&g_cutover_canary_target_height,
                 height >= 0 ? height + 1 : 0);
    atomic_store(&g_cutover_change_header_height, header_height);
    atomic_store(&g_cutover_change_peer_best_height, peer_best_height);
    atomic_store(&g_cutover_change_tip_lag, tip_lag);
}

void cutover_modes_canary_snapshot(int64_t current_tip_height,
                                   struct cutover_canary_snapshot *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));

    int64_t changed_at = atomic_load(&g_cutover_change_unix);
    int64_t target = atomic_load(&g_cutover_canary_target_height);
    int64_t now = platform_time_wall_unix();
    bool has_change = atomic_load(&g_cutover_has_change) != 0;
    bool passed = has_change && target > 0 && current_tip_height >= target;
    int64_t deadline = has_change && changed_at > 0
        ? changed_at + CUTOVER_CANARY_WATCH_SECS : 0;

    out->has_change = has_change;
    out->authoritative_active = cutover_modes_any_authoritative_active();
    out->passed = passed;
    out->failed = has_change && !passed && deadline > 0 && now > deadline;
    out->changed_at_unix = changed_at;
    out->change_height = atomic_load(&g_cutover_change_height);
    out->target_height = target;
    out->current_tip_height = current_tip_height;
    out->elapsed_seconds = has_change && changed_at > 0 && now >= changed_at
        ? now - changed_at : -1;
    out->deadline_unix = deadline;
    out->change_header_height = atomic_load(&g_cutover_change_header_height);
    out->change_peer_best_height =
        atomic_load(&g_cutover_change_peer_best_height);
    out->change_tip_lag = atomic_load(&g_cutover_change_tip_lag);
    out->watch_window_seconds = CUTOVER_CANARY_WATCH_SECS;
}

bool cutover_modes_canary_target_reached(int64_t current_tip_height)
{
    struct cutover_canary_snapshot snap;
    cutover_modes_canary_snapshot(current_tip_height, &snap);
    return snap.has_change && snap.authoritative_active && snap.passed;
}

void cutover_modes_clear_canary(void)
{
    atomic_store(&g_cutover_has_change, 0);
}

static const char *cutover_mode_name(cutover_stage_mode_t mode)
{
    return mode == CUTOVER_STAGE_MODE_AUTHORITATIVE
        ? "authoritative" : "shadow";
}

/* zcl_state subsystem=cutover. Reentrant-safe, non-allocating, READ-ONLY:
 * every read here is a relaxed atomic load (modes + canary anchor) or a
 * best-effort observe-only counter snapshot (conservation) — it never
 * mutates cutover state, never triggers a flip, and never touches the live
 * tip / runs the header diff. The tip-relative canary verdict and the full
 * `ready` gate breakdown live on the heavier `cutoverpreflight` RPC. */
bool cutover_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out)
        return false;

    cutover_stage_mode_t ha = cutover_modes_get_header_admit();
    cutover_stage_mode_t vh = cutover_modes_get_validate_headers();
    cutover_stage_mode_t tf = cutover_modes_get_tip_finalize();
    bool authoritative_active = cutover_modes_any_authoritative_active();

    /* Per-stage runtime modes + the aggregate authoritative flag. */
    struct json_value modes;
    json_init(&modes);
    json_set_object(&modes);
    json_push_kv_str(&modes, "header_admit", cutover_mode_name(ha));
    json_push_kv_str(&modes, "validate_headers", cutover_mode_name(vh));
    json_push_kv_str(&modes, "tip_finalize", cutover_mode_name(tf));
    json_push_kv(out, "modes", &modes);
    json_free(&modes);
    json_push_kv_bool(out, "authoritative_active", authoritative_active);

    /* Recorded canary change anchor — atomic loads only, no live tip. The
     * tip-relative passed/failed verdict needs the current tip height and
     * stays on cutoverpreflight (cutover_state). */
    bool has_change = atomic_load(&g_cutover_has_change) != 0;
    int64_t changed_at = atomic_load(&g_cutover_change_unix);
    int64_t deadline = has_change && changed_at > 0
        ? changed_at + CUTOVER_CANARY_WATCH_SECS : 0;
    struct json_value canary;
    json_init(&canary);
    json_set_object(&canary);
    json_push_kv_bool(&canary, "has_change", has_change);
    json_push_kv_int(&canary, "changed_at_unix", changed_at);
    json_push_kv_int(&canary, "change_height",
                     atomic_load(&g_cutover_change_height));
    json_push_kv_int(&canary, "target_height",
                     atomic_load(&g_cutover_canary_target_height));
    json_push_kv_int(&canary, "change_header_height",
                     atomic_load(&g_cutover_change_header_height));
    json_push_kv_int(&canary, "change_peer_best_height",
                     atomic_load(&g_cutover_change_peer_best_height));
    json_push_kv_int(&canary, "change_tip_lag",
                     atomic_load(&g_cutover_change_tip_lag));
    json_push_kv_int(&canary, "deadline_unix", deadline);
    json_push_kv_int(&canary, "watch_window_seconds",
                     CUTOVER_CANARY_WATCH_SECS);
    json_push_kv(out, "canary", &canary);
    json_free(&canary);

    json_push_kv_str(out, "ready_gate",
                     "see cutoverpreflight RPC for full ready breakdown");
    return true;
}

#ifdef ZCL_TESTING
void cutover_modes_test_reset(void)
{
    atomic_store(&g_header_pipeline_modes, 0);
    atomic_store(&g_cutover_has_change, 0);
    atomic_store(&g_cutover_change_unix, 0);
    atomic_store(&g_cutover_change_height, -1);
    atomic_store(&g_cutover_canary_target_height, 0);
    atomic_store(&g_cutover_change_header_height, -1);
    atomic_store(&g_cutover_change_peer_best_height, -1);
    atomic_store(&g_cutover_change_tip_lag, -1);
}
#endif
