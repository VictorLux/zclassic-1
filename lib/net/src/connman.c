/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#define _DEFAULT_SOURCE
#include "net/connman.h"
#include "net/addrman.h"
#include "net/peer_bandwidth.h"
#include "net/peer_scoring.h"
#include "services/addrman_integrity.h"
#include "net/download.h"
#include "net/tor_integration.h"
#include "controllers/blog_controller.h"
#include "models/peer.h"
#include "core/random.h"
#include "core/serialize.h"
#include "net/netbase.h"
#include "net/version.h"
#include "bloom/bloom.h"
#include "config/runtime.h"
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "core/utiltime.h"
#include "util/safe_alloc.h"
#include "util/log_macros.h"

/* -connect mode: only connect to specified peers, no seeds */
bool g_connect_only = false;

/* Per-peer bandwidth quotas (wave 10 #3). */
static struct peer_bandwidth g_peer_bw;
static bool g_peer_bw_active = false;

static pthread_t g_thread_dns_seed;
static pthread_t g_thread_socket;
static pthread_t g_thread_open;
static pthread_t g_thread_message;
static volatile bool g_stop = false;

static void dns_seed_resolve(struct connman *cm)
{
    for (size_t i = 0; i < cm->params->nSeeds; i++) {
        const char *host = cm->params->vSeeds[i].host;
        if (host[0] == '\0') continue;

        printf("Resolving DNS seed: %s\n", host);

        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        struct addrinfo *res = NULL;
        if (getaddrinfo(host, NULL, &hints, &res) != 0)
            continue;

        int count = 0;
        for (struct addrinfo *p = res; p && count < 256; p = p->ai_next) {
            struct net_address addr;
            net_address_init(&addr);
            addr.svc.port = (uint16_t)cm->params->nDefaultPort;

            if (p->ai_family == AF_INET) {
                struct sockaddr_in *s4 = (struct sockaddr_in *)p->ai_addr;
                memset(addr.svc.addr.ip, 0, 10);
                addr.svc.addr.ip[10] = 0xff;
                addr.svc.addr.ip[11] = 0xff;
                memcpy(addr.svc.addr.ip + 12,
                       &s4->sin_addr, 4);
                addr.nServices = NODE_NETWORK;
            } else if (p->ai_family == AF_INET6) {
                struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)p->ai_addr;
                memcpy(addr.svc.addr.ip,
                       &s6->sin6_addr, 16);
                addr.nServices = NODE_NETWORK;
            } else {
                continue;
            }

            { struct net_addr src; net_addr_init(&src);
            addrman_add(&cm->manager.addrman, &addr, &src, 0); }
            count++;
        }
        freeaddrinfo(res);
        printf("DNS seed %s: %d addresses\n", host, count);
    }
}

static void seed_from_fixed(struct connman *cm)
{
    const struct chain_params *p = cm->params;
    for (size_t i = 0; i < p->nFixedSeeds && !g_stop; i++) {
        struct net_address addr;
        net_address_init(&addr);
        memcpy(addr.svc.addr.ip, p->vFixedSeeds[i].addr, 16);
        addr.svc.port = p->vFixedSeeds[i].port;
        addr.nServices = NODE_NETWORK;
        struct net_addr src;
        net_addr_init(&src);
        addrman_add(&cm->manager.addrman, &addr, &src, 0);
    }
    if (p->nFixedSeeds > 0)
        printf("Added %zu hardcoded seed nodes\n", p->nFixedSeeds);
}

/* Fetch /directory.json from a .onion seed and add clearnet IPs */
static void try_onion_seed_fetch(struct connman *cm, const char *onion)
{
    extern int tor_integration_fetch_onion_blocking(const char *, const char *,
        struct onion_fetch_result *, int);

    printf("Onion seed: fetching /directory.json from %s...\n", onion);
    fflush(stdout);

    struct onion_fetch_result result = {0};
    int rc = tor_integration_fetch_onion_blocking(onion, "/directory.json",
                                                    &result, 60);
    if (rc < 0 || result.status != 200 || !result.body) {
        printf("Onion seed: fetch failed (rc=%d status=%d)\n",
               rc, result.status);
        if (result.body) free(result.body);
        return;
    }

    /* Parse minimal JSON: extract clearnet_ip and clearnet_port fields */
    const char *p = (const char *)result.body;
    int added = 0;
    while ((p = strstr(p, "\"clearnet_ip\":\"")) != NULL) {
        p += 15; /* skip "clearnet_ip":" */
        const char *end = strchr(p, '"');
        if (!end || end == p) { p++; continue; }

        char ip[64];
        size_t iplen = (size_t)(end - p);
        if (iplen >= sizeof(ip)) { p = end; continue; }
        memcpy(ip, p, iplen);
        ip[iplen] = '\0';
        p = end + 1;

        /* Find clearnet_port */
        uint16_t port = 8033;
        const char *pp = strstr(p, "\"clearnet_port\":");
        if (pp && pp - p < 50) {
            port = (uint16_t)atoi(pp + 16);
            if (port == 0) port = 8033;
        }

        /* Add to address manager */
        if (ip[0] && strcmp(ip, "0.0.0.0") != 0) {
            struct net_address addr;
            memset(&addr, 0, sizeof(addr));
            /* Parse IPv4 */
            unsigned a, b, c, d;
            if (sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
                /* IPv4-mapped IPv6 */
                memset(addr.svc.addr.ip, 0, 10);
                addr.svc.addr.ip[10] = 0xff;
                addr.svc.addr.ip[11] = 0xff;
                addr.svc.addr.ip[12] = (uint8_t)a;
                addr.svc.addr.ip[13] = (uint8_t)b;
                addr.svc.addr.ip[14] = (uint8_t)c;
                addr.svc.addr.ip[15] = (uint8_t)d;
                addr.svc.port = port;
                addr.nServices = NODE_NETWORK;
                struct net_addr src;
                net_addr_init(&src);
                addrman_add(&cm->manager.addrman, &addr, &src, 0);
                added++;
                printf("Onion seed: discovered clearnet peer %s:%d\n",
                       ip, port);
            }
        }
    }

    printf("Onion seed: added %d clearnet peers from %s\n", added, onion);
    free(result.body);
}

