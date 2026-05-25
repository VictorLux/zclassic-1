/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "conditions/watchdog_dissolve_pr3.h"
#include "framework/condition.h"

#include "event/event.h"
#include "net/connman.h"
#include "services/cutover_modes.h"
#include "services/header_admit_stage.h"
#include "services/sync_monitor.h"
#include "services/validate_headers_stage.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <stdatomic.h>
#include <stdio.h>

#define CUTOVER_WATCH_SECS 180
#define CUTOVER_STAGE_HEADER_ADMIT      0x01
#define CUTOVER_STAGE_VALIDATE_HEADERS  0x02

static _Atomic int g_stage_mask_at_detect;
static _Atomic int g_local_height_at_detect;
static _Atomic int g_peer_max_at_detect;
static _Atomic int64_t g_tip_age_at_detect;

#ifdef ZCL_TESTING
static _Atomic int g_test_remedy_calls;
#endif

static int authoritative_stage_mask(void)
{
    int mask = 0;
    if (header_admit_get_mode() == HEADER_ADMIT_MODE_AUTHORITATIVE)
        mask |= CUTOVER_STAGE_HEADER_ADMIT;
    if (validate_headers_get_mode() == VALIDATE_HEADERS_MODE_AUTHORITATIVE)
        mask |= CUTOVER_STAGE_VALIDATE_HEADERS;
    return mask;
}

static bool detect_cutover_no_forward_progress(void)
{
    int mask = authoritative_stage_mask();
    if (mask == 0)
        return false;

    int64_t age = sync_monitor_tip_advance_age();
    if (age < 0 || age <= CUTOVER_WATCH_SECS)
        return false;

    struct connman *cm = sync_monitor_connman();
    struct main_state *ms = sync_monitor_main_state();
    if (!cm || !ms)
        return false;

    int local = active_chain_height(&ms->chain_active);
    int peer_max = connman_max_peer_height(cm);
    if (local < 0 || peer_max <= local)
        return false;

    atomic_store(&g_stage_mask_at_detect, mask);
    atomic_store(&g_local_height_at_detect, local);
    atomic_store(&g_peer_max_at_detect, peer_max);
    atomic_store(&g_tip_age_at_detect, age);
    return true;
}

static enum condition_remedy_result remedy_cutover_no_forward_progress(void)
{
    int mask = authoritative_stage_mask();
    if (mask == 0)
        return COND_REMEDY_SKIP;

    cutover_modes_set_header_pipeline(CUTOVER_STAGE_MODE_SHADOW,
                                      CUTOVER_STAGE_MODE_SHADOW);

    fprintf(stderr,  // obs-ok:condition-cutover-revert
            "[condition:cutover_no_forward_progress] reverted mask=%d "
            "local=%d peer_max=%d tip_age=%llds\n",
            mask,
            atomic_load(&g_local_height_at_detect),
            atomic_load(&g_peer_max_at_detect),
            (long long)atomic_load(&g_tip_age_at_detect));
    event_emitf(EV_SYNC_STATE_CHANGE, 0,
                "condition CUTOVER_REVERT mask=%d local=%d peer_max=%d age=%lld",
                mask,
                atomic_load(&g_local_height_at_detect),
                atomic_load(&g_peer_max_at_detect),
                (long long)atomic_load(&g_tip_age_at_detect));
    sync_monitor_record_recovery(WATCHDOG_STATE_STUCK,
                                 atomic_load(&g_local_height_at_detect),
                                 atomic_load(&g_peer_max_at_detect),
                                 mask,
                                 "condition:cutover_no_forward_progress");

#ifdef ZCL_TESTING
    atomic_fetch_add(&g_test_remedy_calls, 1);
#endif
    return COND_REMEDY_OK;
}

static bool witness_cutover_no_forward_progress(int64_t target_at_detect)
{
    (void)target_at_detect;
    struct main_state *ms = sync_monitor_main_state();
    if (!ms)
        return false;

    int height_now = active_chain_height(&ms->chain_active);
    int height_at_detect = atomic_load(&g_local_height_at_detect);
    return height_at_detect >= 0 && height_now > height_at_detect;
}

static struct condition c_cutover_no_forward_progress = {
    .name = "cutover_no_forward_progress",
    .severity = COND_CRITICAL,
    .poll_secs = 5,
    .backoff_secs = 60,
    .max_attempts = 1,
    .detect = detect_cutover_no_forward_progress,
    .remedy = remedy_cutover_no_forward_progress,
    .witness = witness_cutover_no_forward_progress,
    .witness_window_secs = 60,
};

void register_cutover_no_forward_progress(void)
{
    (void)condition_register(&c_cutover_no_forward_progress);
}

#ifdef ZCL_TESTING
void cutover_no_forward_progress_test_reset(void)
{
    atomic_store(&g_stage_mask_at_detect, 0);
    atomic_store(&g_local_height_at_detect, -1);
    atomic_store(&g_peer_max_at_detect, -1);
    atomic_store(&g_tip_age_at_detect, 0);
    atomic_store(&g_test_remedy_calls, 0);
    cutover_modes_set_header_pipeline(CUTOVER_STAGE_MODE_SHADOW,
                                      CUTOVER_STAGE_MODE_SHADOW);
}

int cutover_no_forward_progress_test_remedy_calls(void)
{
    return atomic_load(&g_test_remedy_calls);
}
#endif
