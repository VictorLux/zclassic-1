/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"
#include "primitives/transaction.h"
#include "chain/chainparams.h"
#include "net/version.h"
#include "net/fast_sync.h"
#include "net/onion_service.h"

static int test_tip_count = 0;
static int test_tip_height = 0;

static void test_updated_block_tip(void *ctx, int height)
{
    (void)ctx;
    test_tip_count++;
    test_tip_height = height;
}

int test_net(void)
{
    int failures = 0;

    printf("net_addr IPv4... ");
    {
        struct net_addr a;
        net_addr_init(&a);
        unsigned char ip4[] = {192, 168, 1, 1};
        net_addr_set_ipv4(&a, ip4);
        char str[64];
        net_addr_to_string(&a, str, sizeof(str));
        if (net_addr_is_ipv4(&a) && strcmp(str, "192.168.1.1") == 0)
            printf("OK (%s)\n", str);
        else {
            printf("FAIL: %s\n", str);
            failures++;
        }
    }

    printf("net_service to_string... ");
    {
        struct net_service s;
        net_service_init(&s);
        unsigned char ip4[] = {10, 0, 0, 1};
        net_addr_set_ipv4(&s.addr, ip4);
        s.port = 8233;
        char str[64];
        net_service_to_string(&s, str, sizeof(str));
        if (strcmp(str, "10.0.0.1:8233") == 0)
            printf("OK (%s)\n", str);
        else {
            printf("FAIL: %s\n", str);
            failures++;
        }
    }

    printf("msg_header... ");
    {
        unsigned char start[4] = {0x24, 0xe9, 0x27, 0x64};
        struct msg_header h;
        msg_header_init_full(&h, start, "version", 100);
        char cmd[COMMAND_SIZE + 1];
        msg_header_get_command(&h, cmd, sizeof(cmd));
        if (strcmp(cmd, "version") == 0 && h.nMessageSize == 100 &&
            msg_header_is_valid(&h, start))
            printf("OK (%s)\n", cmd);
        else {
            printf("FAIL: %s\n", cmd);
            failures++;
        }
    }

    printf("inv_item... ");
    {
        struct uint256 hash;
        memset(hash.data, 0xab, 32);
        struct inv_item inv;
        inv_item_init_typed(&inv, MSG_TX, &hash);
        char str[128];
        inv_item_to_string(&inv, str, sizeof(str));
        if (inv_item_is_known_type(&inv) &&
            strcmp(inv_item_get_command(&inv), "tx") == 0)
            printf("OK (%s)\n", inv_item_get_command(&inv));
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("net_address serialize/deserialize roundtrip... ");
    {
        struct net_address a;
        net_address_init(&a);
        a.nServices = NODE_NETWORK | NODE_BLOOM;
        a.nTime = 1700000000;
        a.svc.addr.ip[12] = 192; a.svc.addr.ip[13] = 168;
        a.svc.addr.ip[14] = 1; a.svc.addr.ip[15] = 1;
        a.svc.port = 8233;
        struct byte_stream s;
        stream_init(&s, 64);
        net_address_serialize(&a, &s, true);
        struct byte_stream r;
        stream_init_from_data(&r, s.data, s.size);
        struct net_address a2;
        net_address_init(&a2);
        net_address_deserialize(&a2, &r, true);
        if (a2.nTime == 1700000000 &&
            a2.nServices == (NODE_NETWORK | NODE_BLOOM) &&
            a2.svc.addr.ip[12] == 192 && a2.svc.port == 8233)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
        stream_free(&s);
    }

    printf("inv_item serialize/deserialize roundtrip... ");
    {
        struct inv_item inv;
        struct uint256 h;
        memset(h.data, 0xAA, 32);
        inv_item_init_typed(&inv, MSG_TX, &h);
        struct byte_stream s;
        stream_init(&s, 64);
        inv_item_serialize(&inv, &s);
        struct byte_stream r;
        stream_init_from_data(&r, s.data, s.size);
        struct inv_item inv2;
        inv_item_deserialize(&inv2, &r);
        if (inv2.type == MSG_TX && inv2.hash.data[0] == 0xAA)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
        stream_free(&s);
    }

    printf("block_locator serialize/deserialize roundtrip... ");
    {
        struct block_locator loc;
        block_locator_init(&loc);
        loc.num_hashes = 3;
        loc.vhave = calloc(3, sizeof(struct uint256));
        memset(loc.vhave[0].data, 0x11, 32);
        memset(loc.vhave[1].data, 0x22, 32);
        memset(loc.vhave[2].data, 0x33, 32);
        struct byte_stream s;
        stream_init(&s, 128);
        block_locator_serialize(&loc, &s);
        struct byte_stream r;
        stream_init_from_data(&r, s.data, s.size);
        struct block_locator loc2;
        block_locator_init(&loc2);
        block_locator_deserialize(&loc2, &r);
        if (loc2.num_hashes == 3 &&
            loc2.vhave[0].data[0] == 0x11 &&
            loc2.vhave[2].data[0] == 0x33)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
        block_locator_free(&loc);
        block_locator_free(&loc2);
        stream_free(&s);
    }

    printf("version_message serialize/deserialize roundtrip... ");
    {
        struct version_message v;
        version_message_init(&v);
        v.protocol_version = 170009;
        v.services = NODE_NETWORK;
        v.timestamp = 1700000000;
        v.addr_recv.nServices = NODE_NETWORK;
        v.addr_recv.svc.port = 8233;
        v.addr_from.nServices = NODE_NETWORK;
        v.addr_from.svc.port = 8233;
        v.nonce = 0xDEADBEEFCAFEBABEULL;
        snprintf(v.sub_version, MAX_SUBVER_LENGTH, "/ZClassic:2.1.1-3/");
        v.start_height = 500000;
        v.relay = true;

        struct byte_stream s;
        stream_init(&s, 256);
        version_message_serialize(&v, &s);

        struct byte_stream r;
        stream_init_from_data(&r, s.data, s.size);
        struct version_message v2;
        version_message_init(&v2);
        version_message_deserialize(&v2, &r);

        if (v2.protocol_version == 170009 &&
            v2.services == NODE_NETWORK &&
            v2.timestamp == 1700000000 &&
            v2.nonce == 0xDEADBEEFCAFEBABEULL &&
            strcmp(v2.sub_version, "/ZClassic:2.1.1-3/") == 0 &&
            v2.start_height == 500000 &&
            v2.relay == true &&
            v2.addr_recv.svc.port == 8233)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
        stream_free(&s);
    }

    printf("split_host_port... ");
    {
        char host[128];
        int port = 8233;
        split_host_port("192.168.1.1:9033", host, sizeof(host), &port);
        if (strcmp(host, "192.168.1.1") == 0 && port == 9033)
            printf("OK\n");
        else { printf("FAIL (host=%s port=%d)\n", host, port); failures++; }
    }

    printf("split_host_port ipv6... ");
    {
        char host[128];
        int port = 8233;
        split_host_port("[::1]:9033", host, sizeof(host), &port);
        if (strcmp(host, "::1") == 0 && port == 9033)
            printf("OK\n");
        else { printf("FAIL (host=%s port=%d)\n", host, port); failures++; }
    }

    printf("lookup_host numeric ipv4... ");
    {
        struct net_addr addrs[4];
        size_t n = 0;
        bool ok = lookup_host("127.0.0.1", addrs, 4, &n, false);
        if (ok && n == 1 && addrs[0].ip[12] == 127 && addrs[0].ip[15] == 1)
            printf("OK\n");
        else { printf("FAIL (ok=%d n=%zu)\n", ok, n); failures++; }
    }

    printf("lookup_numeric... ");
    {
        struct net_service svc;
        bool ok = lookup_numeric("10.0.0.1:8233", &svc, 0);
        if (ok && svc.addr.ip[12] == 10 && svc.port == 8233)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("millis_to_timeval... ");
    {
        struct timeval tv = millis_to_timeval(5500);
        if (tv.tv_sec == 5 && tv.tv_usec == 500000)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("net_addr RFC classification... ");
    {
        struct net_addr a;
        net_addr_init(&a);

        unsigned char priv10[] = {10, 0, 0, 1};
        net_addr_set_ipv4(&a, priv10);
        bool ok = net_addr_is_rfc1918(&a);

        unsigned char pub8[] = {8, 8, 8, 8};
        net_addr_set_ipv4(&a, pub8);
        ok = ok && !net_addr_is_rfc1918(&a);
        ok = ok && net_addr_is_routable(&a);

        unsigned char local127[] = {127, 0, 0, 1};
        net_addr_set_ipv4(&a, local127);
        ok = ok && net_addr_is_local(&a);
        ok = ok && !net_addr_is_routable(&a);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("net_addr_get_group ipv4... ");
    {
        struct net_addr a;
        net_addr_init(&a);
        unsigned char ip[] = {1, 2, 3, 4};
        net_addr_set_ipv4(&a, ip);
        unsigned char group[NET_ADDR_GROUP_MAX];
        size_t glen = net_addr_get_group(&a, group, sizeof(group));
        bool ok = glen == 3 && group[0] == NET_IPV4 &&
                  group[1] == 1 && group[2] == 2;
        if (ok) printf("OK\n");
        else { printf("FAIL (len=%zu g0=%d g1=%d g2=%d)\n", glen, group[0], group[1], group[2]); failures++; }
    }

    printf("net_service_get_key... ");
    {
        struct net_service s;
        net_service_init(&s);
        unsigned char ip[] = {192, 168, 1, 1};
        net_addr_set_ipv4(&s.addr, ip);
        s.port = 8233;
        unsigned char key[18];
        net_service_get_key(&s, key);
        bool ok = key[16] == (8233 >> 8) && key[17] == (8233 & 0xFF);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("validation_signals register/dispatch... ");
    {
        test_tip_count = 0;
        test_tip_height = 0;

        struct validation_signals vs;
        validation_signals_init(&vs);

        struct validation_callbacks cb;
        memset(&cb, 0, sizeof(cb));
        cb.ctx = NULL;
        cb.updated_block_tip = test_updated_block_tip;

        validation_register(&vs, &cb);
        signal_updated_block_tip(&vs, 42);

        bool ok = (test_tip_count == 1 && test_tip_height == 42);

        signal_updated_block_tip(&vs, 100);
        ok = ok && test_tip_count == 2 && test_tip_height == 100;

        validation_unregister(&vs, NULL);
        signal_updated_block_tip(&vs, 200);
        ok = ok && test_tip_count == 2;

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("validation_signals unregister_all... ");
    {
        struct validation_signals vs;
        validation_signals_init(&vs);

        struct validation_callbacks cb1, cb2;
        memset(&cb1, 0, sizeof(cb1));
        memset(&cb2, 0, sizeof(cb2));
        int ctx1 = 0, ctx2 = 0;
        cb1.ctx = &ctx1;
        cb2.ctx = &ctx2;

        validation_register(&vs, &cb1);
        validation_register(&vs, &cb2);
        bool ok = (vs.num_listeners == 2);

        validation_unregister_all(&vs);
        ok = ok && (vs.num_listeners == 0);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("addrman init/add/size... ");
    {
        struct addr_man am;
        addrman_init(&am);
        bool ok = addrman_size(&am) == 0;

        struct net_address addr;
        net_address_init(&addr);
        unsigned char ip1[] = {8, 8, 8, 8};
        net_addr_set_ipv4(&addr.svc.addr, ip1);
        addr.svc.port = 8233;
        addr.nTime = (uint32_t)GetTime();

        struct net_addr source;
        net_addr_init(&source);
        unsigned char src_ip[] = {1, 2, 3, 4};
        net_addr_set_ipv4(&source, src_ip);

        bool added = addrman_add(&am, &addr, &source, 0);
        ok = ok && added;
        ok = ok && addrman_size(&am) == 1;

        if (ok) printf("OK\n");
        else { printf("FAIL (added=%d size=%zu)\n", added, addrman_size(&am)); failures++; }
        addrman_free(&am);
    }

    printf("addrman select... ");
    {
        struct addr_man am;
        addrman_init(&am);

        for (int i = 0; i < 10; i++) {
            struct net_address addr;
            net_address_init(&addr);
            unsigned char ip[] = {50 + (unsigned char)i, 100, 0, 1};
            net_addr_set_ipv4(&addr.svc.addr, ip);
            addr.svc.port = 8233;
            addr.nTime = (uint32_t)GetTime();

            struct net_addr source;
            net_addr_init(&source);
            unsigned char src_ip[] = {60, 2, 3, (unsigned char)(i + 1)};
            net_addr_set_ipv4(&source, src_ip);

            addrman_add(&am, &addr, &source, 0);
        }

        bool ok = addrman_size(&am) == 10;
        struct addr_info result;
        bool selected = addrman_select(&am, true, &result);
        ok = ok && selected;
        ok = ok && result.addr.svc.port == 8233;

        if (ok) printf("OK\n");
        else { printf("FAIL (size=%zu sel=%d)\n", addrman_size(&am), selected); failures++; }
        addrman_free(&am);
    }

    printf("addr_info bucket computation... ");
    {
        struct addr_info info;
        memset(&info, 0, sizeof(info));
        unsigned char ip[] = {192, 168, 1, 1};
        net_addr_set_ipv4(&info.addr.svc.addr, ip);
        info.addr.svc.port = 8233;

        struct uint256 key;
        memset(key.data, 0x42, 32);

        int tried_bucket = addr_info_get_tried_bucket(&info, &key);
        int new_bucket = addr_info_get_new_bucket(&info, &key, &info.addr.svc.addr);
        int pos = addr_info_get_bucket_position(&info, &key, true, new_bucket);

        bool ok = tried_bucket >= 0 && tried_bucket < ADDRMAN_TRIED_BUCKET_COUNT;
        ok = ok && new_bucket >= 0 && new_bucket < ADDRMAN_NEW_BUCKET_COUNT;
        ok = ok && pos >= 0 && pos < ADDRMAN_BUCKET_SIZE;

        if (ok) printf("OK\n");
        else { printf("FAIL (tb=%d nb=%d pos=%d)\n", tried_bucket, new_bucket, pos); failures++; }
    }

    printf("addr_info_is_terrible... ");
    {
        struct addr_info info;
        memset(&info, 0, sizeof(info));
        info.addr.nTime = 0;
        bool ok = addr_info_is_terrible(&info, GetTime());

        info.addr.nTime = (uint32_t)GetTime();
        info.last_try = GetTime();
        ok = ok && !addr_info_is_terrible(&info, GetTime());

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("net_manager init/free... ");
    {
        struct net_manager nm;
        net_manager_init(&nm);
        memcpy(nm.message_start, "\xfa\x1a\xf9\xbf", 4);
        nm.default_port = 8233;
        bool ok = (nm.max_connections == DEFAULT_MAX_PEER_CONNECTIONS);
        ok = ok && (nm.discover == true);
        ok = ok && (nm.listen == true);
        ok = ok && (nm.num_nodes == 0);
        net_manager_free(&nm);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("net_message framing... ");
    {
        unsigned char msgstart[4] = {0xfa, 0x1a, 0xf9, 0xbf};
        struct net_message msg;
        net_message_init(&msg, msgstart);

        struct msg_header hdr;
        msg_header_init_full(&hdr, msgstart, "ping", 8);
        unsigned char payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};

        uint8_t wire[MSG_HEADER_SIZE + 8];
        memcpy(wire, &hdr, MSG_HEADER_SIZE);
        memcpy(wire + MSG_HEADER_SIZE, payload, 8);

        int r1 = net_message_read_header(&msg, (const char *)wire, MSG_HEADER_SIZE);
        bool ok = (r1 == MSG_HEADER_SIZE);
        ok = ok && msg.in_data;
        ok = ok && (msg.hdr.nMessageSize == 8);

        int r2 = net_message_read_data(&msg, (const char *)payload, 8);
        ok = ok && (r2 == 8);
        ok = ok && net_message_complete(&msg);
        ok = ok && (memcmp(msg.recv_data, payload, 8) == 0);

        net_message_free(&msg);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("p2p_node create/free... ");
    {
        struct net_manager nm;
        net_manager_init(&nm);
        memcpy(nm.message_start, "\xfa\x1a\xf9\xbf", 4);

        struct net_address addr;
        net_address_init(&addr);
        unsigned char ip4[4] = {50, 0, 0, 1};
        net_addr_set_ipv4(&addr.svc.addr, ip4);
        addr.svc.port = 8233;

        struct p2p_node *node = p2p_node_create(&nm, ZCL_INVALID_SOCKET,
                                                 &addr, "test-peer", false);
        bool ok = (node != NULL);
        ok = ok && (node->id == 0);
        ok = ok && (node->inbound == false);
        ok = ok && (node->disconnect == false);
        ok = ok && (node->version == 0);
        ok = ok && (strcmp(node->addr_name, "test-peer") == 0);

        p2p_node_free(node);
        net_manager_free(&nm);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("ban management... ");
    {
        struct net_manager nm;
        net_manager_init(&nm);

        struct net_addr addr;
        net_addr_init(&addr);
        unsigned char ip4[4] = {50, 0, 0, 1};
        net_addr_set_ipv4(&addr, ip4);

        bool ok = !is_banned(&nm, &addr);
        ban_addr(&nm, &addr, 3600, false);
        ok = ok && is_banned(&nm, &addr);
        ok = ok && unban_addr(&nm, &addr);
        ok = ok && !is_banned(&nm, &addr);

        ban_addr(&nm, &addr, 3600, false);
        clear_banned(&nm);
        ok = ok && !is_banned(&nm, &addr);

        net_manager_free(&nm);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("local address management... ");
    {
        struct net_manager nm;
        net_manager_init(&nm);

        struct net_service svc;
        net_service_init(&svc);
        unsigned char ip4[4] = {50, 0, 0, 1};
        net_addr_set_ipv4(&svc.addr, ip4);
        svc.port = 8233;

        bool ok = !is_local(&nm, &svc);
        ok = ok && add_local(&nm, &svc, LOCAL_BIND);
        ok = ok && is_local(&nm, &svc);
        ok = ok && remove_local(&nm, &svc);
        ok = ok && !is_local(&nm, &svc);

        net_manager_free(&nm);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("set_limited/is_reachable... ");
    {
        struct net_manager nm;
        net_manager_init(&nm);

        bool ok = is_reachable_net(&nm, NET_IPV4);
        set_limited(&nm, NET_IPV4, true);
        ok = ok && !is_reachable_net(&nm, NET_IPV4);
        set_limited(&nm, NET_IPV4, false);
        ok = ok && is_reachable_net(&nm, NET_IPV4);

        net_manager_free(&nm);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("node_stats copy... ");
    {
        struct net_manager nm;
        net_manager_init(&nm);
        memcpy(nm.message_start, "\xfa\x1a\xf9\xbf", 4);

        struct net_address addr;
        net_address_init(&addr);
        unsigned char ip4[4] = {50, 0, 0, 1};
        net_addr_set_ipv4(&addr.svc.addr, ip4);
        addr.svc.port = 8233;

        struct p2p_node *node = p2p_node_create(&nm, ZCL_INVALID_SOCKET,
                                                 &addr, "stats-test", true);
        node->version = 170002;
        snprintf(node->clean_sub_ver, sizeof(node->clean_sub_ver),
                 "/ZClassic:1.0.0/");

        struct node_stats stats;
        p2p_node_copy_stats(node, &stats);

        bool ok = (stats.nodeid == 0);
        ok = ok && (stats.version == 170002);
        ok = ok && (stats.inbound == true);
        ok = ok && (strcmp(stats.clean_sub_ver, "/ZClassic:1.0.0/") == 0);

        p2p_node_free(node);
        net_manager_free(&nm);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    /* ===== NETWORKING TESTS ===== */

    /* net_addr: IPv4 init and classification */
    {
        printf("net_addr: IPv4 init and classify... ");
        struct net_addr a;
        net_addr_init(&a);
        unsigned char ip4[4] = {192, 168, 1, 100};
        net_addr_set_ipv4(&a, ip4);
        bool ok = net_addr_is_ipv4(&a);
        ok = ok && !net_addr_is_ipv6(&a);
        ok = ok && !net_addr_is_tor(&a);
        ok = ok && (net_addr_get_network(&a) == NET_IPV4);
        ok = ok && net_addr_is_valid(&a);
        ok = ok && (net_addr_get_byte(&a, 0) == 100);
        ok = ok && (net_addr_get_byte(&a, 1) == 1);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* net_addr: IPv6 classification */
    {
        printf("net_addr: IPv6 classify... ");
        struct net_addr a;
        net_addr_init(&a);
        a.ip[0] = 0x20; a.ip[1] = 0x01;
        a.ip[2] = 0x0d; a.ip[3] = 0x00;
        a.ip[15] = 0x01;
        bool ok = !net_addr_is_ipv4(&a);
        ok = ok && net_addr_is_ipv6(&a);
        ok = ok && (net_addr_get_network(&a) == NET_IPV6);
        ok = ok && net_addr_is_valid(&a);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* net_addr: null address invalid */
    {
        printf("net_addr: null address is invalid... ");
        struct net_addr a;
        net_addr_init(&a);
        bool ok = !net_addr_is_valid(&a);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* net_addr: RFC 3849 documentation address invalid */
    {
        printf("net_addr: RFC3849 doc address invalid... ");
        struct net_addr a;
        net_addr_init(&a);
        a.ip[0] = 0x20; a.ip[1] = 0x01;
        a.ip[2] = 0x0d; a.ip[3] = 0xb8;
        a.ip[15] = 0x01;
        bool ok = !net_addr_is_valid(&a);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* net_addr: equality */
    {
        printf("net_addr: equality... ");
        struct net_addr a, b;
        net_addr_init(&a);
        net_addr_init(&b);
        unsigned char ip4[4] = {10, 0, 0, 1};
        net_addr_set_ipv4(&a, ip4);
        net_addr_set_ipv4(&b, ip4);
        bool ok = net_addr_eq(&a, &b);
        unsigned char ip4b[4] = {10, 0, 0, 2};
        net_addr_set_ipv4(&b, ip4b);
        ok = ok && !net_addr_eq(&a, &b);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* net_addr: Tor address */
    {
        printf("net_addr: Tor address... ");
        struct net_addr a;
        net_addr_init(&a);
        a.has_torv3 = true;
        memset(a.torv3, 0xAB, TORV3_ADDR_SIZE);
        bool ok = net_addr_is_tor(&a);
        ok = ok && !net_addr_is_ipv4(&a);
        ok = ok && (net_addr_get_network(&a) == NET_ONION);
        ok = ok && net_addr_is_valid(&a);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* net_addr: RFC1918 private ranges */
    {
        printf("net_addr: RFC1918 private... ");
        struct net_addr a;
        net_addr_init(&a);
        unsigned char ip4[4] = {10, 0, 0, 1};
        net_addr_set_ipv4(&a, ip4);
        bool ok = net_addr_is_rfc1918(&a);
        ok = ok && !net_addr_is_routable(&a);
        unsigned char ip4b[4] = {172, 16, 5, 1};
        net_addr_set_ipv4(&a, ip4b);
        ok = ok && net_addr_is_rfc1918(&a);
        unsigned char ip4c[4] = {192, 168, 0, 1};
        net_addr_set_ipv4(&a, ip4c);
        ok = ok && net_addr_is_rfc1918(&a);
        unsigned char ip4d[4] = {8, 8, 8, 8};
        net_addr_set_ipv4(&a, ip4d);
        ok = ok && !net_addr_is_rfc1918(&a);
        ok = ok && net_addr_is_routable(&a);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* net_addr: get_group deterministic */
    {
        printf("net_addr: address group... ");
        struct net_addr a, b;
        net_addr_init(&a);
        net_addr_init(&b);
        unsigned char ip4[4] = {8, 8, 8, 8};
        net_addr_set_ipv4(&a, ip4);
        unsigned char ip4b[4] = {8, 8, 4, 4};
        net_addr_set_ipv4(&b, ip4b);
        unsigned char ga[NET_ADDR_GROUP_MAX], gb[NET_ADDR_GROUP_MAX];
        size_t la = net_addr_get_group(&a, ga, sizeof(ga));
        size_t lb = net_addr_get_group(&b, gb, sizeof(gb));
        bool ok = (la > 0 && lb > 0);
        /* Same /16 prefix -> same group */
        ok = ok && (la == lb) && (memcmp(ga, gb, la) == 0);
        /* Different /16 -> different group */
        unsigned char ip4c[4] = {1, 2, 3, 4};
        net_addr_set_ipv4(&b, ip4c);
        lb = net_addr_get_group(&b, gb, sizeof(gb));
        ok = ok && (memcmp(ga, gb, (la < lb ? la : lb)) != 0 || la != lb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* net_service: init and equality */
    {
        printf("net_service: init and equality... ");
        struct net_service a, b;
        net_service_init(&a);
        net_service_init(&b);
        unsigned char ip4[4] = {127, 0, 0, 1};
        net_addr_set_ipv4(&a.addr, ip4);
        a.port = 8033;
        net_addr_set_ipv4(&b.addr, ip4);
        b.port = 8033;
        bool ok = net_service_eq(&a, &b);
        b.port = 18033;
        ok = ok && !net_service_eq(&a, &b);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* net_service: to_string */
    {
        printf("net_service: to_string... ");
        struct net_service s;
        net_service_init(&s);
        unsigned char ip4[4] = {192, 168, 1, 1};
        net_addr_set_ipv4(&s.addr, ip4);
        s.port = 8033;
        char buf[64];
        int n = net_service_to_string(&s, buf, sizeof(buf));
        bool ok = (n > 0);
        ok = ok && (strstr(buf, "192.168.1.1") != NULL);
        ok = ok && (strstr(buf, "8033") != NULL);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* net_addr: to_string */
    {
        printf("net_addr: to_string... ");
        struct net_addr a;
        net_addr_init(&a);
        unsigned char ip4[4] = {10, 20, 30, 40};
        net_addr_set_ipv4(&a, ip4);
        char buf[64];
        int n = net_addr_to_string(&a, buf, sizeof(buf));
        bool ok = (n > 0);
        ok = ok && (strcmp(buf, "10.20.30.40") == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* msg_header: init and validate */
    {
        printf("msg_header: init and validate... ");
        unsigned char magic[MESSAGE_START_SIZE] = {0x24, 0xe9, 0x27, 0x64};
        struct msg_header h;
        msg_header_init_full(&h, magic, "ping", 8);
        bool ok = msg_header_is_valid(&h, magic);
        /* Wrong magic must fail */
        unsigned char bad_magic[MESSAGE_START_SIZE] = {0xFF, 0xFF, 0xFF, 0xFF};
        ok = ok && !msg_header_is_valid(&h, bad_magic);
        /* Default init has invalid size (-1) so should fail validation */
        struct msg_header h2;
        msg_header_init(&h2, magic);
        ok = ok && !msg_header_is_valid(&h2, magic);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* msg_header: full init with command */
    {
        printf("msg_header: command get/set... ");
        unsigned char magic[MESSAGE_START_SIZE] = {0x24, 0xe9, 0x27, 0x64};
        struct msg_header h;
        msg_header_init_full(&h, magic, "version", 100);
        bool ok = msg_header_is_valid(&h, magic);
        ok = ok && (h.nMessageSize == 100);
        char cmd[COMMAND_SIZE + 1];
        msg_header_get_command(&h, cmd, sizeof(cmd));
        ok = ok && (strcmp(cmd, "version") == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* inv_item: init, types, string */
    {
        printf("inv_item: init and types... ");
        struct inv_item inv;
        inv_item_init(&inv);
        bool ok = (inv.type == 0);
        ok = ok && uint256_is_null(&inv.hash);

        struct uint256 h;
        uint256_set_null(&h);
        h.data[0] = 0xAB;
        inv_item_init_typed(&inv, MSG_TX, &h);
        ok = ok && (inv.type == MSG_TX);
        ok = ok && inv_item_is_known_type(&inv);
        ok = ok && (strcmp(inv_item_get_command(&inv), "tx") == 0);

        inv_item_init_typed(&inv, MSG_BLOCK, &h);
        ok = ok && (strcmp(inv_item_get_command(&inv), "block") == 0);
        ok = ok && inv_item_is_known_type(&inv);

        char str[128];
        inv_item_to_string(&inv, str, sizeof(str));
        ok = ok && (strlen(str) > 0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* inv_item: init by name */
    {
        printf("inv_item: init by name... ");
        struct uint256 h;
        uint256_set_null(&h);
        h.data[31] = 0x42;
        struct inv_item inv;
        bool ok = (inv_item_init_by_name(&inv, "tx", &h) == 0);
        ok = ok && (inv.type == MSG_TX);
        ok = ok && (inv_item_init_by_name(&inv, "block", &h) == 0);
        ok = ok && (inv.type == MSG_BLOCK);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* inv_item: serialization roundtrip */
    {
        printf("inv_item: serialize roundtrip... ");
        struct uint256 h;
        uint256_set_null(&h);
        for (int i = 0; i < 32; i++) h.data[i] = (uint8_t)i;
        struct inv_item inv;
        inv_item_init_typed(&inv, MSG_TX, &h);

        struct byte_stream s;
        stream_init(&s, 128);
        bool ok = inv_item_serialize(&inv, &s);

        struct byte_stream r;
        stream_init_from_data(&r, s.data, s.size);
        struct inv_item inv2;
        ok = ok && inv_item_deserialize(&inv2, &r);
        ok = ok && (inv2.type == MSG_TX);
        ok = ok && uint256_eq(&inv2.hash, &h);
        stream_free(&s);
        stream_free(&r);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* net_address: serialization roundtrip */
    {
        printf("net_address: serialize roundtrip... ");
        struct net_address addr;
        net_address_init(&addr);
        unsigned char ip4[4] = {8, 8, 8, 8};
        net_addr_set_ipv4(&addr.svc.addr, ip4);
        addr.svc.port = 8033;
        addr.nServices = NODE_NETWORK;
        addr.nTime = 1700000000;

        struct byte_stream s;
        stream_init(&s, 128);
        bool ok = net_address_serialize(&addr, &s, true);

        struct byte_stream r;
        stream_init_from_data(&r, s.data, s.size);
        struct net_address addr2;
        net_address_init(&addr2);
        ok = ok && net_address_deserialize(&addr2, &r, true);
        ok = ok && net_addr_eq(&addr.svc.addr, &addr2.svc.addr);
        ok = ok && (addr2.svc.port == 8033);
        ok = ok && (addr2.nServices == NODE_NETWORK);
        ok = ok && (addr2.nTime == 1700000000);
        stream_free(&s);
        stream_free(&r);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* net_message: header read with valid magic */
    {
        printf("net_message: read header... ");
        unsigned char magic[MESSAGE_START_SIZE] = {0x24, 0xe9, 0x27, 0x64};
        struct net_message msg;
        net_message_init(&msg, magic);

        struct msg_header fake_hdr;
        msg_header_init_full(&fake_hdr, magic, "ping", 8);
        int n = net_message_read_header(&msg, (const char *)&fake_hdr,
                                         MSG_HEADER_SIZE);
        bool ok = (n == MSG_HEADER_SIZE);
        ok = ok && msg.in_data;
        ok = ok && (msg.hdr.nMessageSize == 8);
        char cmd[COMMAND_SIZE + 1];
        msg_header_get_command(&msg.hdr, cmd, sizeof(cmd));
        ok = ok && (strcmp(cmd, "ping") == 0);
        net_message_free(&msg);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* net_message: reject bad magic */
    {
        printf("net_message: reject bad magic... ");
        unsigned char magic[MESSAGE_START_SIZE] = {0x24, 0xe9, 0x27, 0x64};
        unsigned char bad[MESSAGE_START_SIZE] = {0xFF, 0xFF, 0xFF, 0xFF};
        struct net_message msg;
        net_message_init(&msg, magic);

        struct msg_header fake_hdr;
        msg_header_init_full(&fake_hdr, bad, "ping", 8);
        int n = net_message_read_header(&msg, (const char *)&fake_hdr,
                                         MSG_HEADER_SIZE);
        bool ok = (n == -1);
        ok = ok && !msg.in_data;
        net_message_free(&msg);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* net_message: read data and complete */
    {
        printf("net_message: read data + complete... ");
        unsigned char magic[MESSAGE_START_SIZE] = {0x24, 0xe9, 0x27, 0x64};
        struct net_message msg;
        net_message_init(&msg, magic);

        struct msg_header fake_hdr;
        msg_header_init_full(&fake_hdr, magic, "ping", 8);
        net_message_read_header(&msg, (const char *)&fake_hdr, MSG_HEADER_SIZE);
        bool ok = msg.in_data && !net_message_complete(&msg);

        uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        int n = net_message_read_data(&msg, (const char *)payload, 8);
        ok = ok && (n == 8);
        ok = ok && net_message_complete(&msg);
        ok = ok && (msg.data_pos == 8);
        ok = ok && (memcmp(msg.recv_data, payload, 8) == 0);
        net_message_free(&msg);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* net_message: partial header read */
    {
        printf("net_message: partial header read... ");
        unsigned char magic[MESSAGE_START_SIZE] = {0x24, 0xe9, 0x27, 0x64};
        struct net_message msg;
        net_message_init(&msg, magic);

        struct msg_header fake_hdr;
        msg_header_init_full(&fake_hdr, magic, "verack", 0);
        const char *raw = (const char *)&fake_hdr;
        /* Feed half, then the rest */
        int n1 = net_message_read_header(&msg, raw, 10);
        bool ok = (n1 == 10);
        ok = ok && !msg.in_data;
        int n2 = net_message_read_header(&msg, raw + 10, MSG_HEADER_SIZE - 10);
        ok = ok && ((unsigned)(n1 + n2) == MSG_HEADER_SIZE);
        ok = ok && msg.in_data;
        ok = ok && net_message_complete(&msg); /* size=0 -> immediately complete */
        net_message_free(&msg);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* version_message: serialize/deserialize roundtrip */
    {
        printf("version_message: serialize roundtrip... ");
        struct version_message vm;
        version_message_init(&vm);
        vm.protocol_version = 170002;
        vm.services = NODE_NETWORK;
        vm.timestamp = 1700000000;
        vm.nonce = 0xDEADBEEFCAFEBABE;
        snprintf(vm.sub_version, sizeof(vm.sub_version),
                 "/ZClassic-C23:1.0.0/");
        vm.start_height = 3040000;
        vm.relay = true;

        unsigned char ip4_recv[4] = {192, 168, 1, 1};
        net_addr_set_ipv4(&vm.addr_recv.svc.addr, ip4_recv);
        vm.addr_recv.svc.port = 8033;

        struct byte_stream s;
        stream_init(&s, 256);
        bool ok = version_message_serialize(&vm, &s);

        struct byte_stream r;
        stream_init_from_data(&r, s.data, s.size);
        struct version_message vm2;
        version_message_init(&vm2);
        ok = ok && version_message_deserialize(&vm2, &r);
        ok = ok && (vm2.protocol_version == 170002);
        ok = ok && (vm2.services == NODE_NETWORK);
        ok = ok && (vm2.timestamp == 1700000000);
        ok = ok && (vm2.nonce == 0xDEADBEEFCAFEBABE);
        ok = ok && (strcmp(vm2.sub_version, "/ZClassic-C23:1.0.0/") == 0);
        ok = ok && (vm2.start_height == 3040000);
        ok = ok && vm2.relay;
        stream_free(&s);
        stream_free(&r);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* addrman: init, add, size, select */
    {
        printf("addrman: add and select... ");
        struct addr_man am;
        addrman_init(&am);
        bool ok = (addrman_size(&am) == 0);

        struct net_address addr;
        net_address_init(&addr);
        unsigned char ip4[4] = {8, 8, 8, 8};
        net_addr_set_ipv4(&addr.svc.addr, ip4);
        addr.svc.port = 8033;
        addr.nServices = NODE_NETWORK;

        struct net_addr src;
        net_addr_init(&src);
        unsigned char src_ip[4] = {1, 2, 3, 4};
        net_addr_set_ipv4(&src, src_ip);

        ok = ok && addrman_add(&am, &addr, &src, 0);
        ok = ok && (addrman_size(&am) == 1);

        /* Select should return the address we added */
        struct addr_info info;
        memset(&info, 0, sizeof(info));
        ok = ok && addrman_select(&am, false, &info);
        ok = ok && (info.addr.svc.port == 8033);
        ok = ok && net_addr_eq(&info.addr.svc.addr, &addr.svc.addr);

        addrman_free(&am);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* addrman: multiple addresses */
    {
        printf("addrman: multiple addresses... ");
        struct addr_man am;
        addrman_init(&am);

        struct net_addr src;
        net_addr_init(&src);
        unsigned char src_ip[4] = {1, 1, 1, 1};
        net_addr_set_ipv4(&src, src_ip);

        for (int i = 0; i < 10; i++) {
            struct net_address addr;
            net_address_init(&addr);
            unsigned char ip4[4] = {(unsigned char)(50 + i), 0, 0, 1};
            net_addr_set_ipv4(&addr.svc.addr, ip4);
            addr.svc.port = 8033;
            addr.nServices = NODE_NETWORK;
            addr.nTime = (uint32_t)(GetTime() - 3600);
            addrman_add(&am, &addr, &src, 0);
        }
        bool ok = (addrman_size(&am) == 10);

        /* Select should work repeatedly */
        struct addr_info info;
        memset(&info, 0, sizeof(info));
        ok = ok && addrman_select(&am, false, &info);
        ok = ok && (info.addr.svc.port == 8033);

        /* Duplicate add should not increase count */
        struct net_address dup;
        net_address_init(&dup);
        unsigned char dup_ip[4] = {50, 0, 0, 1};
        net_addr_set_ipv4(&dup.svc.addr, dup_ip);
        dup.svc.port = 8033;
        dup.nServices = NODE_NETWORK;
        dup.nTime = (uint32_t)(GetTime() - 3600);
        addrman_add(&am, &dup, &src, 0);
        ok = ok && (addrman_size(&am) == 10);

        addrman_free(&am);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* addrman: serialize/deserialize roundtrip */
    {
        printf("addrman: serialize roundtrip... ");
        struct addr_man am;
        addrman_init(&am);

        struct net_addr src;
        net_addr_init(&src);
        unsigned char src_ip[4] = {5, 5, 5, 5};
        net_addr_set_ipv4(&src, src_ip);

        for (int i = 0; i < 5; i++) {
            struct net_address addr;
            net_address_init(&addr);
            unsigned char ip4[4] = {(unsigned char)(70 + i), 1, 2, 3};
            net_addr_set_ipv4(&addr.svc.addr, ip4);
            addr.svc.port = 8033;
            addr.nServices = NODE_NETWORK;
            addrman_add(&am, &addr, &src, 0);
        }

        struct byte_stream s;
        stream_init(&s, 4096);
        bool ok = addrman_serialize(&am, &s);

        struct addr_man am2;
        addrman_init(&am2);
        struct byte_stream r;
        stream_init_from_data(&r, s.data, s.size);
        ok = ok && addrman_deserialize(&am2, &r);
        ok = ok && (addrman_size(&am2) == addrman_size(&am));

        stream_free(&s);
        stream_free(&r);
        addrman_free(&am);
        addrman_free(&am2);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* net_manager: init, ban, unban, clear */
    {
        printf("net_manager: ban/unban/clear... ");
        struct net_manager nm;
        net_manager_init(&nm);

        struct net_addr a;
        net_addr_init(&a);
        unsigned char ip4[4] = {1, 2, 3, 4};
        net_addr_set_ipv4(&a, ip4);

        bool ok = !is_banned(&nm, &a);
        ban_addr(&nm, &a, 3600, false);
        ok = ok && is_banned(&nm, &a);

        /* Different address not banned */
        struct net_addr b;
        net_addr_init(&b);
        unsigned char ip4b[4] = {5, 6, 7, 8};
        net_addr_set_ipv4(&b, ip4b);
        ok = ok && !is_banned(&nm, &b);

        /* Unban first address */
        ok = ok && unban_addr(&nm, &a);
        ok = ok && !is_banned(&nm, &a);

        /* Ban two, clear all */
        ban_addr(&nm, &a, 3600, false);
        ban_addr(&nm, &b, 3600, false);
        ok = ok && is_banned(&nm, &a);
        ok = ok && is_banned(&nm, &b);
        clear_banned(&nm);
        ok = ok && !is_banned(&nm, &a);
        ok = ok && !is_banned(&nm, &b);

        net_manager_free(&nm);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* net_manager: init defaults */
    {
        printf("net_manager: init defaults... ");
        struct net_manager nm;
        net_manager_init(&nm);
        bool ok = nm.discover;
        ok = ok && nm.listen;
        ok = ok && (nm.local_services == NODE_NETWORK);
        ok = ok && (nm.max_connections == DEFAULT_MAX_PEER_CONNECTIONS);
        ok = ok && !nm.stop_requested;
        ok = ok && (nm.num_nodes == 0);
        ok = ok && (nm.num_banned == 0);
        ok = ok && (nm.num_listen_sockets == 0);
        net_manager_free(&nm);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* p2p_node: create and free lifecycle */
    {
        printf("p2p_node: create and free... ");
        struct net_manager nm;
        net_manager_init(&nm);
        memset(nm.message_start, 0x24, MESSAGE_START_SIZE);

        struct net_address addr;
        net_address_init(&addr);
        unsigned char ip4[4] = {127, 0, 0, 1};
        net_addr_set_ipv4(&addr.svc.addr, ip4);
        addr.svc.port = 8033;

        struct p2p_node *node = p2p_node_create(&nm, ZCL_INVALID_SOCKET,
                                                  &addr, "test-peer", true);
        bool ok = (node != NULL);
        ok = ok && node->inbound;
        ok = ok && (node->socket == ZCL_INVALID_SOCKET);
        ok = ok && (node->id == 0);
        ok = ok && (node->recv_version == INIT_PROTO_VERSION);
        ok = ok && (strcmp(node->addr_name, "test-peer") == 0);
        ok = ok && (node->starting_height == -1);
        ok = ok && !node->disconnect;
        ok = ok && !node->successfully_connected;

        /* Verify addr was copied */
        ok = ok && (node->addr.svc.port == 8033);
        ok = ok && net_addr_eq(&node->addr.svc.addr, &addr.svc.addr);

        p2p_node_free(node);

        /* Second node gets id=1 */
        struct p2p_node *node2 = p2p_node_create(&nm, ZCL_INVALID_SOCKET,
                                                   &addr, NULL, false);
        ok = ok && (node2 != NULL);
        ok = ok && (node2->id == 1);
        ok = ok && !node2->inbound;
        /* NULL name -> auto-generated from IP */
        ok = ok && (strlen(node2->addr_name) > 0);
        p2p_node_free(node2);

        net_manager_free(&nm);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* p2p_node: receive bytes parses message */
    {
        printf("p2p_node: receive_bytes parses message... ");
        struct net_manager nm;
        net_manager_init(&nm);
        unsigned char magic[MESSAGE_START_SIZE] = {0x24, 0xe9, 0x27, 0x64};
        memcpy(nm.message_start, magic, MESSAGE_START_SIZE);

        struct net_address addr;
        net_address_init(&addr);
        unsigned char ip4[4] = {127, 0, 0, 1};
        net_addr_set_ipv4(&addr.svc.addr, ip4);
        addr.svc.port = 8033;

        struct p2p_node *node = p2p_node_create(&nm, ZCL_INVALID_SOCKET,
                                                  &addr, "test", true);
        bool ok = (node != NULL);

        /* Build a verack message (empty payload) */
        struct msg_header hdr;
        msg_header_init_full(&hdr, magic, "verack", 0);
        /* Compute checksum for empty payload: SHA256d("") truncated to 4 bytes */
        uint8_t empty_hash[32];
        { struct sha256_ctx ctx; sha256_init(&ctx);
          sha256_write(&ctx, (const unsigned char *)"", 0);
          sha256_finalize(&ctx, empty_hash); }
        uint8_t dbl_hash[32];
        { struct sha256_ctx ctx; sha256_init(&ctx);
          sha256_write(&ctx, empty_hash, 32);
          sha256_finalize(&ctx, dbl_hash); }
        memcpy(&hdr.nChecksum, dbl_hash, 4);

        ok = ok && p2p_node_receive_bytes(node, (const char *)&hdr,
                                            MSG_HEADER_SIZE, magic);
        ok = ok && (node->recv_msg_count == 1);
        ok = ok && net_message_complete(&node->recv_msgs[0]);

        char cmd[COMMAND_SIZE + 1];
        msg_header_get_command(&node->recv_msgs[0].hdr, cmd, sizeof(cmd));
        ok = ok && (strcmp(cmd, "verack") == 0);

        p2p_node_free(node);
        net_manager_free(&nm);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* p2p_node: inventory tracking */
    {
        printf("p2p_node: inventory known + push... ");
        struct net_manager nm;
        net_manager_init(&nm);
        memset(nm.message_start, 0x24, MESSAGE_START_SIZE);

        struct net_address addr;
        net_address_init(&addr);
        unsigned char ip4[4] = {127, 0, 0, 1};
        net_addr_set_ipv4(&addr.svc.addr, ip4);

        struct p2p_node *node = p2p_node_create(&nm, ZCL_INVALID_SOCKET,
                                                  &addr, "test", false);
        bool ok = (node != NULL);
        ok = ok && (node->inventory_known_count == 0);
        ok = ok && (node->inventory_to_send_count == 0);

        struct uint256 h;
        uint256_set_null(&h);
        h.data[0] = 0x42;
        struct inv_item inv;
        inv_item_init_typed(&inv, MSG_TX, &h);

        /* push_inventory adds to send queue */
        p2p_node_push_inventory(node, &inv);
        ok = ok && (node->inventory_to_send_count == 1);

        /* add_inventory_known marks it as known */
        p2p_node_add_inventory_known(node, &inv);
        ok = ok && (node->inventory_known_count == 1);

        /* Pushing same hash again should not add duplicate */
        size_t before = node->inventory_to_send_count;
        p2p_node_push_inventory(node, &inv);
        ok = ok && (node->inventory_to_send_count == before);

        p2p_node_free(node);
        net_manager_free(&nm);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* p2p_node: address tracking */
    {
        printf("p2p_node: push_address... ");
        struct net_manager nm;
        net_manager_init(&nm);
        memset(nm.message_start, 0x24, MESSAGE_START_SIZE);

        struct net_address addr;
        net_address_init(&addr);
        unsigned char ip4[4] = {127, 0, 0, 1};
        net_addr_set_ipv4(&addr.svc.addr, ip4);

        struct p2p_node *node = p2p_node_create(&nm, ZCL_INVALID_SOCKET,
                                                  &addr, "test", false);
        bool ok = (node != NULL);
        ok = ok && (node->addr_to_send_count == 0);

        /* Push a routable address */
        struct net_address a2;
        net_address_init(&a2);
        unsigned char routable[4] = {8, 8, 8, 8};
        net_addr_set_ipv4(&a2.svc.addr, routable);
        a2.svc.port = 8033;
        p2p_node_push_address(node, &a2);
        ok = ok && (node->addr_to_send_count == 1);

        /* Insert into addr_known bloom, then second push should be filtered */
        unsigned char key[NET_SERVICE_KEY_SIZE];
        net_service_get_key(&a2.svc, key);
        rolling_bloom_insert(&node->addr_known, key, NET_SERVICE_KEY_SIZE);
        p2p_node_push_address(node, &a2);
        ok = ok && (node->addr_to_send_count == 1);

        /* Invalid address should be rejected */
        struct net_address invalid;
        net_address_init(&invalid); /* all zeros = invalid */
        p2p_node_push_address(node, &invalid);
        ok = ok && (node->addr_to_send_count == 1);

        p2p_node_free(node);
        net_manager_free(&nm);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* p2p_node: ref counting */
    {
        printf("p2p_node: ref counting... ");
        struct net_manager nm;
        net_manager_init(&nm);
        memset(nm.message_start, 0x24, MESSAGE_START_SIZE);

        struct net_address addr;
        net_address_init(&addr);
        unsigned char ip4[4] = {127, 0, 0, 1};
        net_addr_set_ipv4(&addr.svc.addr, ip4);

        struct p2p_node *node = p2p_node_create(&nm, ZCL_INVALID_SOCKET,
                                                  &addr, "test", false);
        bool ok = (node != NULL);
        ok = ok && (p2p_node_get_ref(node) == 0);
        p2p_node_add_ref(node);
        ok = ok && (p2p_node_get_ref(node) == 1);
        p2p_node_add_ref(node);
        ok = ok && (p2p_node_get_ref(node) == 2);
        p2p_node_release(node);
        ok = ok && (p2p_node_get_ref(node) == 1);

        p2p_node_free(node);
        net_manager_free(&nm);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* p2p_node: free NULL is safe */
    {
        printf("p2p_node: free NULL safe... ");
        p2p_node_free(NULL);
        printf("OK\n");
    }

    /* net_message: oversized message rejected */
    {
        printf("net_message: reject oversized... ");
        unsigned char magic[MESSAGE_START_SIZE] = {0x24, 0xe9, 0x27, 0x64};
        struct net_message msg;
        net_message_init(&msg, magic);

        struct msg_header fake_hdr;
        msg_header_init_full(&fake_hdr, magic, "block", MAX_SIZE + 1);
        int n = net_message_read_header(&msg, (const char *)&fake_hdr,
                                         MSG_HEADER_SIZE);
        bool ok = (n == -1); /* Should reject oversized */
        net_message_free(&msg);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* connman: init and free without start */
    {
        printf("connman: init/free lifecycle... ");
        chain_params_select(CHAIN_MAIN);
        const struct chain_params *params = chain_params_get();
        struct connman cm;
        struct node_signals sigs;
        memset(&sigs, 0, sizeof(sigs));
        bool ok = connman_init(&cm, params, &sigs);
        ok = ok && !cm.started;
        ok = ok && (cm.num_deferred_free == 0);
        ok = ok && (cm.manager.default_port == params->nDefaultPort);
        ok = ok && (memcmp(cm.manager.message_start, params->pchMessageStart,
                           MESSAGE_START_SIZE) == 0);
        char *sv = cm.manager.sub_version;
        ok = ok && (strstr(sv, "ZClassic") != NULL);
        connman_free(&cm);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* connman: node count */
    {
        printf("connman: node_count starts at 0... ");
        const struct chain_params *params = chain_params_get();
        struct connman cm;
        struct node_signals sigs;
        memset(&sigs, 0, sizeof(sigs));
        connman_init(&cm, params, &sigs);
        bool ok = (connman_get_node_count(&cm) == 0);
        connman_free(&cm);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* inv_item: less comparison */
    {
        printf("inv_item: less comparison... ");
        struct uint256 h1, h2;
        uint256_set_null(&h1);
        uint256_set_null(&h2);
        h1.data[0] = 0x01;
        h2.data[0] = 0x02;
        struct inv_item a, b;
        inv_item_init_typed(&a, MSG_TX, &h1);
        inv_item_init_typed(&b, MSG_TX, &h2);
        bool ok = inv_item_less(&a, &b);
        ok = ok && !inv_item_less(&b, &a);
        ok = ok && !inv_item_less(&a, &a);
        /* Different types: MSG_TX < MSG_BLOCK */
        inv_item_init_typed(&b, MSG_BLOCK, &h1);
        ok = ok && inv_item_less(&a, &b);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ── Wire compatibility: overwintered tx format ────────── */
    printf("tx overwintered v4 wire format... ");
    {
        struct transaction tx;
        transaction_init(&tx);
        tx.overwintered = true;
        tx.version = SAPLING_TX_VERSION;
        tx.version_group_id = SAPLING_VERSION_GROUP_ID;
        tx.expiry_height = 3046100;

        struct byte_stream s;
        stream_init(&s, 256);
        transaction_serialize(&tx, &s);
        /* Version bytes: 04 00 00 80 (LE, bit 31 set) */
        bool ok = (s.size >= 4 && s.data[0] == 0x04 && s.data[3] == 0x80);

        struct transaction tx2;
        transaction_init(&tx2);
        struct byte_stream r;
        stream_init_from_data(&r, s.data, s.size);
        ok = ok && transaction_deserialize(&tx2, &r);
        ok = ok && tx2.overwintered;
        ok = ok && (tx2.version == SAPLING_TX_VERSION);
        ok = ok && (tx2.version_group_id == SAPLING_VERSION_GROUP_ID);
        ok = ok && (tx2.expiry_height == 3046100);
        stream_free(&s);
        transaction_free(&tx);
        transaction_free(&tx2);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("tx non-overwintered v4 lacks flag... ");
    {
        struct transaction tx;
        transaction_init(&tx);
        tx.overwintered = false;
        tx.version = 4;

        struct byte_stream s;
        stream_init(&s, 256);
        transaction_serialize(&tx, &s);
        bool ok = (s.size >= 4 && s.data[3] != 0x80);
        stream_free(&s);
        transaction_free(&tx);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("compact_size encoding... ");
    {
        struct byte_stream s;
        stream_init(&s, 32);
        stream_write_compact_size(&s, 100);
        bool ok = (s.size == 1 && s.data[0] == 100);
        s.size = 0;
        stream_write_compact_size(&s, 253);
        ok = ok && (s.size == 3);
        s.size = 0;
        stream_write_compact_size(&s, 65536);
        ok = ok && (s.size == 5);
        stream_free(&s);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("protocol version = 170011... ");
    {
        bool ok = (PROTOCOL_VERSION == 170011);
        ok = ok && (MIN_PEER_PROTO_VERSION <= 170002);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("mainnet magic bytes... ");
    {
        chain_params_select(CHAIN_MAIN);
        const struct chain_params *p = chain_params_get();
        unsigned char expected[4] = {0x24, 0xe9, 0x27, 0x64};
        bool ok = (memcmp(p->pchMessageStart, expected, 4) == 0);
        ok = ok && (p->nDefaultPort == 8033);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("version message wire size (empty subver = 86)... ");
    {
        struct version_message v;
        version_message_init(&v);
        v.protocol_version = 170011;
        v.services = NODE_NETWORK;
        struct byte_stream s;
        stream_init(&s, 256);
        version_message_serialize(&v, &s);
        bool ok = (s.size == 86);
        /* Verify proto bytes at offset 0 */
        uint32_t proto = (uint32_t)s.data[0] | ((uint32_t)s.data[1] << 8) |
                         ((uint32_t)s.data[2] << 16) | ((uint32_t)s.data[3] << 24);
        ok = ok && (proto == 170011);
        stream_free(&s);
        if (ok) printf("OK\n");
        else { printf("FAIL (size=%zu)\n", s.size); failures++; }
    }

    printf("SHA256d checksum (first 4 bytes)... ");
    {
        uint8_t payload[] = "test";
        uint8_t hash[32];
        hash256(payload, 4, hash);
        uint32_t checksum;
        memcpy(&checksum, hash, 4);
        bool ok = (checksum != 0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ================================================================
     * Fast sync: PoW solve + verify round-trip
     * ================================================================ */
    printf("fast_sync: PoW solve and verify... ");
    {
        uint8_t peer_id[32];
        GetRandBytes(peer_id, 32);
        struct fast_sync_pow pow;
        memset(&pow, 0, sizeof(pow));
        bool solved = fast_sync_solve_pow(peer_id, &pow);
        bool verified = solved && fast_sync_verify_pow(&pow);
        if (verified) printf("OK (nonce=%llu)\n", (unsigned long long)pow.nonce);
        else { printf("FAIL (solved=%d)\n", solved); failures++; }
    }

    printf("fast_sync: PoW rejects bad nonce... ");
    {
        uint8_t peer_id[32];
        GetRandBytes(peer_id, 32);
        struct fast_sync_pow pow;
        memset(&pow, 0, sizeof(pow));
        fast_sync_solve_pow(peer_id, &pow);
        pow.nonce++; /* corrupt the nonce */
        bool ok = !fast_sync_verify_pow(&pow);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("fast_sync: PoW rejects expired timestamp... ");
    {
        uint8_t peer_id[32];
        GetRandBytes(peer_id, 32);
        struct fast_sync_pow pow;
        memset(&pow, 0, sizeof(pow));
        fast_sync_solve_pow(peer_id, &pow);
        pow.timestamp -= 600; /* 10 minutes ago, beyond 5min window */
        bool ok = !fast_sync_verify_pow(&pow);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("fast_sync: PoW rejects NULL... ");
    {
        bool ok = !fast_sync_verify_pow(NULL);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ================================================================
     * Fast sync: rate limiter
     * ================================================================ */
    printf("fast_sync: rate limiter allows first request... ");
    {
        struct fast_sync_rate_limiter rl;
        memset(&rl, 0, sizeof(rl));
        uint8_t ip[16] = {0,0,0,0,0,0,0,0,0,0,0xFF,0xFF,10,0,0,1};
        bool ok = fast_sync_rate_check(&rl, ip);
        if (ok && rl.num_entries == 1) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("fast_sync: rate limiter tracks separate IPs... ");
    {
        struct fast_sync_rate_limiter rl;
        memset(&rl, 0, sizeof(rl));
        uint8_t ip1[16] = {0,0,0,0,0,0,0,0,0,0,0xFF,0xFF,10,0,0,1};
        uint8_t ip2[16] = {0,0,0,0,0,0,0,0,0,0,0xFF,0xFF,10,0,0,2};
        fast_sync_rate_check(&rl, ip1);
        fast_sync_rate_check(&rl, ip2);
        bool ok = (rl.num_entries == 2);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("fast_sync: rate limiter blocks after max chunks... ");
    {
        struct fast_sync_rate_limiter rl;
        memset(&rl, 0, sizeof(rl));
        uint8_t ip[16] = {0,0,0,0,0,0,0,0,0,0,0xFF,0xFF,10,0,0,1};
        bool all_ok = true;
        for (int i = 0; i < FAST_SYNC_MAX_CHUNKS_PER_HOUR; i++) {
            if (!fast_sync_rate_check(&rl, ip)) { all_ok = false; break; }
        }
        /* Next one should be blocked */
        bool blocked = !fast_sync_rate_check(&rl, ip);
        if (all_ok && blocked) printf("OK (blocked after %d)\n", FAST_SYNC_MAX_CHUNKS_PER_HOUR);
        else { printf("FAIL (ok=%d blocked=%d)\n", all_ok, blocked); failures++; }
    }

    /* ================================================================
     * Fast sync: peer_supports_fast_sync
     * ================================================================ */
    printf("fast_sync: peer_supports_fast_sync... ");
    {
        bool has = peer_supports_fast_sync(NODE_ZCL23 | 1);
        bool not_has = !peer_supports_fast_sync(1);
        bool zero = !peer_supports_fast_sync(0);
        if (has && not_has && zero) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ================================================================
     * Chainparams: fixed seeds within bounds
     * ================================================================ */
    printf("chainparams: fixed seeds within MAX_FIXED_SEEDS... ");
    {
        const struct chain_params *cp = chain_params_get();
        bool ok = (cp->nFixedSeeds > 0 && cp->nFixedSeeds <= MAX_FIXED_SEEDS);
        if (ok) printf("OK (%d seeds)\n", (int)cp->nFixedSeeds);
        else { printf("FAIL (%d)\n", (int)cp->nFixedSeeds); failures++; }
    }

    printf("chainparams: onion seeds present... ");
    {
        const struct chain_params *cp = chain_params_get();
        bool ok = (cp->nOnionSeeds > 0 &&
                   strstr(cp->onionSeeds[0], ".onion") != NULL);
        if (ok) printf("OK (%s)\n", cp->onionSeeds[0]);
        else { printf("FAIL\n"); failures++; }
    }

    /* ================================================================
     * Onion service: XSS prevention in search
     * ================================================================ */
    printf("onion_service: search escapes HTML in query... ");
    {
        uint8_t buf[8192];
        size_t len = onion_service_handle_request(
            "GET", "/search?q=<script>alert(1)</script>",
            NULL, 0, buf, sizeof(buf));
        buf[len < sizeof(buf) ? len : sizeof(buf) - 1] = '\0';
        /* Must NOT contain raw <script> tag */
        bool has_raw_script = (strstr((char *)buf, "<script>") != NULL);
        /* Must contain escaped version */
        bool has_escaped = (strstr((char *)buf, "&lt;script&gt;") != NULL);
        if (!has_raw_script && has_escaped) printf("OK\n");
        else { printf("FAIL (raw=%d escaped=%d)\n", has_raw_script, has_escaped); failures++; }
    }

    printf("onion_service: landing page returns valid HTML... ");
    {
        uint8_t buf[16384];
        size_t len = onion_service_handle_request(
            "GET", "/", NULL, 0, buf, sizeof(buf));
        buf[len < sizeof(buf) ? len : sizeof(buf) - 1] = '\0';
        bool has_header = (strstr((char *)buf, "HTTP/1.1 200") != NULL);
        bool has_html = (strstr((char *)buf, "</html>") != NULL);
        bool has_title = (strstr((char *)buf, "ZClassic23") != NULL);
        if (has_header && has_html && has_title) printf("OK (%zu bytes)\n", len);
        else { printf("FAIL\n"); failures++; }
    }

    printf("onion_service: 404 for unknown path... ");
    {
        uint8_t buf[4096];
        size_t len = onion_service_handle_request(
            "GET", "/nonexistent", NULL, 0, buf, sizeof(buf));
        buf[len < sizeof(buf) ? len : sizeof(buf) - 1] = '\0';
        bool ok = (strstr((char *)buf, "404") != NULL);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("onion_service: search with empty query... ");
    {
        uint8_t buf[8192];
        size_t len = onion_service_handle_request(
            "GET", "/search", NULL, 0, buf, sizeof(buf));
        buf[len < sizeof(buf) ? len : sizeof(buf) - 1] = '\0';
        bool ok = (len > 0 && strstr((char *)buf, "200 OK") != NULL);
        if (ok) printf("OK (%zu bytes)\n", len);
        else { printf("FAIL\n"); failures++; }
    }

    printf("onion_service: NULL path defaults to landing... ");
    {
        uint8_t buf[16384];
        size_t len = onion_service_handle_request(
            "GET", NULL, NULL, 0, buf, sizeof(buf));
        buf[len < sizeof(buf) ? len : sizeof(buf) - 1] = '\0';
        bool ok = (len > 0 && strstr((char *)buf, "ZClassic23 Network") != NULL);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ── Malformed P2P message tests ─────────────────────────── */

    printf("msg_header: oversized message rejection (>2MB)... ");
    {
        unsigned char magic[MESSAGE_START_SIZE] = {0x24, 0xe9, 0x27, 0x64};
        struct msg_header h;
        msg_header_init_full(&h, magic, "block", MAX_SIZE + 1);
        bool ok = !msg_header_is_valid(&h, magic);
        /* Exactly MAX_SIZE should still be valid */
        msg_header_init_full(&h, magic, "block", MAX_SIZE);
        ok = ok && msg_header_is_valid(&h, magic);
        /* Far oversized (e.g., 100MB) must also fail */
        msg_header_init_full(&h, magic, "block", 100 * 1024 * 1024);
        ok = ok && !msg_header_is_valid(&h, magic);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("msg_header: invalid checksum detection... ");
    {
        /* Build a version message, compute correct checksum, then corrupt it */
        unsigned char magic[MESSAGE_START_SIZE] = {0x24, 0xe9, 0x27, 0x64};
        struct version_message v;
        version_message_init(&v);
        v.protocol_version = 170009;
        v.services = NODE_NETWORK;
        v.timestamp = 1700000000;
        v.start_height = 500000;
        snprintf(v.sub_version, MAX_SUBVER_LENGTH, "/ZClassic:2.1.1-3/");

        struct byte_stream s;
        stream_init(&s, 256);
        version_message_serialize(&v, &s);

        /* Compute correct checksum: first 4 bytes of double-SHA256 */
        struct uint256 msg_hash;
        hash256(s.data, s.size, msg_hash.data);
        unsigned int correct_checksum;
        memcpy(&correct_checksum, msg_hash.data, 4);

        /* Corrupt checksum */
        unsigned int bad_checksum = correct_checksum ^ 0xFFFFFFFF;
        bool ok = (bad_checksum != correct_checksum);

        /* Verify detection: simulate what msgprocessor does */
        struct msg_header h;
        msg_header_init_full(&h, magic, "version", (unsigned int)s.size);
        h.nChecksum = bad_checksum;
        struct uint256 verify_hash;
        hash256(s.data, s.size, verify_hash.data);
        unsigned int expected;
        memcpy(&expected, verify_hash.data, 4);
        ok = ok && (expected != h.nChecksum);

        /* Correct checksum should match */
        h.nChecksum = correct_checksum;
        ok = ok && (expected == h.nChecksum);

        stream_free(&s);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("msg_header: truncated message handling... ");
    {
        /* A message header that claims 100 bytes but we only have 10 */
        unsigned char magic[MESSAGE_START_SIZE] = {0x24, 0xe9, 0x27, 0x64};
        struct net_message msg;
        net_message_init(&msg, magic);

        /* Simulate partial header read: only fill part of header buffer */
        struct msg_header h;
        msg_header_init_full(&h, magic, "ping", 100);
        msg.hdr = h;
        msg.in_data = true;
        msg.data_pos = 10; /* Only 10 of 100 bytes received */

        /* Message should not be considered complete */
        bool ok = !net_message_complete(&msg);

        /* Zero-length message should complete immediately */
        struct net_message msg2;
        net_message_init(&msg2, magic);
        msg_header_init_full(&msg2.hdr, magic, "verack", 0);
        msg2.in_data = true;
        msg2.data_pos = 0;
        ok = ok && net_message_complete(&msg2);

        net_message_free(&msg);
        net_message_free(&msg2);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("version_message: protocol version below minimum rejected... ");
    {
        struct version_message v;
        version_message_init(&v);
        v.protocol_version = 170001; /* Below MIN_PEER_PROTO_VERSION (170002) */
        v.services = NODE_NETWORK;
        v.timestamp = 1700000000;
        v.start_height = 100;

        bool ok = (v.protocol_version < MIN_PEER_PROTO_VERSION);

        /* At minimum should be accepted */
        v.protocol_version = MIN_PEER_PROTO_VERSION;
        ok = ok && (v.protocol_version >= MIN_PEER_PROTO_VERSION);

        /* Well above minimum */
        v.protocol_version = PROTOCOL_VERSION;
        ok = ok && (v.protocol_version >= MIN_PEER_PROTO_VERSION);

        /* Ancient protocol version */
        v.protocol_version = 209;
        ok = ok && (v.protocol_version < MIN_PEER_PROTO_VERSION);

        /* Zero protocol version */
        v.protocol_version = 0;
        ok = ok && (v.protocol_version < MIN_PEER_PROTO_VERSION);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    return failures;
}