static void *thread_dns_seed(void *arg)
{
    struct connman *cm = (struct connman *)arg;

    if (g_connect_only) {
        printf("Connect-only mode: skipping seeds, connecting to addnodes only\n");
        return NULL;
    }

    /* Add fixed seeds immediately — don't wait */
    seed_from_fixed(cm);

    /* DNS seeds after 3 seconds (not 11) */
    sleep(3);
    if (!g_stop)
        dns_seed_resolve(cm);

    /* ZSLP chain scan — discover .onion peers from on-chain token data.
     * This is the Tor-native peer discovery: no DNS, no clearnet. */
    if (!g_stop) {
        extern const char *g_blog_datadir;
        if (g_blog_datadir) {
            struct onion_peer peers[64];
            int found = blog_discover_onion_peers(g_blog_datadir, peers, 64);
            if (found > 0) {
                printf("ZSLP chain scan: discovered %d .onion peers\n", found);
                for (int i = 0; i < found; i++)
                    printf("  .onion peer: %s (h=%d)\n",
                           peers[i].hostname, peers[i].height);
            }
            /* Try fetching clearnet IPs from discovered .onion peers */
            extern bool tor_integration_is_ready(void);
            if (tor_integration_is_ready()) {
                for (int i = 0; i < found && i < 3 && !g_stop; i++) {
                    try_onion_seed_fetch(cm, peers[i].hostname);
                }
            }
        }
    }

    /* Also try hardcoded onion seeds if Tor is ready and few peers */
    if (!g_stop && cm->manager.num_nodes < 3) {
        extern bool tor_integration_is_ready(void);
        if (tor_integration_is_ready()) {
            /* Hardcoded .onion seeds for ZClassic23 network.
             * These are known nodes that serve /directory.json with
             * clearnet IPs for fast direct P2P connections. */
            static const char *onion_seeds[] = {
                /* Add seed .onion addresses here as they become known.
                 * This server's .onion gets populated after first Tor boot. */
                NULL
            };
            for (int i = 0; onion_seeds[i] && !g_stop; i++)
                try_onion_seed_fetch(cm, onion_seeds[i]);
        }
    }

    /* If still no peers after 15s, retry everything */
    sleep(12);
    if (!g_stop && cm->manager.num_nodes == 0) {
        printf("No peers found, retrying all discovery methods...\n");
        seed_from_fixed(cm);
        dns_seed_resolve(cm);
    }

    /* Adaptive peer discovery:
     * - 0 peers: retry every 30s (urgent)
     * - 1-2 peers: retry every 60s (degraded)
     * - 3+ peers: check every 5 minutes (healthy) */
    while (!g_stop) {
        size_t n = cm->manager.num_nodes;
        int interval = (n == 0) ? 30 : (n < 3) ? 60 : 300;
        sleep(interval);
        if (g_stop) break;
        if (cm->manager.num_nodes < 3) {
            printf("Peer discovery: %zu peers (need 3+)\n",
                   cm->manager.num_nodes);
            seed_from_fixed(cm);
            dns_seed_resolve(cm);
        }
    }

    return NULL;
}

/* Extract /16 subnet group from IPv4-mapped address (bytes 12-13).
 * Returns a 16-bit value representing the first two octets. */
static uint16_t ipv4_group16(const unsigned char ip[16])
{
    return (uint16_t)((ip[12] << 8) | ip[13]);
}

/* Count outbound peers in the same /16 subnet group. Caller holds cs_nodes. */
static int count_outbound_in_group(const struct net_manager *nm, uint16_t group)
{
    int count = 0;
    for (size_t i = 0; i < nm->num_nodes; i++) {
        const struct p2p_node *n = nm->nodes[i];
        if (n->inbound) continue;
        if (!net_addr_is_ipv4(&n->addr.svc.addr)) continue;
        if (ipv4_group16(n->addr.svc.addr.ip) == group)
            count++;
    }
    return count;
}

#define MAX_OUTBOUND_PER_GROUP16 2

