/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "framework/condition.h"

#include "services/chain_advance_coordinator.h"
#include "services/legacy_mirror_sync_service.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static _Atomic int64_t g_last_tip = -1;
static _Atomic int64_t g_last_tip_change_unix = 0;

static int64_t current_tip(void)
{
    struct main_state *ms = condition_engine_main_state();
    return ms ? (int64_t)active_chain_height(&ms->chain_active) : -1;
}

static bool mirror_has_body_stall_error(void)
{
    struct legacy_mirror_sync_stats s;
    legacy_mirror_sync_stats_snapshot(&s);
    return strstr(s.last_error,
                  "body data available but activation did not advance") != NULL;
}

static bool detect_chain_stalled_with_data(void)
{
    int64_t tip = current_tip();
    int64_t now = (int64_t)time(NULL);
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
    return tip >= 0 && now - changed >= 60 && mirror_has_body_stall_error();
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
    return current_tip() > atomic_load(&g_last_tip);
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
