/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"

#include "conditions/watchdog_dissolve_pr3.h"
#include "framework/condition.h"
#include "platform/clock.h"
#include "services/cutover_modes.h"
#include "jobs/header_admit_stage.h"
#include "services/snapshot_sync_service.h"
#include "services/sync_monitor.h"
#include "jobs/validate_headers_stage.h"
#include "sync/sync_state.h"
#include "validation/chainstate.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define WDP3_CHECK(name, expr) do { \
    printf("watchdog_conditions_pr3: %s... ", (name)); \
    if (expr) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

void register_peer_floor_violated(void);
void register_sync_violation_lag(void);
void register_snapshot_offer_ready(void);
void register_cutover_no_forward_progress(void);
void register_cutover_canary_complete(void);

struct fake_clock_pr3 {
    _Atomic int64_t wall_ms;
};

static int64_t fake_now_mono(void *self)
{
    (void)self;
    return 1;
}

static int64_t fake_now_wall(void *self)
{
    struct fake_clock_pr3 *c = (struct fake_clock_pr3 *)self;
    return atomic_load(&c->wall_ms);
}

static void fake_clock_install(struct fake_clock_pr3 *c, int64_t unix_s)
{
    atomic_store(&c->wall_ms, unix_s * 1000);
    static clock_iface_t iface;
    iface.now_monotonic_ns = fake_now_mono;
    iface.now_wall_ms = fake_now_wall;
    iface.self = c;
    clock_set_default(&iface);
}

static void fake_clock_set(struct fake_clock_pr3 *c, int64_t unix_s)
{
    atomic_store(&c->wall_ms, unix_s * 1000);
}

static void reset_pr3(struct connman *cm,
                      struct download_manager *dm,
                      struct main_state *ms)
{
    condition_engine_reset_for_testing();
    peer_floor_violated_test_reset();
    sync_violation_lag_test_reset();
    snapshot_offer_ready_test_reset();
    cutover_no_forward_progress_test_reset();
    cutover_canary_complete_test_reset();
    cutover_modes_test_reset();
    memset(cm, 0, sizeof(*cm));
    memset(dm, 0, sizeof(*dm));
    memset(ms, 0, sizeof(*ms));
    zcl_mutex_init(&cm->manager.cs_nodes);
    zcl_mutex_init(&dm->cs);
    zcl_mutex_init(&ms->cs_main);
    static struct chain_params params;
    memset(&params, 0, sizeof(params));
    cm->params = &params;
    sync_monitor_init();
    sync_monitor_set_context(cm, dm, ms);
}

static void cleanup_pr3(void)
{
    condition_engine_reset_for_testing();
    sync_monitor_set_context(NULL, NULL, NULL);
    clock_reset_default();
    unsetenv("ZCL_PEERLESS_OK");
    header_admit_set_mode(HEADER_ADMIT_MODE_SHADOW);
    validate_headers_set_mode(VALIDATE_HEADERS_MODE_SHADOW);
    cutover_modes_test_reset();
    if (sync_get_state() != SYNC_IDLE)
        sync_set_state(SYNC_IDLE, "test cleanup");
}