static void *thread_open_connections(void *arg)
{
    struct connman *cm = (struct connman *)arg;

    while (!g_stop) {
        /* Aggressive: try up to 3 connections per second when we have few peers */
        size_t outbound = 0;
        zcl_mutex_lock(&cm->manager.cs_nodes);
        for (size_t i = 0; i < cm->manager.num_nodes; i++) {
            if (!cm->manager.nodes[i]->inbound)
                outbound++;
        }
        zcl_mutex_unlock(&cm->manager.cs_nodes);

        if (outbound >= MAX_OUTBOUND_CONNECTIONS ||
            cm->manager.num_nodes >= (size_t)cm->manager.max_connections) {
            sleep(1);
            continue;
        }

        /* In connect-only mode, only maintain 1 outbound connection
         * per addnode. Multiple connections cause snapshot serving to
         * split across connections and stall. */
        if (g_connect_only && outbound >= 1) {
            sleep(1);
            continue;
        }

        /* ZCL23 peer preference: 50% of connection attempts go to known
         * ZCL23 peers (fast sync capable, high bandwidth). This creates
         * a tight mesh of power nodes that find each other quickly. */
        bool tried_zcl23 = false;
        struct node_db *ndb = app_runtime_node_db();
        if (ndb && (GetRand(2) == 0)) {
            struct db_peer zcl_peers[8];
            int nzcl = db_peer_fast_zcl23(ndb, zcl_peers, 8);
            if (nzcl > 0) {
                struct db_peer *pick = &zcl_peers[GetRand((uint64_t)nzcl)];
                /* Check not already connected */
                bool already = false;
                zcl_mutex_lock(&cm->manager.cs_nodes);
                for (size_t ni = 0; ni < cm->manager.num_nodes; ni++) {
                    struct p2p_node *n = cm->manager.nodes[ni];
                    if (!n->disconnect &&
                        memcmp(n->addr.svc.addr.ip, pick->ip, 16) == 0 &&
                        n->addr.svc.port == pick->port) {
                        already = true;
                        break;
                    }
                }
                zcl_mutex_unlock(&cm->manager.cs_nodes);
                if (!already) {
                    struct net_address addr;
                    memset(&addr, 0, sizeof(addr));
                    memcpy(addr.svc.addr.ip, pick->ip, 16);
                    addr.svc.port = pick->port;
                    addr.nServices = pick->services;
                    addr.nTime = (uint32_t)time(NULL);
                    struct p2p_node *node = connect_node(&cm->manager,
                                                          &addr, NULL);
                    if (node) tried_zcl23 = true;
                }
            }
        }

        /* Try more peers per tick when connections are low */
        int attempts = (outbound == 0) ? 3 : (tried_zcl23 ? 0 : 1);
        for (int a = 0; a < attempts && !g_stop; a++) {
            struct addr_info info;
            memset(&info, 0, sizeof(info));
            if (!addrman_select(&cm->manager.addrman, false, &info))
                continue;

            /* Skip non-ZClassic ports (16125 etc from corrupted addrman).
             * ZClassic mainnet uses port 8033. */
            uint16_t port = info.addr.svc.port;
            if (port != 8033 && port != 18033 && port != 8034 &&
                port != 9033 && port != 20022)
                continue;

            /* Eclipse attack defense: limit outbound peers per /16 subnet */
            if (net_addr_is_ipv4(&info.addr.svc.addr)) {
                uint16_t group = ipv4_group16(info.addr.svc.addr.ip);
                zcl_mutex_lock(&cm->manager.cs_nodes);
                int in_group = count_outbound_in_group(&cm->manager, group);
                zcl_mutex_unlock(&cm->manager.cs_nodes);
                if (in_group >= MAX_OUTBOUND_PER_GROUP16)
                    continue;
            }

            struct p2p_node *node = connect_node(&cm->manager, &info.addr, NULL);
            if (!node) {
                addrman_attempt(&cm->manager.addrman, &info.addr.svc,
                                 (int64_t)time(NULL));
                char ipbuf[64];
                net_addr_to_string(&info.addr.svc.addr, ipbuf, sizeof(ipbuf));
                event_emitf(EV_TCP_CONNECT_FAILED, 0,
                            "%s:%u", ipbuf, info.addr.svc.port);
            }
        }

        /* Reconnect persistent addnodes that dropped.
         * 30-second cooldown per addnode to prevent reconnect storms
         * that cause use-after-free crashes from rapid node churn. */
        {
            static int64_t s_addnode_last_attempt[MAX_ADDNODES] = {0};
            int64_t now_an = (int64_t)time(NULL);
            for (int ai = 0; ai < cm->num_addnodes && !g_stop; ai++) {
                if (now_an - s_addnode_last_attempt[ai] < 30)
                    continue; /* cooldown */
                bool connected = false;
                zcl_mutex_lock(&cm->manager.cs_nodes);
                for (size_t ni = 0; ni < cm->manager.num_nodes; ni++) {
                    struct p2p_node *n = cm->manager.nodes[ni];
                    if (n->disconnect) continue;
                    if (net_addr_eq(&n->addr.svc.addr, &cm->addnodes[ai].svc.addr) &&
                        n->addr.svc.port == cm->addnodes[ai].svc.port) {
                        connected = true;
                        break;
                    }
                }
                zcl_mutex_unlock(&cm->manager.cs_nodes);

                if (!connected) {
                    s_addnode_last_attempt[ai] = now_an;
                    char dest[64];
                    net_service_to_string(&cm->addnodes[ai].svc, dest, sizeof(dest));
                    connect_node(&cm->manager, &cm->addnodes[ai], dest);
                }
            }
        }

        /* Sleep less when desperate for peers */
        if (outbound == 0)
            usleep(200000); /* 200ms */
        else
            sleep(1);
    }
    return NULL;
}

