/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"

static void test_set_ipv4(struct net_address *addr,
                          uint8_t a, uint8_t b, uint8_t c, uint8_t d,
                          uint16_t port)
{
    net_address_init(addr);
    addr->svc.addr.ip[10] = 0xff;
    addr->svc.addr.ip[11] = 0xff;
    addr->svc.addr.ip[12] = a;
    addr->svc.addr.ip[13] = b;
    addr->svc.addr.ip[14] = c;
    addr->svc.addr.ip[15] = d;
    addr->svc.port = port;
}

int test_connman_addnode_fallback(void)
{
    int failures = 0;

    printf("connman_addnode_fallback: addnodes drain before addrman... ");
    {
        chain_params_select(CHAIN_MAIN);
        const struct chain_params *params = chain_params_get();
        struct connman cm;
        struct node_signals sigs;
        memset(&sigs, 0, sizeof(sigs));
        bool ok = connman_init(&cm, params, &sigs);

        for (int i = 0; ok && i < 10; i++) {
            struct net_address addr;
            test_set_ipv4(&addr, 203, 0, 113, (uint8_t)(10 + i), 8033);
            cm.addnodes[cm.num_addnodes++] = addr;
        }

        for (int i = 0; ok && i < 10; i++) {
            struct net_address want = cm.addnodes[i];
            struct addr_info pick;
            enum connman_outbound_target_source source = CONNMAN_TARGET_NONE;
            size_t addnode_index = SIZE_MAX;
            memset(&pick, 0, sizeof(pick));
            ok = connman_pick_next_outbound_target(&cm,
                                                   &cm.next_addnode_cursor,
                                                   &pick,
                                                   &source,
                                                   &addnode_index);
            ok = ok && source == CONNMAN_TARGET_ADDNODE;
            ok = ok && addnode_index == (size_t)i;
            ok = ok && net_addr_eq(&pick.addr.svc.addr, &want.svc.addr);
            ok = ok && pick.addr.svc.port == want.svc.port;

            /* Simulate a failed dial so the next pick advances instead of
             * returning the same addnode again within the cooldown window. */
            connman_record_addnode_attempt(&cm, addnode_index, false);
        }

        if (ok) {
            struct addr_info pick;
            enum connman_outbound_target_source source = CONNMAN_TARGET_NONE;
            memset(&pick, 0, sizeof(pick));
            ok = !connman_pick_next_outbound_target(&cm,
                                                    &cm.next_addnode_cursor,
                                                    &pick,
                                                    &source,
                                                    NULL);
        }

        connman_free(&cm);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    return failures;
}
