/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "framework/condition.h"

#include "platform/time_compat.h"
#include "services/chain_advance_coordinator.h"
#include "services/legacy_mirror_sync_service.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static _Atomic int64_t g_last_tip = -1;
static _Atomic int64_t g_last_tip_change_unix = 0;
static _Atomic int64_t g_tip_at_detect = -1;

#ifdef ZCL_TESTING
static _Atomic int64_t g_test_tip = -2;
#endif

static int64_t current_tip(void)
{
#ifdef ZCL_TESTING
    int64_t test_tip = atomic_load(&g_test_tip);
    if (test_tip != -2)
        return test_tip;
#endif
    struct main_state *ms = condition_engine_main_state();
    return ms ? (int64_t)active_chain_height(&ms->chain_active) : -1;
}

static bool mirror_has_activation_no_progress_blocker(void)
{
    struct legacy_mirror_sync_stats s;
    legacy_mirror_sync_stats_snapshot(&s);
    return strcmp(s.activation_blocker, "activation-no-progress") == 0 ||
           strcmp(s.last_blocker_code, "activation-no-progress") == 0;
}

static bool detect_chain_stalled_with_data(void)
{
    int64_t tip = current_tip();
    int64_t now = platform_time_wall_unix();
    int64_t prev = atomic_load(&g_last_tip);
    if (prev != tip) {
        atomic_store(&g_last_tip, tip);
        atomic_store(&g_last_tip_change_unix, now);
        return false;
    }
    int64_t changed = atomic_load(&g_last_tip_change_unix);
    if (changed == 0) {
        atomic_store(&g_last_tip_change_unix, now);
        return false;
    }
    bool stalled = tip >= 0 && now - changed >= 60 &&
                   mirror_has_activation_no_progress_blocker();
    if (stalled)
        atomic_store(&g_tip_at_detect, tip);
    return stalled;
}

static enum condition_remedy_result remedy_chain_stalled_with_data(void)
{
    fprintf(stderr,  // obs-ok:condition-chain-stalled
            "[condition:chain_stalled_with_data] forcing mirror promotion\n");
    chain_advance_coordinator_force_mirror_promotion(
        "condition:chain_stalled_with_data");
    return COND_REMEDY_OK;
}

static bool witness_chain_stalled_with_data(int64_t target_at_detect)
{
    (void)target_at_detect;
    int64_t tip_at_detect = atomic_load(&g_tip_at_detect);
    return tip_at_detect >= 0 && current_tip() > tip_at_detect;
}

static struct condition c_chain_stalled_with_data = {
    .name = "chain_stalled_with_data",
    .severity = COND_WARN,
    .poll_secs = 10,
    .backoff_secs = 60,
    .max_attempts = 5,
    .detect = detect_chain_stalled_with_data,
    .remedy = remedy_chain_stalled_with_data,
    .witness = witness_chain_stalled_with_data,
    .witness_window_secs = 60,
};

void register_chain_stalled_with_data(void)
{
    (void)condition_register(&c_chain_stalled_with_data);
}

#ifdef ZCL_TESTING
void chain_stalled_with_data_test_reset(void)
{
    struct condition_state *s = &c_chain_stalled_with_data.state;
    atomic_store(&g_last_tip, -1);
    atomic_store(&g_last_tip_change_unix, 0);
    atomic_store(&g_tip_at_detect, -1);
    atomic_store(&g_test_tip, -2);
    atomic_store(&s->attempts, 0);
    atomic_store(&s->last_outcome, COND_REMEDY_SKIP);
    atomic_store(&s->currently_active, false);
}

void chain_stalled_with_data_test_seed_tip(int64_t tip,
                                           int64_t last_change_unix)
{
    atomic_store(&g_test_tip, tip);
    atomic_store(&g_last_tip, tip);
    atomic_store(&g_last_tip_change_unix, last_change_unix);
}

bool chain_stalled_with_data_test_detect(void)
{
    return detect_chain_stalled_with_data();
}
#endif