static void *thread_socket_handler(void *arg)
{
    struct connman *cm = (struct connman *)arg;

    while (!g_stop) {
        /* Build poll array: listen sockets + connected nodes.
         * Using poll() instead of select() avoids FD_SETSIZE (1024) limit
         * which caused stack corruption with high fd numbers. */
        struct pollfd pfds[256]; /* max: 8 listen + ~200 peers */
        size_t npfds = 0;
        size_t listen_count = cm->manager.num_listen_sockets;

        /* Add listen sockets */
        for (size_t i = 0; i < listen_count && npfds < 256; i++) {
            pfds[npfds].fd = (int)cm->manager.listen_sockets[i].socket;
            pfds[npfds].events = POLLIN;
            pfds[npfds].revents = 0;
            npfds++;
        }

        /* Add connected nodes — snapshot fd + index under lock */
        struct { int fd; size_t node_idx; } node_fds[200];
        size_t n_node_fds = 0;
        zcl_mutex_lock(&cm->manager.cs_nodes);
        for (size_t i = 0; i < cm->manager.num_nodes && npfds < 256; i++) {
            struct p2p_node *node = cm->manager.nodes[i];
            int fd = (int)node->socket;
            if (fd < 0) continue;
            short events = POLLIN;
            if (node->send_size > 0)
                events |= POLLOUT;
            pfds[npfds].fd = fd;
            pfds[npfds].events = events;
            pfds[npfds].revents = 0;
            if (n_node_fds < 200) {
                node_fds[n_node_fds].fd = fd;
                node_fds[n_node_fds].node_idx = i;
                n_node_fds++;
            }
            npfds++;
        }
        zcl_mutex_unlock(&cm->manager.cs_nodes);

        if (npfds == 0) {
            usleep(50000);
            continue;
        }

        int nready = poll(pfds, (nfds_t)npfds, 50 /* ms */);
        if (nready <= 0) continue;

        /* Accept new connections via net.c accept_connection() */
        for (size_t i = 0; i < listen_count; i++) {
            if (pfds[i].revents & POLLIN)
                accept_connection(&cm->manager,
                                  &cm->manager.listen_sockets[i]);
        }

        /* Read/write on connected nodes — re-acquire lock and match by fd.
         * Nodes may have changed since the poll snapshot, so validate. */
        zcl_mutex_lock(&cm->manager.cs_nodes);
        for (size_t pi = 0; pi < n_node_fds; pi++) {
            size_t poll_idx = listen_count + pi;
            if (poll_idx >= npfds) break;
            short rev = pfds[poll_idx].revents;
            if (!rev) continue;

            /* Find the node that still has this fd */
            int target_fd = node_fds[pi].fd;
            struct p2p_node *node = NULL;
            for (size_t ni = 0; ni < cm->manager.num_nodes; ni++) {
                if ((int)cm->manager.nodes[ni]->socket == target_fd) {
                    node = cm->manager.nodes[ni];
                    break;
                }
            }
            if (!node) continue; /* node was removed between poll and now */

            if ((rev & POLLIN) && !node->disconnect) {
                /* Bandwidth quota: check download budget before recv.
                 * If no tokens available, skip this peer until refill. */
                uint32_t bw_id = (uint32_t)node->id;
                size_t bw_avail = g_peer_bw_active
                    ? peer_bandwidth_available(&g_peer_bw, bw_id, PEER_BW_DOWN)
                    : SIZE_MAX;
                if (bw_avail == 0) goto skip_recv;

                zcl_mutex_lock(&node->cs_recv);
                /* Backpressure: if the message queue is full, skip recv
                 * until the processing thread drains it. This prevents
                 * disconnecting fast senders (e.g. snapshot serving over
                 * localhost) just because we can't parse fast enough. */
                if (node->recv_msg_count >= MAX_RECV_MESSAGES) {
                    zcl_mutex_unlock(&node->cs_recv);
                    goto skip_recv;
                }
                char buf[0x10000];
                /* Cap recv size to bandwidth budget. */
                size_t recv_cap = sizeof(buf);
                if (bw_avail < recv_cap) recv_cap = bw_avail;
                ssize_t n = recv(target_fd, buf, recv_cap, MSG_DONTWAIT);
                if (n > 0) {
                    if (!p2p_node_receive_bytes(node, buf, (unsigned int)n,
                                                cm->manager.message_start)) {
                        node->disconnect = true;
                    }
                    node->last_recv = GetTime();
                    node->recv_bytes += (uint64_t)n;
                    /* Consume download tokens post-recv. */
                    if (g_peer_bw_active)
                        peer_bandwidth_consume(&g_peer_bw, bw_id,
                                               PEER_BW_DOWN, (size_t)n);
                } else if (n == 0) {
                    node->disconnect = true;
                } else {
                    int err = errno;
                    if (err != EAGAIN && err != EWOULDBLOCK && err != EINTR)
                        node->disconnect = true;
                }
                /* If disconnecting, clean up messages while holding lock */
                if (node->disconnect) {
                    for (size_t mi = 0; mi < node->recv_msg_count; mi++)
                        net_message_free(&node->recv_msgs[mi]);
                    node->recv_msg_count = 0;
                }
                zcl_mutex_unlock(&node->cs_recv);
            }
            skip_recv: ;

            if ((rev & POLLOUT) && !node->disconnect) {
                /* Bandwidth quota: check upload budget before send. */
                uint32_t bw_id_s = (uint32_t)node->id;
                size_t up_avail = g_peer_bw_active
                    ? peer_bandwidth_available(&g_peer_bw, bw_id_s, PEER_BW_UP)
                    : SIZE_MAX;
                if (up_avail > 0) {
                    zcl_mutex_lock(&node->cs_send);
                    uint64_t before = node->send_bytes;
                    socket_send_data(node);
                    uint64_t sent = node->send_bytes - before;
                    zcl_mutex_unlock(&node->cs_send);
                    /* Consume upload tokens for actual bytes sent. */
                    if (g_peer_bw_active && sent > 0)
                        peer_bandwidth_consume(&g_peer_bw, bw_id_s,
                                               PEER_BW_UP, (size_t)sent);
                }
                /* If up_avail == 0, skip send — peer is throttled. */
            }

            /* POLLHUP/POLLERR — peer disconnected or socket error */
            if (rev & (POLLHUP | POLLERR))
                node->disconnect = true;
        }
        /* Free deferred nodes (still under cs_nodes lock) */
        for (size_t i = 0; i < cm->num_deferred_free; i++)
            p2p_node_free(cm->deferred_free[i]);
        cm->num_deferred_free = 0;

        /* Periodic peer stats (every 60s) */
        {
            static int64_t last_peer_log = 0;
            int64_t now_log = GetTime();
            if (now_log - last_peer_log >= 60) {
                last_peer_log = now_log;
                size_t in = 0, out = 0, connected = 0;
                for (size_t pi = 0; pi < cm->manager.num_nodes; pi++) {
                    struct p2p_node *p = cm->manager.nodes[pi];
                    if (p->disconnect) continue;
                    if (p->inbound) in++; else out++;
                    if (p->state >= PEER_HANDSHAKE_COMPLETE) connected++;
                }
                if (out == 0 && cm->manager.num_nodes > 0)
                    printf("WARNING: 0 outbound peers (%zu inbound) "
                           "— cannot sync\n", in);
                else if (cm->manager.num_nodes > 0)
                    printf("Peers: %zu (%zu out, %zu in, %zu active)\n",
                           cm->manager.num_nodes, out, in, connected);
                else if (now_log > 30) /* don't warn during first 30s */
                    printf("WARNING: 0 peers connected\n");
            }
        }

        /* Timeout: disconnect nodes with no activity */
        {
            int64_t now_check = GetTime();
            for (size_t i = 0; i < cm->manager.num_nodes; i++) {
                struct p2p_node *n = cm->manager.nodes[i];
                if (n->disconnect) continue;

                /* Addnode peers get longer timeout (10 min) since they
                 * auto-reconnect anyway and we want them stable */
                bool is_addnode = false;
                for (int ai = 0; ai < cm->num_addnodes; ai++) {
                    if (net_addr_eq(&n->addr.svc.addr,
                                    &cm->addnodes[ai].svc.addr) &&
                        n->addr.svc.port == cm->addnodes[ai].svc.port) {
                        is_addnode = true;
                        break;
                    }
                }
                int timeout = is_addnode ? 600 : 120;

                if (n->state >= PEER_HANDSHAKE_COMPLETE &&
                    n->last_recv > 0 &&
                    now_check - n->last_recv > timeout) {
                    event_emitf(EV_TCP_TIMEOUT, (uint32_t)n->id,
                                "inactivity %llds state=%s",
                                (long long)(now_check - n->last_recv),
                                peer_state_name(n->state));
                    printf("Peer %s: timeout (no data for %llds)\n",
                           n->addr_name,
                           (long long)(now_check - n->last_recv));
                    n->disconnect = true;
                }
                /* Version handshake timeout: 30s */
                if (n->state < PEER_HANDSHAKE_COMPLETE &&
                    n->time_connected > 0 &&
                    now_check - n->time_connected > 30) {
                    event_emitf(EV_TCP_TIMEOUT, (uint32_t)n->id,
                                "handshake %llds state=%s",
                                (long long)(now_check - n->time_connected),
                                peer_state_name(n->state));
                    printf("Peer %s: handshake timeout after %llds "
                           "(version=%d, state=%s, %s)\n",
                           n->addr_name,
                           (long long)(now_check - n->time_connected),
                           n->version,
                           peer_state_name(n->state),
                           n->inbound ? "inbound" : "outbound");
                    n->disconnect = true;
                }
            }
        }

        /* Disconnect flagged nodes — defer free to next cycle */
        for (size_t i = 0; i < cm->manager.num_nodes; ) {
            if (cm->manager.nodes[i]->disconnect) {
                struct p2p_node *node = cm->manager.nodes[i];
                event_emitf(EV_TCP_DISCONNECTED, (uint32_t)node->id,
                            "%s state=%s misbehavior=%d",
                            node->addr_name,
                            peer_state_name(node->state),
                            node->misbehavior);

                /* Re-queue any in-flight blocks from this peer */
                {
                    dl_peer_disconnected(msg_get_download_mgr(),
                                          (uint32_t)node->id);
                }

                /* Force disconnect — bypass transition validator since this
                 * is cleanup, not a normal state change. The event was
                 * already emitted (EV_TCP_DISCONNECTED above). */
                node->state = PEER_DISCONNECTED;
                p2p_node_close_socket(node);

                /* Clean send queue while holding cs_nodes */
                zcl_mutex_lock(&node->cs_send);
                while (node->send_head) {
                    struct send_segment *seg = node->send_head;
                    node->send_head = seg->next;
                    free(seg->data);
                    free(seg);
                }
                node->send_size = 0;
                zcl_mutex_unlock(&node->cs_send);

                /* Clean recv messages */
                zcl_mutex_lock(&node->cs_recv);
                for (size_t mi = 0; mi < node->recv_msg_count; mi++)
                    net_message_free(&node->recv_msgs[mi]);
                node->recv_msg_count = 0;
                zcl_mutex_unlock(&node->cs_recv);

                cm->manager.nodes[i] =
                    cm->manager.nodes[cm->manager.num_nodes - 1];
                cm->manager.num_nodes--;
                if (cm->num_deferred_free < 64)
                    cm->deferred_free[cm->num_deferred_free++] = node;
                else
                    p2p_node_free(node);
            } else {
                i++;
            }
        }
        zcl_mutex_unlock(&cm->manager.cs_nodes);
    }
    return NULL;
}

