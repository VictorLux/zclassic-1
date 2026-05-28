/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "conditions/watchdog_dissolve_pr3.h"
#include "util/log_macros.h"
#include "framework/condition.h"

#include "event/event.h"
#include "services/cutover_modes.h"
#include "services/sync_monitor.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include "jobs/tip_finalize_stage.h"

#include <stdatomic.h>
#include <stdio.h>

#ifdef ZCL_TESTING
static _Atomic int g_test_remedy_calls;
#endif

static bool detect_cutover_canary_complete(void)
{
    struct main_state *ms = sync_monitor_main_state();
    if (!ms)
        return false;
    int local = active_chain_height(&ms->chain_active);
    return cutover_modes_canary_target_reached(local);
}

static enum condition_remedy_result remedy_cutover_canary_complete(void)
{
    struct main_state *ms = sync_monitor_main_state();
    int local = ms ? active_chain_height(&ms->chain_active) : -1;
    struct cutover_canary_snapshot snap;
    cutover_modes_canary_snapshot(local, &snap);

    if (!snap.authoritative_active || !snap.passed)
        return COND_REMEDY_SKIP;

    uint64_t diverged = tip_finalize_stage_utxo_count_diverged_total() +
                        tip_finalize_stage_precondition_failed_total() +
                        tip_finalize_stage_upstream_failed_total();

    if (diverged > 0) {
        cutover_modes_revert_all_to_shadow();

        LOG_WARN("condition", "[condition:cutover_canary_complete] REVERTED DUE TO DIVERGENCE! " "target=%lld current=%lld changed_at=%lld diverged=%llu", (long long)snap.target_height, (long long)snap.current_tip_height, (long long)snap.changed_at_unix, (unsigned long long)diverged);
        event_emitf(EV_SYNC_STATE_CHANGE, 0,
                    "condition CUTOVER_CANARY_COMPLETE FAILED_DIVERGENCE target=%lld current=%lld",
                    (long long)snap.target_height,
                    (long long)snap.current_tip_height);
    } else {
        cutover_modes_clear_canary();
        LOG_INFO("condition", "[condition:cutover_canary_complete] passed with NO divergence! target=%lld. Authoritative mode locked in.", (long long)snap.target_height);
        event_emitf(EV_SYNC_STATE_CHANGE, 0,
                    "condition CUTOVER_CANARY_COMPLETE PASSED target=%lld",
                    (long long)snap.target_height);
    }

#ifdef ZCL_TESTING
    atomic_fetch_add(&g_test_remedy_calls, 1);
#endif
    return COND_REMEDY_OK;
}

static bool witness_cutover_canary_complete(int64_t target_at_detect)
{
    (void)target_at_detect;
    /* The remedy has succeeded if we either successfully reverted to shadow,
     * OR if the canary passed without divergence and was cleared. */
    if (!cutover_modes_any_authoritative_active())
        return true;
        
    /* If authoritative mode is still active, we must check if the canary was 
     * cleared. If the target was cleared, it means we locked in authoritative mode. */
    struct main_state *ms = sync_monitor_main_state();
    int local = ms ? active_chain_height(&ms->chain_active) : -1;
    return !cutover_modes_canary_target_reached(local);
}

static struct condition c_cutover_canary_complete = {
    .name = "cutover_canary_complete",
    .severity = COND_WARN,
    .poll_secs = 1,
    .backoff_secs = 5,
    .max_attempts = 3,
    .detect = detect_cutover_canary_complete,
    .remedy = remedy_cutover_canary_complete,
    .witness = witness_cutover_canary_complete,
    .witness_window_secs = 5,
};

void register_cutover_canary_complete(void)
{
    (void)condition_register(&c_cutover_canary_complete);
}

#ifdef ZCL_TESTING
void cutover_canary_complete_test_reset(void)
{
    atomic_store(&g_test_remedy_calls, 0);
}

int cutover_canary_complete_test_remedy_calls(void)
{
    return atomic_load(&g_test_remedy_calls);
}
#endif
