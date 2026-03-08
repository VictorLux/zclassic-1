/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#define _DEFAULT_SOURCE
#include "net/connman.h"
#include "core/random.h"
#include "net/netbase.h"
#include "net/version.h"
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

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

static void *thread_dns_seed(void *arg)
{
    struct connman *cm = (struct connman *)arg;
    sleep(11);
    if (!g_stop)
        dns_seed_resolve(cm);
    return NULL;
}

static void *thread_open_connections(void *arg)
{
    struct connman *cm = (struct connman *)arg;

    while (!g_stop) {
        sleep(1);

        if (cm->manager.num_nodes >= (size_t)cm->manager.max_connections)
            continue;

        size_t outbound = 0;
        zcl_mutex_lock(&cm->manager.cs_nodes);
        for (size_t i = 0; i < cm->manager.num_nodes; i++) {
            if (!cm->manager.nodes[i]->inbound)
                outbound++;
        }
        zcl_mutex_unlock(&cm->manager.cs_nodes);

        if (outbound >= MAX_OUTBOUND_CONNECTIONS)
            continue;

        struct addr_info info;
        memset(&info, 0, sizeof(info));
        if (!addrman_select(&cm->manager.addrman, false, &info))
            continue;

        connect_node(&cm->manager, &info.addr, NULL);
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

        /* Accept new connections */
        for (size_t i = 0; i < cm->manager.num_listen_sockets; i++) {
            int lfd = (int)cm->manager.listen_sockets[i].socket;
            if (FD_ISSET(lfd, &readfds)) {
                struct sockaddr_storage saddr;
                socklen_t slen = sizeof(saddr);
                int newfd = accept(lfd, (struct sockaddr *)&saddr, &slen);
                if (newfd >= 0) {
                    struct net_address addr;
                    net_address_init(&addr);
                    addr.svc.port = (uint16_t)cm->params->nDefaultPort;
                    p2p_node_create(&cm->manager, (zcl_socket_t)newfd,
                                    &addr, "inbound", true);
                }
            }
        }

        /* Read/write on connected nodes */
        zcl_mutex_lock(&cm->manager.cs_nodes);
        for (size_t i = 0; i < cm->manager.num_nodes; i++) {
            struct p2p_node *node = cm->manager.nodes[i];
            int fd = (int)node->socket;
            if (fd < 0) continue;

            if (FD_ISSET(fd, &readfds)) {
                char buf[8192];
                ssize_t n = read(fd, buf, sizeof(buf));
                if (n > 0) {
                    p2p_node_receive_bytes(node, buf, (unsigned int)n,
                                           cm->manager.message_start);
                } else {
                    node->disconnect = true;
                }
            }

            if (FD_ISSET(fd, &writefds)) {
                socket_send_data(node);
            }
        }
        zcl_mutex_unlock(&cm->manager.cs_nodes);

        /* Disconnect flagged nodes */
        zcl_mutex_lock(&cm->manager.cs_nodes);
        for (size_t i = 0; i < cm->manager.num_nodes; ) {
            if (cm->manager.nodes[i]->disconnect) {
                p2p_node_close_socket(cm->manager.nodes[i]);
                p2p_node_release(cm->manager.nodes[i]);
                cm->manager.nodes[i] =
                    cm->manager.nodes[cm->manager.num_nodes - 1];
                cm->manager.num_nodes--;
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

            if (node->recv_msg_count > 0 &&
                cm->manager.signals.process_messages) {
                cm->manager.signals.process_messages(
                    cm->manager.signals.ctx, node);
                did_work = true;
            }

            if (cm->manager.signals.send_messages) {
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
    cm->manager.signals = *signals;

    memcpy(cm->manager.message_start, params->pchMessageStart,
           MESSAGE_START_SIZE);
    cm->manager.default_port = (uint16_t)params->nDefaultPort;
    cm->manager.local_services = NODE_NETWORK;
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