static void *thread_message_handler(void *arg)
{
    struct connman *cm = (struct connman *)arg;

    while (!g_stop) {
        bool did_work = false;

        zcl_mutex_lock(&cm->manager.cs_nodes);
        for (size_t i = 0; i < cm->manager.num_nodes; i++) {
            struct p2p_node *node = cm->manager.nodes[i];
            if (node->disconnect) continue;

            if (node->recv_msg_count > 0 &&
                cm->manager.signals.process_messages) {
                cm->manager.signals.process_messages(
                    cm->manager.signals.ctx, node);
                did_work = true;
            }

            if (!node->disconnect && cm->manager.signals.send_messages) {
                bool trickle = (GetRand(cm->manager.num_nodes) == 0);
                cm->manager.signals.send_messages(
                    cm->manager.signals.ctx, node, trickle);
                /* Keep tight loop when serving snapshot — don't sleep */
                if (node->state == PEER_SNAPSHOT_SERVING ||
                    node->state == PEER_SNAPSHOT_RECEIVING)
                    did_work = true;
            }
        }
        zcl_mutex_unlock(&cm->manager.cs_nodes);

        if (!did_work)
            usleep(100000);
    }
    return NULL;
}

bool connman_init(struct connman *cm, const struct chain_params *params,
                   struct node_signals *signals)
{
    /* Load peer-scoring config from environment. Safe to call multiple
     * times; late env-var changes don't matter since connman_init runs
     * once per process startup. Done here so every binary that spins up
     * a connman (node, test, tool) honours the operator's settings. */
    peer_scoring_init();

