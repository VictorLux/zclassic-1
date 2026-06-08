/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "conditions/reducer_frontier_reconcile_light.h"

#include "framework/condition.h"
#include "jobs/stage_repair.h"
#include "net/connman.h"
#include "services/sync_monitor.h"
#include "storage/progress_store.h"
#include "util/log_macros.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <sqlite3.h>
#include <stdatomic.h>

static _Atomic int g_local_height_at_detect = -1;
static _Atomic int g_hstar_at_detect = -1;
static _Atomic int g_sweep_top_at_detect = -1;
static _Atomic int g_remedy_calls = 0;

static bool peer_lag_allows_repair(struct main_state *ms)
{
    struct connman *cm = sync_monitor_connman();
    if (!cm)
        return true;

    int local = ms ? active_chain_height(&ms->chain_active) : -1;
    if (local < 0)
        return false;

    int peer_max = connman_max_peer_height(cm);
    if (peer_max > local)
        return true;

    /* A zero-peer copy still needs the local flag/cursor repair so it can hold
     * in repairing state and resume when a body source appears. Non-zero peers
     * that are not ahead are not useful evidence of a stale local tip. */
    return connman_get_node_count(cm) == 0;
}

static bool detect_reducer_frontier_reconcile_light(void)
{
    int64_t tip_age = sync_monitor_tip_advance_age();
    if (tip_age >= 0 && tip_age < 60)
        return false;

    sqlite3 *db = progress_store_db();
    struct main_state *ms = sync_monitor_main_state();
    if (!db || !ms)
        return false;
    if (!peer_lag_allows_repair(ms))
        return false;

    struct stage_reducer_frontier_reconcile_result rr;
    if (!stage_reducer_frontier_reconcile_light_needed(db, ms, &rr))
        return false;
    if (rr.refused_coin_tear || rr.refused_coin_unknown || !rr.repaired)
        return false;

    atomic_store(&g_local_height_at_detect,
                 active_chain_height(&ms->chain_active));
    atomic_store(&g_hstar_at_detect, rr.hstar);
    atomic_store(&g_sweep_top_at_detect, rr.sweep_top);
    return true;
}

static enum condition_remedy_result remedy_reducer_frontier_reconcile_light(void)
{
    sqlite3 *db = progress_store_db();
    struct main_state *ms = sync_monitor_main_state();
    if (!db || !ms)
        return COND_REMEDY_SKIP;

    atomic_fetch_add(&g_remedy_calls, 1);

    struct stage_reducer_frontier_reconcile_result rr;
    if (!stage_reducer_frontier_reconcile_light(db, ms, &rr))
        return COND_REMEDY_FAILED;
    if (rr.refused_coin_unknown) {
        LOG_WARN("condition",
                 "[condition:reducer_frontier_reconcile_light] refused "
                 "coins_applied_height absent");
        return COND_REMEDY_SKIP;
    }
    if (rr.refused_coin_tear) {
        LOG_WARN("condition",
                 "[condition:reducer_frontier_reconcile_light] refused "
                 "coins_applied_height=%d hstar=%d",
                 rr.coins_applied_height, rr.hstar);
        return COND_REMEDY_SKIP;
    }
    if (!rr.repaired)
        return COND_REMEDY_SKIP;

    LOG_WARN("condition",
             "[condition:reducer_frontier_reconcile_light] hstar=%d "
             "coins_applied=%d sweep_top=%d body_fetch=%d->%d "
             "tip_finalize=%d->%d scripts_set=%d have_data_set=%d "
             "have_data_cleared=%d failed_mask_cleared=%d",
             rr.hstar, rr.coins_applied_height, rr.sweep_top,
             rr.body_fetch_cursor_before, rr.body_fetch_cursor_after,
             rr.tip_finalize_cursor_before, rr.tip_finalize_cursor_after,
             rr.scripts_set, rr.have_data_set, rr.have_data_cleared,
             rr.failed_mask_cleared);
    return COND_REMEDY_OK;
}

static bool witness_reducer_frontier_reconcile_light(int64_t target_at_detect)
{
    (void)target_at_detect;

    struct main_state *ms = sync_monitor_main_state();
    if (!ms)
        return false;

    int before = atomic_load(&g_local_height_at_detect);
    if (before < 0)
        return false;

    int now = active_chain_height(&ms->chain_active);
    return now > before;
}

static struct condition c_reducer_frontier_reconcile_light = {
    .name = "reducer_frontier_reconcile_light",
    .severity = COND_CRITICAL,
    .poll_secs = 5,
    .backoff_secs = 30,
    .max_attempts = 5,
    .detect = detect_reducer_frontier_reconcile_light,
    .remedy = remedy_reducer_frontier_reconcile_light,
    .witness = witness_reducer_frontier_reconcile_light,
    .witness_window_secs = 60,
};

void register_reducer_frontier_reconcile_light(void)
{
    (void)condition_register(&c_reducer_frontier_reconcile_light);
}

#ifdef ZCL_TESTING
void reducer_frontier_reconcile_light_test_reset(void)
{
    atomic_store(&g_local_height_at_detect, -1);
    atomic_store(&g_hstar_at_detect, -1);
    atomic_store(&g_sweep_top_at_detect, -1);
    atomic_store(&g_remedy_calls, 0);
    condition_reset_state(&c_reducer_frontier_reconcile_light);
}

int reducer_frontier_reconcile_light_test_remedy_calls(void)
{
    return atomic_load(&g_remedy_calls);
}
#endif
