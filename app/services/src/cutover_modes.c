/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "services/cutover_modes.h"

#include "platform/time_compat.h"

#include <stdatomic.h>
#include <string.h>

#define CUTOVER_MODE_HEADER_ADMIT      0x01u
#define CUTOVER_MODE_VALIDATE_HEADERS  0x02u

static _Atomic unsigned g_header_pipeline_modes = 0;
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

void cutover_modes_set_header_pipeline(cutover_stage_mode_t header_admit,
                                       cutover_stage_mode_t validate_headers)
{
    unsigned bits = bit_for_mode(CUTOVER_MODE_HEADER_ADMIT, header_admit) |
                    bit_for_mode(CUTOVER_MODE_VALIDATE_HEADERS,
                                 validate_headers);
    atomic_store(&g_header_pipeline_modes, bits);
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