    net_manager_init(&cm->manager);
    cm->params = params;
    cm->started = false;
    cm->dns_seed_thread_started = false;
    cm->socket_thread_started = false;
    cm->open_thread_started = false;
    cm->message_thread_started = false;
    cm->num_deferred_free = 0;
    cm->manager.signals = *signals;

    memcpy(cm->manager.message_start, params->pchMessageStart,
           MESSAGE_START_SIZE);
    cm->manager.default_port = (uint16_t)params->nDefaultPort;
    cm->manager.local_services = NODE_NETWORK;
    if (bip37_enabled())
        cm->manager.local_services |= NODE_BLOOM;
    cm->manager.local_host_nonce = GetRand(UINT64_MAX);
    snprintf(cm->manager.sub_version, MAX_SUBVERSION_LENGTH,
             "/ZClassic-C23:1.0.0/");

    return true;
}

bool connman_start(struct connman *cm)
{
    if (!cm)
        LOG_FAIL("net", "connman_start called with NULL connman");
    if (cm->started)
        return true;

    /* Initialize per-peer bandwidth quotas from env vars. */
    if (!g_peer_bw_active) {
        peer_bandwidth_init(&g_peer_bw);
        peer_bandwidth_load_from_env(&g_peer_bw);
        g_peer_bw_active = true;
    }

    g_stop = false;

    if (pthread_create(&g_thread_dns_seed, NULL, thread_dns_seed, cm) != 0) {
        perror("connman: pthread_create dns_seed");
        g_stop = true;
        LOG_FAIL("net", "pthread_create failed for dns_seed thread");
    }
    cm->dns_seed_thread_started = true;

    if (pthread_create(&g_thread_socket, NULL, thread_socket_handler, cm) != 0) {
        perror("connman: pthread_create socket");
        g_stop = true;
        pthread_join(g_thread_dns_seed, NULL);
        cm->dns_seed_thread_started = false;
        LOG_FAIL("net", "pthread_create failed for socket_handler thread");
    }
    cm->socket_thread_started = true;

    if (pthread_create(&g_thread_open, NULL, thread_open_connections, cm) != 0) {
        perror("connman: pthread_create open");
        g_stop = true;
        pthread_join(g_thread_socket, NULL);
        pthread_join(g_thread_dns_seed, NULL);
        cm->socket_thread_started = false;
        cm->dns_seed_thread_started = false;
        LOG_FAIL("net", "pthread_create failed for open_connections thread");
    }
    cm->open_thread_started = true;

    if (pthread_create(&g_thread_message, NULL, thread_message_handler, cm) != 0) {
        perror("connman: pthread_create message");
        g_stop = true;
        pthread_join(g_thread_open, NULL);
        pthread_join(g_thread_socket, NULL);
        pthread_join(g_thread_dns_seed, NULL);
        cm->open_thread_started = false;
        cm->socket_thread_started = false;
        cm->dns_seed_thread_started = false;
        LOG_FAIL("net", "pthread_create failed for message_handler thread");
    }
    cm->message_thread_started = true;

    cm->started = true;
    printf("P2P threads started.\n");
    return true;
}

