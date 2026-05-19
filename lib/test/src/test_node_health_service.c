/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Tests for node health snapshot service. */

#include "test/test_helpers.h"
#include "config/db_service.h"
#include "config/runtime.h"
#include "controllers/network_controller.h"
#include "net/connman.h"
#include "net/net.h"
#include "services/chain_advance_coordinator.h"
#include "validation/main_state.h"
#include "util/safe_alloc.h"

int test_node_health_service(void)
{
    int failures = 0;

    printf("node_health_service: idle snapshot reports unhealthy without peers... ");
    {
        struct node_health_snapshot health;
        rpc_net_set_connman(NULL);
        sync_set_state(SYNC_IDLE, "reset");
        node_health_collect(&health, NULL, NULL);

        bool ok = (health.sync_state == SYNC_IDLE);
        ok = ok && !health.synced;
        ok = ok && !health.has_peers;
        ok = ok && !health.healthy;
        ok = ok && !health.tor_ready;
        ok = ok && !health.onion_service_ready;
        ok = ok && health.tip_height == -1;
        ok = ok && health.header_height == -1;
        ok = ok && health.tip_lag == 0;
        ok = ok && !health.catchup_active;
        ok = ok && health.catchup_progress_age_seconds == 0;
        ok = ok && !health.import_active;
        ok = ok && health.import_progress_age_seconds == 0;
        ok = ok && strcmp(health.degraded_reason, "no_peers") == 0;

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("node_health_service: at_tip without peers stays unhealthy... ");
    {
        struct node_health_snapshot health;
        rpc_net_set_connman(NULL);
        sync_set_state(SYNC_FINDING_PEERS, "test");
        sync_set_state(SYNC_HEADERS_DOWNLOAD, "test");
        sync_set_state(SYNC_BLOCKS_DOWNLOAD, "test");
        sync_set_state(SYNC_CONNECTING_BLOCKS, "test");
        sync_set_state(SYNC_AT_TIP, "test");
        node_health_collect(&health, NULL, NULL);

        bool ok = health.synced;
        ok = ok && !health.has_peers;
        ok = ok && !health.healthy;
        ok = ok && health.peer_count == 0;
        ok = ok && !health.onion_service_ready;
        ok = ok && strcmp(health.degraded_reason, "no_peers") == 0;

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("node_health_service: headers ahead of active tip report degraded state... ");
    {
        struct node_health_snapshot health;
        struct main_state ms;
        struct connman cm;
        struct net_address addr;
        struct p2p_node *node = NULL;
        struct block_index tip, header;
        struct uint256 h_tip = {0}, h_hdr = {0};

        memset(&health, 0, sizeof(health));
        memset(&cm, 0, sizeof(cm));
        memset(&addr, 0, sizeof(addr));
        memset(&tip, 0, sizeof(tip));
        memset(&header, 0, sizeof(header));
        main_state_init(&ms);
        net_manager_init(&cm.manager);
        block_index_init(&tip);
        block_index_init(&header);

        h_tip.data[0] = 1;
        h_hdr.data[0] = 2;
        tip.phashBlock = &h_tip;
        tip.nHeight = 100;
        tip.nTime = (uint32_t)time(NULL);
        header.phashBlock = &h_hdr;
        header.nHeight = 125;
        header.pprev = &tip;
        header.nTime = tip.nTime;
        bool ok = active_chain_set_tip(&ms.chain_active, &tip);
        ms.pindex_best_header = &header;

        cm.manager.nodes = zcl_calloc(1, sizeof(*cm.manager.nodes), "test_nodes");
        ok = ok && (cm.manager.nodes != NULL);
        node = p2p_node_create(&cm.manager, ZCL_INVALID_SOCKET, &addr,
                               "test-peer", false);
        ok = ok && (node != NULL);
        if (ok) {
            node->starting_height = 125;
            cm.manager.nodes[0] = node;
            cm.manager.num_nodes = 1;
            rpc_net_set_connman(&cm);
            sync_set_state(SYNC_FINDING_PEERS, "test");
            sync_set_state(SYNC_HEADERS_DOWNLOAD, "test");
            sync_set_state(SYNC_BLOCKS_DOWNLOAD, "test");
            sync_set_state(SYNC_CONNECTING_BLOCKS, "test");
            sync_set_state(SYNC_AT_TIP, "test");
            node_health_collect(&health, NULL, &ms);

            ok = health.synced;
            ok = ok && !health.healthy;
            ok = ok && health.tip_height == 100;
            ok = ok && health.header_height == 125;
            ok = ok && strcmp(health.degraded_reason, "headers_ahead_25") == 0;
        }

        main_state_free(&ms);
        rpc_net_set_connman(NULL);
        net_manager_free(&cm.manager);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("node_health_service: coordinator at-tip overrides stale sync FSM... ");
    {
        struct cac_decision decision;
        memset(&decision, 0, sizeof(decision));
        decision.result = CAC_DECISION_USE_SOURCE;
        decision.selected_source = CAC_SOURCE_P2P;
        decision.local_height = 3117591;
        decision.target_height = 3117591;
        decision.projection_height = 3117591;
        decision.projection_lag = 0;
        struct cac_source_status *p2p =
            &decision.sources[CAC_SOURCE_P2P];
        p2p->source = CAC_SOURCE_P2P;
        p2p->available = true;
        p2p->healthy = true;
        p2p->selectable = true;
        p2p->height = 3117591;

        bool ok = node_health_chain_advance_synced(&decision);
        decision.projection_lag = 2;
        ok = ok && !node_health_chain_advance_synced(&decision);
        decision.projection_lag = 0;
        snprintf(decision.blocker, sizeof(decision.blocker),
                 "body-hash-mismatch");
        ok = ok && !node_health_chain_advance_synced(&decision);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("node_health_service: old errors stay visible without degrading... ");
    {
        struct node_health_snapshot health;
        struct error_ring *er = error_ring_global();
        struct connman cm;
        struct net_address addr;
        struct p2p_node *node = NULL;
        bool ok = true;

        memset(&cm, 0, sizeof(cm));
        memset(&addr, 0, sizeof(addr));
        error_ring_init(er);
        net_manager_init(&cm.manager);
        cm.manager.nodes = zcl_calloc(1, sizeof(*cm.manager.nodes), "test_nodes");
        ok = ok && (cm.manager.nodes != NULL);
        if (ok) {
            node = p2p_node_create(&cm.manager, ZCL_INVALID_SOCKET, &addr,
                                   "old-error-peer", false);
            ok = ok && (node != NULL);
        }
        if (ok) {
            cm.manager.nodes[0] = node;
            cm.manager.num_nodes = 1;
            rpc_net_set_connman(&cm);
            sync_set_state(SYNC_FINDING_PEERS, "test");
            sync_set_state(SYNC_HEADERS_DOWNLOAD, "test");
            sync_set_state(SYNC_BLOCKS_DOWNLOAD, "test");
            sync_set_state(SYNC_CONNECTING_BLOCKS, "test");
            sync_set_state(SYNC_AT_TIP, "test");
            error_ring_observer(EV_DB_ERROR, 0, "old recoverable error",
                                21, er);
            er->entries[0].timestamp_us =
                ((int64_t)time(NULL) - 600) * 1000000;
            node_health_collect(&health, NULL, NULL);

            ok = health.synced;
            ok = ok && health.has_peers;
            ok = ok && health.healthy;
            ok = ok && health.error_total == 1;
            ok = ok && !health.last_error_recent;
            ok = ok && health.last_error_age_seconds >= 300;
            ok = ok && strcmp(health.last_error_type,
                              event_type_name(EV_DB_ERROR)) == 0;
            ok = ok && strcmp(health.last_error,
                              "old recoverable error") == 0;
            ok = ok && health.degraded_reason[0] == '\0';
        }

        rpc_net_set_connman(NULL);
        net_manager_free(&cm.manager);
        error_ring_init(er);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("node_health_service: recent errors stay actionable without false degradation... ");
    {
        struct node_health_snapshot health;
        struct error_ring *er = error_ring_global();
        struct connman cm;
        struct net_address addr;
        struct p2p_node *node = NULL;
        bool ok = true;

        memset(&cm, 0, sizeof(cm));
        memset(&addr, 0, sizeof(addr));
        error_ring_init(er);
        net_manager_init(&cm.manager);
        cm.manager.nodes = zcl_calloc(1, sizeof(*cm.manager.nodes), "test_nodes");
        ok = ok && (cm.manager.nodes != NULL);
        if (ok) {
            node = p2p_node_create(&cm.manager, ZCL_INVALID_SOCKET, &addr,
                                   "recent-error-peer", false);
            ok = ok && (node != NULL);
        }
        if (ok) {
            cm.manager.nodes[0] = node;
            cm.manager.num_nodes = 1;
            rpc_net_set_connman(&cm);
            sync_set_state(SYNC_FINDING_PEERS, "test");
            sync_set_state(SYNC_HEADERS_DOWNLOAD, "test");
            sync_set_state(SYNC_BLOCKS_DOWNLOAD, "test");
            sync_set_state(SYNC_CONNECTING_BLOCKS, "test");
            sync_set_state(SYNC_AT_TIP, "test");
            error_ring_observer(EV_BLOCK_REJECTED, 0, "fresh reject",
                                12, er);
            node_health_collect(&health, NULL, NULL);

            ok = health.synced;
            ok = ok && health.has_peers;
            ok = ok && health.healthy;
            ok = ok && health.error_total == 1;
            ok = ok && health.last_error_recent;
            ok = ok && health.last_error_age_seconds >= 0;
            ok = ok && health.last_error_age_seconds <= 300;
            ok = ok && strcmp(health.last_error_type,
                              event_type_name(EV_BLOCK_REJECTED)) == 0;
            ok = ok && strcmp(health.last_error, "fresh reject") == 0;
            ok = ok && health.degraded_reason[0] == '\0';
        }

        rpc_net_set_connman(NULL);
        net_manager_free(&cm.manager);
        error_ring_init(er);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("node_health_service: stale DB transaction reports degraded state... ");
    {
        struct node_health_snapshot health;
        struct node_db ndb;
        struct node_db_status st;
        struct db_service dbsvc;
        struct app_runtime_context runtime;
        struct connman cm;
        struct net_address addr;
        struct p2p_node *node = NULL;
        bool ok = node_db_open(&ndb, ":memory:");
        memset(&dbsvc, 0, sizeof(dbsvc));
        memset(&runtime, 0, sizeof(runtime));
        memset(&cm, 0, sizeof(cm));
        memset(&addr, 0, sizeof(addr));
        net_manager_init(&cm.manager);
        cm.manager.nodes = zcl_calloc(1, sizeof(*cm.manager.nodes), "test_nodes");
        ok = ok && (cm.manager.nodes != NULL);
        if (ok) {
            node = p2p_node_create(&cm.manager, ZCL_INVALID_SOCKET, &addr,
                                   "db-test-peer", false);
            ok = ok && (node != NULL);
        }
        if (ok) {
            node->starting_height = 10;
            cm.manager.nodes[0] = node;
            cm.manager.num_nodes = 1;
            rpc_net_set_connman(&cm);
        }
        if (ok) {
            db_service_init(&dbsvc);
            ok = ok && db_service_attach(&dbsvc, &ndb);
            ok = ok && db_service_start(&dbsvc);
            runtime.db_service = &dbsvc;
            app_runtime_set_current(&runtime);
        }

        ok = ok && node_db_begin(&ndb);
        node_db_get_status(&ndb, &st);
        ok = ok && st.tx_open;

        /* Simulate an inactive open transaction and existing peers-free state. */
        if (ndb.state_mutex_init) {
            zcl_mutex_lock(&ndb.state_mutex);
            ndb.last_activity_time = (int64_t)time(NULL) - 61;
            zcl_mutex_unlock(&ndb.state_mutex);
        }

        sync_set_state(SYNC_FINDING_PEERS, "test");
        node_health_collect(&health, &ndb, NULL);
        ok = ok && health.db_open;
        ok = ok && health.db_tx_open;
        ok = ok && health.db_service_started;
        ok = ok && health.db_service_worker_started;
        ok = ok && health.db_service_queue_depth == 0;
        ok = ok && health.db_last_activity_age_seconds >= 60;
        ok = ok && !health.catchup_active;
        ok = ok && health.catchup_progress_age_seconds == 0;
        ok = ok && !health.import_active;
        ok = ok && health.import_progress_age_seconds == 0;
        ok = ok && strncmp(health.degraded_reason, "db_tx_open_", 11) == 0;

        app_runtime_set_current(NULL);
        rpc_net_set_connman(NULL);
        net_manager_free(&cm.manager);
        db_service_stop(&dbsvc);
        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    sync_set_state(SYNC_IDLE, "done");
    return failures;
}