int test_watchdog_conditions_pr3(void)
{
    printf("\n=== watchdog PR-3 condition tests ===\n");
    int failures = 0;

    {
        struct fake_clock_pr3 clock;
        fake_clock_install(&clock, 1000);
        struct connman cm;
        struct download_manager dm;
        struct main_state ms;
        reset_pr3(&cm, &dm, &ms);
        bool ok = true;
        register_peer_floor_violated();

        struct p2p_node stuck = {0};
        stuck.id = 1;
        stuck.state = PEER_CONNECTED;
        struct p2p_node *peers[1] = { &stuck };
        cm.manager.nodes = peers;
        cm.manager.num_nodes = 1;

        condition_engine_tick();
        ok = ok && peer_floor_violated_test_remedy_calls() == 0;
        fake_clock_set(&clock, 1061);
        condition_engine_tick();
        ok = ok && peer_floor_violated_test_remedy_calls() == 1;
        ok = ok && stuck.disconnect;
        WDP3_CHECK("peer floor kicks unhealthy outbound slots", ok);
        cleanup_pr3();
    }

    {
        struct fake_clock_pr3 clock;
        fake_clock_install(&clock, 2000);
        struct connman cm;
        struct download_manager dm;
        struct main_state ms;
        reset_pr3(&cm, &dm, &ms);
        bool ok = true;
        register_peer_floor_violated();
        setenv("ZCL_PEERLESS_OK", "1", 1);

        condition_engine_tick();
        fake_clock_set(&clock, 2061);
        condition_engine_tick();
        ok = ok && peer_floor_violated_test_remedy_calls() == 0;
        WDP3_CHECK("peer floor honors peerless test mode", ok);
        cleanup_pr3();
    }

    {
        struct fake_clock_pr3 clock;
        fake_clock_install(&clock, 3000);
        struct connman cm;
        struct download_manager dm;
        struct main_state ms;
        reset_pr3(&cm, &dm, &ms);
        bool ok = true;
        register_sync_violation_lag();

        struct block_index tip = {0};
        tip.nHeight = 100;
        ok = ok && active_chain_set_tip(&ms.chain_active, &tip);
        struct p2p_node peer = {0};
        peer.id = 1;
        peer.starting_height = 250;
        peer.state = PEER_ACTIVE;
        struct p2p_node *peers[1] = { &peer };
        cm.manager.nodes = peers;
        cm.manager.num_nodes = 1;

        condition_engine_tick();
        ok = ok && sync_violation_lag_test_remedy_calls() == 0;
        fake_clock_set(&clock, 3601);
        condition_engine_tick();
        ok = ok && sync_violation_lag_test_remedy_calls() == 1;
        ok = ok && peer.disconnect;
        ok = ok && condition_engine_get_unresolved_count() == 1;
        condition_engine_tick();
        ok = ok && sync_violation_lag_test_remedy_calls() == 1;
        WDP3_CHECK("sync violation rotates peers once and pages", ok);
        cleanup_pr3();
    }

    {
        struct fake_clock_pr3 clock;
        fake_clock_install(&clock, 4000);
        struct connman cm;
        struct download_manager dm;
        struct main_state ms;
        reset_pr3(&cm, &dm, &ms);
        bool ok = true;
        register_snapshot_offer_ready();

        struct block_index tip = {0};
        tip.nHeight = 100;
        ok = ok && active_chain_set_tip(&ms.chain_active, &tip);

        struct snapshot_sync_service svc;
        memset(&svc, 0, sizeof(svc));
        svc.state = SNAPSYNC_NEGOTIATING;
        svc.offered_height = 2000;
        svc.offered_count = 100;
        svc.serving_peer_id = 42;
        snapshot_offer_ready_test_set_service(&svc);

        ok = ok && sync_get_state() == SYNC_IDLE;
        condition_engine_tick();
        ok = ok && snapshot_offer_ready_test_remedy_calls() == 1;
        ok = ok && sync_get_state() == SYNC_SNAPSHOT_RECEIVE;
        ok = ok && condition_engine_get_active_count() == 0;

        condition_engine_tick();
        ok = ok && snapshot_offer_ready_test_remedy_calls() == 1;
        WDP3_CHECK("snapshot offer ready reasserts snapshot receive", ok);
        cleanup_pr3();
    }

    {
        struct fake_clock_pr3 clock;
        fake_clock_install(&clock, 4500);
        struct connman cm;
        struct download_manager dm;
        struct main_state ms;
        reset_pr3(&cm, &dm, &ms);
        bool ok = true;
        register_snapshot_offer_ready();

        struct block_index tip = {0};
        tip.nHeight = 100;
        ok = ok && active_chain_set_tip(&ms.chain_active, &tip);

        ok = ok && sync_set_state(SYNC_FINDING_PEERS,
                                  "test snapshot at-tip setup");
        ok = ok && sync_set_state(SYNC_HEADERS_DOWNLOAD,
                                  "test snapshot at-tip setup");
        ok = ok && sync_set_state(SYNC_AT_TIP,
                                  "test snapshot at-tip setup");

        struct snapshot_sync_service svc;
        memset(&svc, 0, sizeof(svc));
        svc.state = SNAPSYNC_NEGOTIATING;
        svc.offered_height = 2000;
        svc.offered_count = 100;
        svc.serving_peer_id = 43;
        snapshot_offer_ready_test_set_service(&svc);

        condition_engine_tick();
        ok = ok && snapshot_offer_ready_test_remedy_calls() == 0;
        ok = ok && sync_get_state() == SYNC_AT_TIP;
        ok = ok && condition_engine_get_active_count() == 0;
        WDP3_CHECK("snapshot offer ready ignores at-tip state", ok);
        cleanup_pr3();
    }

    {
        struct fake_clock_pr3 clock;
        fake_clock_install(&clock, 5000);
        struct connman cm;
        struct download_manager dm;
        struct main_state ms;
        reset_pr3(&cm, &dm, &ms);
        bool ok = true;
        register_cutover_no_forward_progress();

        struct block_index tip = {0};
        tip.nHeight = 100;
        ok = ok && active_chain_set_tip(&ms.chain_active, &tip);
        sync_monitor_on_block_connected(100);

        struct p2p_node peer = {0};
        peer.id = 1;
        peer.starting_height = 101;
        peer.state = PEER_ACTIVE;
        struct p2p_node *peers[1] = { &peer };
        cm.manager.nodes = peers;
        cm.manager.num_nodes = 1;

        header_admit_set_mode(HEADER_ADMIT_MODE_AUTHORITATIVE);
        validate_headers_set_mode(VALIDATE_HEADERS_MODE_AUTHORITATIVE);

        condition_engine_tick();
        ok = ok && cutover_no_forward_progress_test_remedy_calls() == 0;
        ok = ok && header_admit_get_mode() == HEADER_ADMIT_MODE_AUTHORITATIVE;
        ok = ok && validate_headers_get_mode() ==
             VALIDATE_HEADERS_MODE_AUTHORITATIVE;

        fake_clock_set(&clock, 5181);
        condition_engine_tick();
        ok = ok && cutover_no_forward_progress_test_remedy_calls() == 1;
        ok = ok && header_admit_get_mode() == HEADER_ADMIT_MODE_SHADOW;
        ok = ok && validate_headers_get_mode() ==
             VALIDATE_HEADERS_MODE_SHADOW;
        ok = ok && condition_engine_get_unresolved_count() == 1;

        struct block_index next = {0};
        next.nHeight = 101;
        ok = ok && active_chain_set_tip(&ms.chain_active, &next);
        sync_monitor_on_block_connected(101);
        condition_engine_tick();
        ok = ok && condition_engine_get_active_count() == 0;
        WDP3_CHECK("cutover guard reverts stuck authoritative stages", ok);
        cleanup_pr3();
    }

    {
        struct fake_clock_pr3 clock;
        fake_clock_install(&clock, 6000);
        struct connman cm;
        struct download_manager dm;
        struct main_state ms;
        reset_pr3(&cm, &dm, &ms);
        bool ok = true;
        register_cutover_no_forward_progress();

        struct block_index tip = {0};
        tip.nHeight = 100;
        ok = ok && active_chain_set_tip(&ms.chain_active, &tip);
        sync_monitor_on_block_connected(100);

        struct p2p_node peer = {0};
        peer.id = 1;
        peer.starting_height = 100;
        peer.state = PEER_ACTIVE;
        struct p2p_node *peers[1] = { &peer };
        cm.manager.nodes = peers;
        cm.manager.num_nodes = 1;

        validate_headers_set_mode(VALIDATE_HEADERS_MODE_AUTHORITATIVE);
        fake_clock_set(&clock, 6181);
        condition_engine_tick();
        ok = ok && cutover_no_forward_progress_test_remedy_calls() == 0;
        ok = ok && validate_headers_get_mode() ==
             VALIDATE_HEADERS_MODE_AUTHORITATIVE;
        WDP3_CHECK("cutover guard ignores at-tip authoritative mode", ok);
        cleanup_pr3();
    }

    {
        struct fake_clock_pr3 clock;
        fake_clock_install(&clock, 7000);
        struct connman cm;
        struct download_manager dm;
        struct main_state ms;
        reset_pr3(&cm, &dm, &ms);
        bool ok = true;
        register_cutover_canary_complete();

        struct block_index tip = {0};
        tip.nHeight = 100;
        ok = ok && active_chain_set_tip(&ms.chain_active, &tip);
        cutover_modes_set_header_pipeline(CUTOVER_STAGE_MODE_AUTHORITATIVE,
                                          CUTOVER_STAGE_MODE_AUTHORITATIVE);
        cutover_modes_record_change(100, 100, 101, 1);

        condition_engine_tick();
        bool armed = cutover_canary_complete_test_remedy_calls() == 0 &&
                     header_admit_get_mode() ==
                         HEADER_ADMIT_MODE_AUTHORITATIVE &&
                     validate_headers_get_mode() ==
                         VALIDATE_HEADERS_MODE_AUTHORITATIVE;
        WDP3_CHECK("cutover canary remains armed before target", armed);

        struct block_index next = {0};
        next.nHeight = 101;
        ok = ok && active_chain_set_tip(&ms.chain_active, &next);
        fake_clock_set(&clock, 7001);
        condition_engine_tick();
        
        /* The canary matched with no divergence. It clears the canary and locks in 
         * authoritative mode. It does not revert. */
        bool locked_in =
            cutover_canary_complete_test_remedy_calls() == 1 &&
            header_admit_get_mode() == HEADER_ADMIT_MODE_AUTHORITATIVE &&
            validate_headers_get_mode() == VALIDATE_HEADERS_MODE_AUTHORITATIVE;
        WDP3_CHECK("cutover canary locks in authoritative mode on success", locked_in);

        /* Advance clock past witness window to allow the condition to clear.
         * The witness will fail (because authoritative mode is still active),
         * but since the canary target was cleared, detect() will return false,
         * causing the engine to clear the condition. */
        fake_clock_set(&clock, 7010);
        condition_engine_tick();
        
        bool cleared = condition_engine_get_active_count() == 0;
        WDP3_CHECK("cutover canary clears after success", cleared);
        
        cleanup_pr3();
    }

    cleanup_pr3();
    return failures;
}