void connman_signal_stop(struct connman *cm)
{
    (void)cm;
    g_stop = true;
}

/* Join a thread with a timeout using a cancel-based approach.
 * Spawns a helper that joins the target; if it takes too long,
 * we detach and move on. */
static volatile bool g_join_done = false;
static pthread_t g_join_target;

static void *join_helper(void *arg)
{
    (void)arg;
    pthread_join(g_join_target, NULL);
    g_join_done = true;
    return NULL;
}

static bool timed_join(pthread_t thread, int timeout_sec)
{
    g_join_done = false;
    g_join_target = thread;

    pthread_t helper;
    if (pthread_create(&helper, NULL, join_helper, NULL) != 0) {
        /* Fallback: blocking join */
        pthread_join(thread, NULL);
        return true;
    }

    for (int i = 0; i < timeout_sec * 10; i++) {
        if (g_join_done) {
            pthread_join(helper, NULL);
            return true;
        }
        usleep(100000); /* 100ms */
    }

    /* Timeout — detach both threads */
    pthread_detach(helper);
    pthread_detach(thread);
    return false;
}

void connman_join(struct connman *cm)
{
    if (!cm)
        return;

    /* Use 30-second timeout per thread to prevent SIGKILL from systemd.
     * If a thread is stuck (e.g., message thread in activate_best_chain),
     * detach it rather than blocking shutdown indefinitely. */
    if (cm->started || cm->dns_seed_thread_started || cm->socket_thread_started ||
        cm->open_thread_started || cm->message_thread_started) {
        if (cm->dns_seed_thread_started) {
            if (!timed_join(g_thread_dns_seed, 30))
                fprintf(stderr, "connman: dns_seed thread join timed out\n");
            cm->dns_seed_thread_started = false;
        }
        if (cm->socket_thread_started) {
            if (!timed_join(g_thread_socket, 30))
                fprintf(stderr, "connman: socket thread join timed out\n");
            cm->socket_thread_started = false;
        }
        if (cm->open_thread_started) {
            if (!timed_join(g_thread_open, 30))
                fprintf(stderr, "connman: open thread join timed out\n");
            cm->open_thread_started = false;
        }
        if (cm->message_thread_started) {
            if (!timed_join(g_thread_message, 30))
                fprintf(stderr, "connman: message thread join timed out\n");
            cm->message_thread_started = false;
        }
        cm->started = false;
    }
    printf("P2P threads stopped.\n");
}

void connman_stop(struct connman *cm)
{
    connman_signal_stop(cm);
    connman_join(cm);

    /* Tear down bandwidth quotas. */
    if (g_peer_bw_active) {
        peer_bandwidth_destroy(&g_peer_bw);
        g_peer_bw_active = false;
    }
}

void connman_save_addrman(struct connman *cm)
{
    if (!cm->datadir) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/peers.dat", cm->datadir);

    struct byte_stream s;
    stream_init(&s, 65536);
    zcl_mutex_lock(&cm->manager.addrman.cs);
    bool ok = addrman_serialize(&cm->manager.addrman, &s);
    zcl_mutex_unlock(&cm->manager.addrman.cs);

    if (ok && s.size > 0) {
        char tmp_path[520];
        snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
        FILE *f = fopen(tmp_path, "wb");
        if (f) {
            size_t written = fwrite(s.data, 1, s.size, f);
            fflush(f);
            int fd = fileno(f);
            if (fd >= 0) (void)fsync(fd);
            fclose(f);
            if (written == s.size) {
                rename(tmp_path, path);
                /* Write the SHA3 sidecar so the next boot can
                 * detect tampering or partial corruption. Best-
                 * effort — a sidecar write failure is logged but
                 * not fatal; the next load will see
                 * AII_SIDECAR_MISSING and accept the body. */
                (void)aii_write_sidecar(cm->datadir);
                printf("Saved %zu peers to %s (%zu bytes)\n",
                       addrman_size(&cm->manager.addrman), path, s.size);
            } else {
                remove(tmp_path);
                fprintf(stderr, "addrman save: short write (%zu/%zu)\n",
                        written, s.size);
            }
        }
    }
    stream_free(&s);
}

