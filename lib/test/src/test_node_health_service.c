/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Tests for node health snapshot service. */

#include "test/test_helpers.h"
#include "controllers/network_controller.h"
#include "net/connman.h"
#include "net/net.h"
#include "validation/main_state.h"

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

        cm.manager.nodes = calloc(1, sizeof(*cm.manager.nodes));
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

    sync_set_state(SYNC_IDLE, "done");
    return failures;
}
