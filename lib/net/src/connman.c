/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#define _DEFAULT_SOURCE
#include "net/connman.h"
#include "net/addrman.h"
#include "net/download.h"
#include "controllers/blog_controller.h"
#include "core/random.h"
#include "net/netbase.h"
#include "net/version.h"
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "core/utiltime.h"

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

static void *thread_dns_seed(void *arg)
{
    struct connman *cm = (struct connman *)arg;

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
            /* TODO: connect to discovered .onion peers via Tor SOCKS
             * or direct dynhost connection */
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

        /* Try more peers per tick when connections are low */
        int attempts = (outbound == 0) ? 5 : (outbound < 4) ? 3 : 1;
        for (int a = 0; a < attempts && !g_stop; a++) {
            struct addr_info info;
            memset(&info, 0, sizeof(info));
            if (!addrman_select(&cm->manager.addrman, false, &info))
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
        fd_set readfds, writefds;
        FD_ZERO(&readfds);
        FD_ZERO(&writefds);
        int maxfd = -1;

        /* Add listen sockets */
        for (size_t i = 0; i < cm->manager.num_listen_sockets; i++) {
            int fd = (int)cm->manager.listen_sockets[i].socket;
            FD_SET(fd, &readfds);
            if (fd > maxfd) maxfd = fd;
        }

        /* Add connected nodes */
        zcl_mutex_lock(&cm->manager.cs_nodes);
        for (size_t i = 0; i < cm->manager.num_nodes; i++) {
            struct p2p_node *node = cm->manager.nodes[i];
            int fd = (int)node->socket;
            if (fd < 0) continue;
            FD_SET(fd, &readfds);
            if (node->send_size > 0)
                FD_SET(fd, &writefds);
            if (fd > maxfd) maxfd = fd;
        }
        zcl_mutex_unlock(&cm->manager.cs_nodes);

        if (maxfd < 0) {
            usleep(50000);
            continue;
        }

        struct timeval tv = { .tv_sec = 0, .tv_usec = 50000 };
        int nsel = select(maxfd + 1, &readfds, &writefds, NULL, &tv);
        if (nsel <= 0) continue;

        /* Accept new connections via net.c accept_connection() */
        for (size_t i = 0; i < cm->manager.num_listen_sockets; i++) {
            int lfd = (int)cm->manager.listen_sockets[i].socket;
            if (FD_ISSET(lfd, &readfds))
                accept_connection(&cm->manager,
                                  &cm->manager.listen_sockets[i]);
        }

        /* Read/write on connected nodes */
        zcl_mutex_lock(&cm->manager.cs_nodes);
        for (size_t i = 0; i < cm->manager.num_nodes; i++) {
            struct p2p_node *node = cm->manager.nodes[i];
            int fd = (int)node->socket;
            if (fd < 0) continue;

            if (FD_ISSET(fd, &readfds) && !node->disconnect) {
                zcl_mutex_lock(&node->cs_recv);
                char buf[0x10000];
                ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
                if (n > 0) {
                    if (!p2p_node_receive_bytes(node, buf, (unsigned int)n,
                                                cm->manager.message_start)) {
                        node->disconnect = true;
                    }
                    node->last_recv = GetTime();
                    node->recv_bytes += (uint64_t)n;
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

            if (FD_ISSET(fd, &writefds)) {
                zcl_mutex_lock(&node->cs_send);
                socket_send_data(node);
                zcl_mutex_unlock(&node->cs_send);
            }
        }
        zcl_mutex_unlock(&cm->manager.cs_nodes);

        /* Free deferred nodes from previous cycle */
        zcl_mutex_lock(&cm->manager.cs_nodes);
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

        /* Timeout: disconnect nodes with no activity for 120s */
        {
            int64_t now_check = GetTime();
            for (size_t i = 0; i < cm->manager.num_nodes; i++) {
                struct p2p_node *n = cm->manager.nodes[i];
                if (n->disconnect) continue;
                /* No recv for 120s and version handshake done */
                if (n->state >= PEER_HANDSHAKE_COMPLETE &&
                    n->last_recv > 0 &&
                    now_check - n->last_recv > 120) {
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
    net_manager_init(&cm->manager);
    cm->params = params;
    cm->started = false;
    cm->num_deferred_free = 0;
    cm->manager.signals = *signals;

    memcpy(cm->manager.message_start, params->pchMessageStart,
           MESSAGE_START_SIZE);
    cm->manager.default_port = (uint16_t)params->nDefaultPort;
    cm->manager.local_services = NODE_NETWORK | NODE_BLOOM;
    cm->manager.local_host_nonce = GetRand(UINT64_MAX);
    snprintf(cm->manager.sub_version, MAX_SUBVERSION_LENGTH,
             "/ZClassic-C23:1.0.0/");

    return true;
}

bool connman_start(struct connman *cm)
{
    g_stop = false;

    pthread_create(&g_thread_dns_seed, NULL, thread_dns_seed, cm);
    pthread_create(&g_thread_socket, NULL, thread_socket_handler, cm);
    pthread_create(&g_thread_open, NULL, thread_open_connections, cm);
    pthread_create(&g_thread_message, NULL, thread_message_handler, cm);

    cm->started = true;
    printf("P2P threads started.\n");
    return true;
}

void connman_stop(struct connman *cm)
{
    g_stop = true;
    if (cm->started) {
        pthread_join(g_thread_dns_seed, NULL);
        pthread_join(g_thread_socket, NULL);
        pthread_join(g_thread_open, NULL);
        pthread_join(g_thread_message, NULL);
        cm->started = false;
    }
    printf("P2P threads stopped.\n");
}

void connman_free(struct connman *cm)
{
    if (cm->started)
        connman_stop(cm);
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
    connect_node(&cm->manager, (struct net_address *)addr, NULL);
}

size_t connman_get_node_count(const struct connman *cm)
{
    return cm->manager.num_nodes;
}