void connman_load_addrman(struct connman *cm)
{
    if (!cm->datadir) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/peers.dat", cm->datadir);

    /* Integrity check before we deserialize. On any non-OK,
     * non-MISSING verdict we quarantine the file (rename aside)
     * and return — the caller ends up with an empty addrman and
     * must re-learn from DNS seeds / hardcoded peers. A corrupt
     * peers.dat is always safer discarded than deserialized:
     * the contents directly influence outbound peer selection,
     * which is exactly what an attacker would target. The
     * `SIDECAR_MISSING` verdict is expected on the first boot
     * after this service ships (the body has no companion yet);
     * we accept it. */
    char aii_err[256];
    enum aii_verdict verdict = aii_verify(cm->datadir, aii_err, sizeof(aii_err));
    if (verdict != AII_OK && verdict != AII_SIDECAR_MISSING &&
        verdict != AII_BODY_MISSING) {
        fprintf(stderr, "addrman load: integrity check failed (%s): %s\n",
                aii_verdict_name(verdict), aii_err);
        aii_quarantine_corrupt(cm->datadir, verdict);
        return;
    }

    FILE *f = fopen(path, "rb");
    if (!f) return; /* no saved peers — normal on first run */

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 50 * 1024 * 1024) { /* sanity: max 50 MB */
        fclose(f);
        return;
    }

    uint8_t *buf = zcl_malloc((size_t)sz, "connman_read_buf");
    if (!buf) { fclose(f); return; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);

    if (rd == (size_t)sz) {
        struct byte_stream s;
        stream_init_from_data(&s, buf, (size_t)sz);
        zcl_mutex_lock(&cm->manager.addrman.cs);
        if (addrman_deserialize(&cm->manager.addrman, &s)) {
            printf("Loaded %zu peers from %s\n",
                   addrman_size(&cm->manager.addrman), path);
        } else {
            fprintf(stderr, "addrman load: deserialize failed, starting fresh\n");
            addrman_clear(&cm->manager.addrman);
        }
        zcl_mutex_unlock(&cm->manager.addrman.cs);
        stream_free(&s);
    }
    free(buf);
}

void connman_free(struct connman *cm)
{
    if (cm->started)
        connman_stop(cm);
    connman_save_addrman(cm);
    net_manager_free(&cm->manager);
}

void connman_relay_transaction(struct connman *cm,
                                const struct uint256 *txid)
{
    struct inv_item inv;
    inv_item_init_typed(&inv, MSG_TX, txid);

    int relayed = 0;
    zcl_mutex_lock(&cm->manager.cs_nodes);
    for (size_t i = 0; i < cm->manager.num_nodes; i++) {
        struct p2p_node *node = cm->manager.nodes[i];
        if (node->state >= PEER_HANDSHAKE_COMPLETE && !node->disconnect) {
            p2p_node_push_inventory(node, &inv);
            relayed++;
        }
    }
    zcl_mutex_unlock(&cm->manager.cs_nodes);

    char hex[65];
    uint256_get_hex(txid, hex);
    printf("Relay tx %s to %d peers\n", hex, relayed);
}

void connman_add_seed_node(struct connman *cm, const char *host,
                            uint16_t port)
{
    struct net_address addr;
    net_address_init(&addr);
    addr.svc.port = port;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) == 0 && res) {
        if (res->ai_family == AF_INET) {
            struct sockaddr_in *s4 = (struct sockaddr_in *)res->ai_addr;
            memset(addr.svc.addr.ip, 0, 10);
            addr.svc.addr.ip[10] = 0xff;
            addr.svc.addr.ip[11] = 0xff;
            memcpy(addr.svc.addr.ip + 12, &s4->sin_addr, 4);
        } else if (res->ai_family == AF_INET6) {
            struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)res->ai_addr;
            memcpy(addr.svc.addr.ip, &s6->sin6_addr, 16);
        }
        freeaddrinfo(res);
        { struct net_addr src; net_addr_init(&src);
            addrman_add(&cm->manager.addrman, &addr, &src, 0); }
    }
}

void connman_open_connection(struct connman *cm,
                              const struct net_address *addr)
{
    /* Store in persistent addnode list for automatic reconnection */
    if (cm->num_addnodes < MAX_ADDNODES) {
        /* Avoid duplicates */
        bool dup = false;
        for (int i = 0; i < cm->num_addnodes; i++) {
            if (net_addr_eq(&cm->addnodes[i].svc.addr, &addr->svc.addr) &&
                cm->addnodes[i].svc.port == addr->svc.port) {
                dup = true;
                break;
            }
        }
        if (!dup)
            cm->addnodes[cm->num_addnodes++] = *addr;
    }

    /* Pass addr_name as dest so connect_node skips is_local check.
     * This allows connecting to localhost (e.g. local zclassicd peer). */
    char dest[64];
    net_service_to_string(&addr->svc, dest, sizeof(dest));
    connect_node(&cm->manager, (struct net_address *)addr, dest);
}

size_t connman_get_node_count(const struct connman *cm)
{
    return cm->manager.num_nodes;
}

struct peer_bandwidth *connman_peer_bandwidth(void)
{
    return g_peer_bw_active ? &g_peer_bw : NULL;
}
