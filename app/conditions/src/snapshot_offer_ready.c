/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "conditions/snapshot_offer_ready.h"
#include "util/log_macros.h"
#include "framework/condition.h"

#include "config/runtime.h"
#include "event/event.h"
#include "net/snapshot_sync_contract.h"
#include "services/sync_monitor.h"
#include "sync/sync_state.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <stdatomic.h>
#include <stdio.h>

#define SNAPSHOT_OFFER_READY_MIN_GAP 1000

static _Atomic int g_local_height_at_detect = -1;
static _Atomic int g_snapshot_height_at_detect = -1;
static _Atomic int g_peer_id_at_detect = 0;

#ifdef ZCL_TESTING
static struct snapshot_sync_service *g_test_svc;
static _Atomic int g_test_remedy_calls;
#endif

static struct snapshot_sync_service *runtime_snapsync(void)
{
#ifdef ZCL_TESTING
    if (g_test_svc)
        return g_test_svc;
#endif
    struct snapshot_sync_service *svc = app_runtime_snapshot_sync();
    if (svc)
        return svc;
    return snapsync_global_initialized() ? snapsync_global() : NULL;
}

static int local_chain_height(void)
{
    struct main_state *ms = sync_monitor_main_state();
    if (!ms)
        ms = condition_engine_main_state();
    if (!ms)
        return -1;
    return active_chain_height(&ms->chain_active);
}

static bool snapshot_offer_is_active(enum snapshot_sync_state state)
{
    return state == SNAPSYNC_NEGOTIATING ||
           state == SNAPSYNC_RECEIVING ||
           state == SNAPSYNC_VERIFYING;
}

static bool sync_state_can_receive_snapshot(enum sync_state state)
{
    return state == SYNC_IDLE ||
           state == SYNC_FINDING_PEERS ||
           state == SYNC_HEADERS_DOWNLOAD ||
           state == SYNC_BLOCKS_DOWNLOAD ||
           state == SYNC_CONNECTING_BLOCKS;
}

static bool detect_snapshot_offer_ready(void)
{
    struct snapshot_sync_service *svc = runtime_snapsync();
    struct snapsync_status status = {0};
    if (!svc)
        return false;

#ifdef ZCL_TESTING
    if (svc == g_test_svc) {
        status.state = svc->state;
        status.offered_height = svc->offered_height;
        status.offered_count = svc->offered_count;
        status.serving_peer_id = svc->serving_peer_id;
    } else
#endif
    {
        snapsync_get_status_snapshot(svc, &status);
    }
    if (!snapshot_offer_is_active(status.state) ||
        status.offered_height <= 0 ||
        status.offered_count == 0)
        return false;

    int local_h = local_chain_height();
    if (local_h < 0 ||
        local_h >= status.offered_height - SNAPSHOT_OFFER_READY_MIN_GAP)
        return false;

    if (!sync_state_can_receive_snapshot(sync_get_state()))
        return false;

    atomic_store(&g_local_height_at_detect, local_h);
    atomic_store(&g_snapshot_height_at_detect, status.offered_height);
    atomic_store(&g_peer_id_at_detect, (int)status.serving_peer_id);
    return true;
}

static enum condition_remedy_result remedy_snapshot_offer_ready(void)
{
    int local_h = atomic_load(&g_local_height_at_detect);
    int snap_h = atomic_load(&g_snapshot_height_at_detect);
    int peer_id = atomic_load(&g_peer_id_at_detect);

#ifdef ZCL_TESTING
    atomic_fetch_add(&g_test_remedy_calls, 1);
#endif

    LOG_INFO("condition", "[condition:snapshot_offer_ready] local=%d snapshot=%d " "peer=%d sync_state=%s action=set_snapshot_receive", local_h, snap_h, peer_id, sync_state_name(sync_get_state()));

    if (!sync_set_state(SYNC_SNAPSHOT_RECEIVE,
                        "condition snapshot_offer_ready")) {
        event_emitf(EV_SYNC_STATE_CHANGE, 0,
                    "condition snapshot_offer_ready failed local=%d "
                    "snapshot=%d peer=%d",
                    local_h, snap_h, peer_id);
        return COND_REMEDY_FAILED;
    }

    event_emitf(EV_SYNC_STATE_CHANGE, 0,
                "condition snapshot_offer_ready local=%d snapshot=%d "
                "peer=%d",
                local_h, snap_h, peer_id);
    return COND_REMEDY_OK;
}

static bool witness_snapshot_offer_ready(int64_t target_at_detect)
{
    (void)target_at_detect;
    return sync_get_state() == SYNC_SNAPSHOT_RECEIVE;
}

static struct condition c_snapshot_offer_ready = {
    .name = "snapshot_offer_ready",
    .severity = COND_WARN,
    .poll_secs = 5,
    .backoff_secs = 60,
    .max_attempts = 2,
    .detect = detect_snapshot_offer_ready,
    .remedy = remedy_snapshot_offer_ready,
    .witness = witness_snapshot_offer_ready,
    .witness_window_secs = 30,
};

void register_snapshot_offer_ready(void)
{
    (void)condition_register(&c_snapshot_offer_ready);
}

#ifdef ZCL_TESTING
void snapshot_offer_ready_test_reset(void)
{
    g_test_svc = NULL;
    atomic_store(&g_local_height_at_detect, -1);
    atomic_store(&g_snapshot_height_at_detect, -1);
    atomic_store(&g_peer_id_at_detect, 0);
    atomic_store(&g_test_remedy_calls, 0);
}

void snapshot_offer_ready_test_set_service(struct snapshot_sync_service *svc)
{
    g_test_svc = svc;
}

int snapshot_offer_ready_test_remedy_calls(void)
{
    return atomic_load(&g_test_remedy_calls);
}
#endif
