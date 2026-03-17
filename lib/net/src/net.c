/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "net/net.h"
#include "core/hash.h"
#include "core/random.h"
#include "core/utiltime.h"
#include "core/serialize.h"
#include "crypto/sha256.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <netinet/tcp.h>
#include <ifaddrs.h>
#include <net/if.h>
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif
#else
#define MSG_NOSIGNAL 0
#define MSG_DONTWAIT 0
#endif

/* --- net_message --- */

void net_message_init(struct net_message *msg,
                      const unsigned char msgstart[MESSAGE_START_SIZE])
{
    memset(msg, 0, sizeof(*msg));
    msg->in_data = false;
    msg->hdr_pos = 0;
    msg->data_pos = 0;
    msg->recv_data = NULL;
    msg->recv_alloc = 0;
    msg->time_usec = 0;
    memcpy(msg->expected_msgstart, msgstart, MESSAGE_START_SIZE);
    msg_header_init(&msg->hdr, msgstart);
}

void net_message_free(struct net_message *msg)
{
    free(msg->recv_data);
    msg->recv_data = NULL;
    msg->recv_alloc = 0;
}

bool net_message_complete(const struct net_message *msg)
{
    if (!msg->in_data)
        return false;
    return msg->hdr.nMessageSize == msg->data_pos;
}

int net_message_read_header(struct net_message *msg,
                            const char *pch, unsigned int nbytes)
{
    unsigned int remaining = MSG_HEADER_SIZE - msg->hdr_pos;
    unsigned int copy = remaining < nbytes ? remaining : nbytes;

    memcpy(msg->hdr_buf + msg->hdr_pos, pch, copy);
    msg->hdr_pos += copy;

    if (msg->hdr_pos < MSG_HEADER_SIZE)
        return (int)copy;

    memcpy(&msg->hdr, msg->hdr_buf, MSG_HEADER_SIZE);

    /* Validate message start magic and size */
    if (memcmp(msg->hdr.pchMessageStart, msg->expected_msgstart,
               MESSAGE_START_SIZE) != 0)
        return -1;

    if (msg->hdr.nMessageSize > MAX_SIZE)
        return -1;

    msg->in_data = true;
    return (int)copy;
}

int net_message_read_data(struct net_message *msg,
                          const char *pch, unsigned int nbytes)
{
    unsigned int remaining = msg->hdr.nMessageSize - msg->data_pos;
    unsigned int copy = remaining < nbytes ? remaining : nbytes;

    size_t needed = msg->data_pos + copy;
    if (msg->recv_alloc < needed) {
        size_t alloc = msg->hdr.nMessageSize;
        uint8_t *tmp = realloc(msg->recv_data, alloc);
        if (!tmp) return -1;
        msg->recv_data = tmp;
        msg->recv_alloc = alloc;
    }

    memcpy(msg->recv_data + msg->data_pos, pch, copy);
    msg->data_pos += copy;
    return (int)copy;
}

/* --- send segment helpers --- */

static struct send_segment *send_segment_create(const uint8_t *data, size_t size)
{
    struct send_segment *seg = malloc(sizeof(*seg));
    if (!seg) return NULL;
    seg->data = malloc(size);
    if (!seg->data) { free(seg); return NULL; }
    memcpy(seg->data, data, size);
    seg->size = size;
    seg->next = NULL;
    return seg;
}

static void send_segment_free(struct send_segment *seg)
{
    free(seg->data);
    free(seg);
}

/* --- p2p_node --- */

struct p2p_node *p2p_node_create(struct net_manager *nm, zcl_socket_t sock,
                                  const struct net_address *addr,
                                  const char *name, bool inbound)
{
    struct p2p_node *node = calloc(1, sizeof(*node));
    if (!node) return NULL;

    node->socket = sock;
    node->addr = *addr;
    if (name && name[0]) {
        snprintf(node->addr_name, sizeof(node->addr_name), "%s", name);
    } else {
        char ipbuf[64];
        net_addr_to_string(&addr->svc.addr, ipbuf, sizeof(ipbuf));
        snprintf(node->addr_name, sizeof(node->addr_name), "%s:%u",
                 ipbuf, addr->svc.port);
    }

    node->inbound = inbound;
    node->recv_version = INIT_PROTO_VERSION;
    node->time_connected = GetTime();
    node->starting_height = -1;
    uint256_set_null(&node->hash_continue);
    net_service_init(&node->addr_local);

    zcl_mutex_init(&node->cs_send);
    zcl_mutex_init(&node->cs_recv);
    zcl_mutex_init(&node->cs_inventory);
    zcl_mutex_init(&node->cs_filter);

    rolling_bloom_init(&node->addr_known, 5000, 0.001);

    node->pfilter = calloc(1, sizeof(*node->pfilter));
    if (node->pfilter)
        bloom_filter_init(node->pfilter, 1, 0.0001, 0, BLOOM_UPDATE_NONE);

    node->min_ping_usec_time = INT64_MAX;

    zcl_mutex_lock(&nm->cs_last_node_id);
    node->id = nm->last_node_id++;
    zcl_mutex_unlock(&nm->cs_last_node_id);

    if (nm->signals.initialize_node)
        nm->signals.initialize_node(nm->signals.ctx, node->id, node);

    return node;
}

void p2p_node_free(struct p2p_node *node)
{
    if (!node) return;

    close_socket(&node->socket);

    zcl_mutex_lock(&node->cs_send);
    while (node->send_head) {
        struct send_segment *seg = node->send_head;
        node->send_head = seg->next;
        send_segment_free(seg);
    }
    zcl_mutex_unlock(&node->cs_send);

    zcl_mutex_lock(&node->cs_recv);
    for (size_t i = 0; i < node->recv_msg_count; i++)
        net_message_free(&node->recv_msgs[i]);
    node->recv_msg_count = 0;
    free(node->recv_msgs);
    node->recv_msgs = NULL;
    zcl_mutex_unlock(&node->cs_recv);

    free(node->addr_to_send);
    node->addr_to_send = NULL;
    free(node->inventory_to_send);
    node->inventory_to_send = NULL;
    free(node->inventory_known_hashes);
    node->inventory_known_hashes = NULL;
    free(node->askfor_set);
    node->askfor_set = NULL;
    free(node->askfor_map);
    node->askfor_map = NULL;

    if (node->pfilter) {
        bloom_filter_free(node->pfilter);
        free(node->pfilter);
        node->pfilter = NULL;
    }

    rolling_bloom_free(&node->addr_known);

    zcl_mutex_destroy(&node->cs_send);
    zcl_mutex_destroy(&node->cs_recv);
    zcl_mutex_destroy(&node->cs_inventory);
    zcl_mutex_destroy(&node->cs_filter);

    free(node);
}

void p2p_node_add_ref(struct p2p_node *node)
{
    node->ref_count++;
}

void p2p_node_release(struct p2p_node *node)
{
    node->ref_count--;
}

int p2p_node_get_ref(struct p2p_node *node)
{
    return node->ref_count;
}

void p2p_node_close_socket(struct p2p_node *node)
{
    node->disconnect = true;
    if (node->socket != ZCL_INVALID_SOCKET)
        close_socket(&node->socket);
}

bool p2p_node_receive_bytes(struct p2p_node *node, const char *data,
                             unsigned int nbytes,
                             const unsigned char msgstart[MESSAGE_START_SIZE])
{
    unsigned int orig_nbytes = nbytes;
    int msg_idx = 0;
    while (nbytes > 0) {
        if (node->recv_msg_count == 0 ||
            net_message_complete(&node->recv_msgs[node->recv_msg_count - 1])) {
            if (node->recv_msg_count >= node->recv_msg_cap) {
                size_t newcap = node->recv_msg_cap ? node->recv_msg_cap * 2 : 16;
                struct net_message *tmp = realloc(node->recv_msgs,
                                                   newcap * sizeof(*tmp));
                if (!tmp) return false;
                node->recv_msgs = tmp;
                node->recv_msg_cap = newcap;
            }
            net_message_init(&node->recv_msgs[node->recv_msg_count], msgstart);
            node->recv_msg_count++;
        }

        struct net_message *msg = &node->recv_msgs[node->recv_msg_count - 1];
        int handled;
        if (!msg->in_data)
            handled = net_message_read_header(msg, data, nbytes);
        else
            handled = net_message_read_data(msg, data, nbytes);

        if (handled < 0) {
            printf("  PARSE FAIL at msg_idx=%d offset=%u/%u in_data=%d "
                   "hdr_pos=%u data_pos=%u nMessageSize=%u "
                   "next4: %02x%02x%02x%02x\n",
                   msg_idx, orig_nbytes - nbytes, orig_nbytes,
                   msg->in_data, msg->hdr_pos, msg->data_pos,
                   msg->hdr.nMessageSize,
                   (unsigned char)data[0],
                   nbytes>1?(unsigned char)data[1]:0,
                   nbytes>2?(unsigned char)data[2]:0,
                   nbytes>3?(unsigned char)data[3]:0);
            return false;
        }

        if (msg->in_data && msg->hdr.nMessageSize > MAX_PROTOCOL_MESSAGE_LENGTH) {
            char dcmd[COMMAND_SIZE + 1];
            msg_header_get_command(&msg->hdr, dcmd, sizeof(dcmd));
            printf("Dropped oversized '%s' message: %u bytes > %u\n",
                   dcmd, msg->hdr.nMessageSize, MAX_PROTOCOL_MESSAGE_LENGTH);
            return false;
        }

        data += handled;
        nbytes -= (unsigned int)handled;
        msg_idx++;

        if (net_message_complete(msg)) {
            char dcmd[COMMAND_SIZE + 1];
            msg_header_get_command(&msg->hdr, dcmd, sizeof(dcmd));
            printf("  completed msg '%s' size=%u at offset=%u/%u\n",
                   dcmd, msg->hdr.nMessageSize,
                   orig_nbytes - nbytes, orig_nbytes);
            msg->time_usec = GetTimeMicros();
        }
    }
    return true;
}

void p2p_node_copy_stats(const struct p2p_node *node, struct node_stats *stats)
{
    memset(stats, 0, sizeof(*stats));
    stats->nodeid = node->id;
    stats->services = node->services;
    stats->last_send = node->last_send;
    stats->last_recv = node->last_recv;
    stats->time_connected = node->time_connected;
    stats->time_offset = node->time_offset;
    snprintf(stats->addr_name, sizeof(stats->addr_name), "%s", node->addr_name);
    stats->version = node->version;
    snprintf(stats->clean_sub_ver, sizeof(stats->clean_sub_ver), "%s",
             node->clean_sub_ver);
    stats->inbound = node->inbound;
    stats->starting_height = node->starting_height;
    stats->send_bytes = node->send_bytes;
    stats->recv_bytes = node->recv_bytes;
    stats->whitelisted = node->whitelisted;

    int64_t ping_wait = 0;
    if (node->ping_nonce_sent != 0 && node->ping_usec_start != 0)
        ping_wait = GetTimeMicros() - node->ping_usec_start;

    stats->ping_time = (double)node->ping_usec_time / 1e6;
    stats->ping_wait = (double)ping_wait / 1e6;

    if (net_addr_is_valid(&node->addr_local.addr)) {
        char buf[64];
        net_addr_to_string(&node->addr_local.addr, buf, sizeof(buf));
        snprintf(stats->addr_local, sizeof(stats->addr_local), "%s:%u",
                 buf, node->addr_local.port);
    }
}

void p2p_node_push_address(struct p2p_node *node, const struct net_address *addr)
{
    unsigned char key[NET_SERVICE_KEY_SIZE];
    net_service_get_key(&addr->svc, key);
    if (!net_addr_is_valid(&addr->svc.addr) ||
        rolling_bloom_contains(&node->addr_known, key, NET_SERVICE_KEY_SIZE))
        return;

    if (node->addr_to_send_count >= MAX_ADDR_TO_SEND) {
        uint64_t idx;
        GetRandBytes((unsigned char *)&idx, sizeof(idx));
        node->addr_to_send[idx % node->addr_to_send_count] = *addr;
    } else {
        if (node->addr_to_send_count >= node->addr_to_send_cap) {
            size_t newcap = node->addr_to_send_cap ? node->addr_to_send_cap * 2 : 64;
            struct net_address *tmp = realloc(node->addr_to_send,
                                               newcap * sizeof(*tmp));
            if (!tmp) return;
            node->addr_to_send = tmp;
            node->addr_to_send_cap = newcap;
        }
        node->addr_to_send[node->addr_to_send_count++] = *addr;
    }
}

void p2p_node_add_inventory_known(struct p2p_node *node, const struct inv_item *inv)
{
    zcl_mutex_lock(&node->cs_inventory);
    if (node->inventory_known_count >= node->inventory_known_cap) {
        size_t newcap = node->inventory_known_cap ? node->inventory_known_cap * 2 : 1024;
        if (newcap > MAX_INVENTORY_KNOWN) newcap = MAX_INVENTORY_KNOWN;
        if (node->inventory_known_count >= newcap) {
            memmove(node->inventory_known_hashes,
                    node->inventory_known_hashes + newcap / 2,
                    (newcap / 2) * sizeof(struct uint256));
            node->inventory_known_count = newcap / 2;
        } else {
            struct uint256 *tmp = realloc(node->inventory_known_hashes,
                                           newcap * sizeof(*tmp));
            if (!tmp) { zcl_mutex_unlock(&node->cs_inventory); return; }
            node->inventory_known_hashes = tmp;
            node->inventory_known_cap = newcap;
        }
    }
    node->inventory_known_hashes[node->inventory_known_count++] = inv->hash;
    zcl_mutex_unlock(&node->cs_inventory);
}

static bool inventory_known_contains(struct p2p_node *node,
                                      const struct uint256 *hash)
{
    for (size_t i = 0; i < node->inventory_known_count; i++)
        if (uint256_eq(&node->inventory_known_hashes[i], hash))
            return true;
    return false;
}

void p2p_node_push_inventory(struct p2p_node *node, const struct inv_item *inv)
{
    zcl_mutex_lock(&node->cs_inventory);
    if (!inventory_known_contains(node, &inv->hash)) {
        if (node->inventory_to_send_count >= node->inventory_to_send_cap) {
            size_t newcap = node->inventory_to_send_cap ?
                            node->inventory_to_send_cap * 2 : 256;
            struct inv_item *tmp = realloc(node->inventory_to_send,
                                            newcap * sizeof(*tmp));
            if (!tmp) { zcl_mutex_unlock(&node->cs_inventory); return; }
            node->inventory_to_send = tmp;
            node->inventory_to_send_cap = newcap;
        }
        node->inventory_to_send[node->inventory_to_send_count++] = *inv;
    }
    zcl_mutex_unlock(&node->cs_inventory);
}

/* --- message building (byte_stream based send buffer) --- */

static _Thread_local struct byte_stream tls_msg_stream;
static _Thread_local bool tls_msg_active = false;

bool p2p_node_begin_message(struct p2p_node *node, const char *command,
                             const unsigned char msgstart[MESSAGE_START_SIZE])
{
    zcl_mutex_lock(&node->cs_send);
    stream_init(&tls_msg_stream, 256);
    tls_msg_active = true;

    struct msg_header hdr;
    msg_header_init_full(&hdr, msgstart, command, 0);
    stream_write(&tls_msg_stream, (const uint8_t *)&hdr, MSG_HEADER_SIZE);
    return true;
}

void p2p_node_write_message_data(struct p2p_node *node,
                                  const uint8_t *data, size_t len)
{
    (void)node;
    if (tls_msg_active)
        stream_write(&tls_msg_stream, data, len);
}

void p2p_node_abort_message(struct p2p_node *node)
{
    (void)node;
    if (tls_msg_active) {
        stream_free(&tls_msg_stream);
        tls_msg_active = false;
    }
    zcl_mutex_unlock(&node->cs_send);
}

bool p2p_node_end_message(struct p2p_node *node)
{
    if (!tls_msg_active) {
        zcl_mutex_unlock(&node->cs_send);
        return false;
    }

    size_t total = tls_msg_stream.size;
    if (total == 0 || tls_msg_stream.error) {
        stream_free(&tls_msg_stream);
        tls_msg_active = false;
        zcl_mutex_unlock(&node->cs_send);
        return false;
    }

    uint8_t *buf = tls_msg_stream.data;

    unsigned int payload_size = (unsigned int)(total - MSG_HEADER_SIZE);
    buf[MESSAGE_START_SIZE + COMMAND_SIZE] = (uint8_t)(payload_size & 0xff);
    buf[MESSAGE_START_SIZE + COMMAND_SIZE + 1] = (uint8_t)((payload_size >> 8) & 0xff);
    buf[MESSAGE_START_SIZE + COMMAND_SIZE + 2] = (uint8_t)((payload_size >> 16) & 0xff);
    buf[MESSAGE_START_SIZE + COMMAND_SIZE + 3] = (uint8_t)((payload_size >> 24) & 0xff);

    struct uint256 hash;
    hash256(buf + MSG_HEADER_SIZE, total - MSG_HEADER_SIZE, hash.data);
    memcpy(buf + MESSAGE_START_SIZE + COMMAND_SIZE + 4, hash.data, 4);

    struct send_segment *seg = send_segment_create(buf, total);
    stream_free(&tls_msg_stream);
    tls_msg_active = false;

    if (!seg) {
        zcl_mutex_unlock(&node->cs_send);
        return false;
    }

    if (node->send_tail) {
        node->send_tail->next = seg;
        node->send_tail = seg;
    } else {
        node->send_head = seg;
        node->send_tail = seg;
    }
    node->send_size += seg->size;

    if (node->send_head == seg)
        socket_send_data(node);

    zcl_mutex_unlock(&node->cs_send);
    return true;
}

/* --- socket_send_data --- */

void socket_send_data(struct p2p_node *node)
{
    while (node->send_head) {
        struct send_segment *seg = node->send_head;
        size_t remain = seg->size - node->send_offset;

        ssize_t sent = send(node->socket,
                            (const char *)(seg->data + node->send_offset),
                            remain, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent > 0) {
            node->last_send = GetTime();
            node->send_bytes += (uint64_t)sent;
            node->send_offset += (size_t)sent;

            if (node->send_offset >= seg->size) {
                node->send_head = seg->next;
                if (!node->send_head)
                    node->send_tail = NULL;
                node->send_size -= seg->size;
                node->send_offset = 0;
                send_segment_free(seg);
            } else {
                break;
            }
        } else {
            if (sent < 0) {
                int err = errno;
                if (err != EAGAIN && err != EWOULDBLOCK && err != EINTR && err != EINPROGRESS)
                    p2p_node_close_socket(node);
            }
            break;
        }
    }
}

/* --- net_manager --- */

void net_manager_init(struct net_manager *nm)
{
    memset(nm, 0, sizeof(*nm));
    nm->discover = true;
    nm->listen = true;
    nm->local_services = NODE_NETWORK;
    nm->max_connections = DEFAULT_MAX_PEER_CONNECTIONS;
    nm->stop_requested = false;

    addrman_init(&nm->addrman);

    zcl_mutex_init(&nm->cs_nodes);
    zcl_mutex_init(&nm->cs_local_host);
    zcl_mutex_init(&nm->cs_banned);
    zcl_mutex_init(&nm->cs_last_node_id);
    zcl_mutex_init(&nm->cs_total_bytes_recv);
    zcl_mutex_init(&nm->cs_total_bytes_sent);
    zcl_cond_init(&nm->msg_handler_cond);
    zcl_mutex_init(&nm->msg_handler_mutex);
}

void net_manager_free(struct net_manager *nm)
{
    for (size_t i = 0; i < nm->num_listen_sockets; i++)
        if (nm->listen_sockets[i].socket != ZCL_INVALID_SOCKET)
            close_socket(&nm->listen_sockets[i].socket);
    free(nm->listen_sockets);

    for (size_t i = 0; i < nm->num_nodes; i++)
        p2p_node_free(nm->nodes[i]);
    free(nm->nodes);

    for (size_t i = 0; i < nm->num_disconnected; i++)
        p2p_node_free(nm->nodes_disconnected[i]);
    free(nm->nodes_disconnected);

    free(nm->local_hosts);
    free(nm->local_host_info);
    free(nm->banned);
    free(nm->whitelisted);
    free(nm->whitelist_prefix);

    addrman_free(&nm->addrman);

    zcl_mutex_destroy(&nm->cs_nodes);
    zcl_mutex_destroy(&nm->cs_local_host);
    zcl_mutex_destroy(&nm->cs_banned);
    zcl_mutex_destroy(&nm->cs_last_node_id);
    zcl_mutex_destroy(&nm->cs_total_bytes_recv);
    zcl_mutex_destroy(&nm->cs_total_bytes_sent);
    zcl_cond_destroy(&nm->msg_handler_cond);
    zcl_mutex_destroy(&nm->msg_handler_mutex);
}

/* --- find node --- */

struct p2p_node *find_node_by_addr(struct net_manager *nm,
                                    const struct net_addr *addr)
{
    zcl_mutex_lock(&nm->cs_nodes);
    for (size_t i = 0; i < nm->num_nodes; i++) {
        if (net_addr_eq(&nm->nodes[i]->addr.svc.addr, addr)) {
            zcl_mutex_unlock(&nm->cs_nodes);
            return nm->nodes[i];
        }
    }
    zcl_mutex_unlock(&nm->cs_nodes);
    return NULL;
}

struct p2p_node *find_node_by_service(struct net_manager *nm,
                                       const struct net_service *addr)
{
    zcl_mutex_lock(&nm->cs_nodes);
    for (size_t i = 0; i < nm->num_nodes; i++) {
        if (net_addr_eq(&nm->nodes[i]->addr.svc.addr, &addr->addr) &&
            nm->nodes[i]->addr.svc.port == addr->port) {
            zcl_mutex_unlock(&nm->cs_nodes);
            return nm->nodes[i];
        }
    }
    zcl_mutex_unlock(&nm->cs_nodes);
    return NULL;
}

/* --- add node to manager --- */

static bool nm_add_node(struct net_manager *nm, struct p2p_node *node)
{
    if (nm->num_nodes >= nm->nodes_cap) {
        size_t newcap = nm->nodes_cap ? nm->nodes_cap * 2 : 32;
        struct p2p_node **tmp = realloc(nm->nodes, newcap * sizeof(*tmp));
        if (!tmp) return false;
        nm->nodes = tmp;
        nm->nodes_cap = newcap;
    }
    nm->nodes[nm->num_nodes++] = node;
    return true;
}

/* --- connect_node --- */

struct p2p_node *connect_node(struct net_manager *nm,
                               struct net_address *addr_connect,
                               const char *dest)
{
    if (!dest) {
        if (is_local(nm, &addr_connect->svc))
            return NULL;

        struct p2p_node *existing = find_node_by_service(nm, &addr_connect->svc);
        if (existing) {
            p2p_node_add_ref(existing);
            return existing;
        }
    }

    zcl_socket_t sock;

    if (!connect_socket_directly(&addr_connect->svc, &sock, DEFAULT_CONNECT_TIMEOUT)) {
        char addr_str[64];
        net_service_to_string(&addr_connect->svc, addr_str, sizeof(addr_str));
        printf("connect_node: failed to connect to %s\n", addr_str);
        fflush(stdout);
        return NULL;
    }

    struct p2p_node *node = p2p_node_create(nm, sock, addr_connect,
                                             dest ? dest : "", false);
    if (!node) {
        close_socket(&sock);
        return NULL;
    }
    p2p_node_add_ref(node);

    zcl_mutex_lock(&nm->cs_nodes);
    nm_add_node(nm, node);
    zcl_mutex_unlock(&nm->cs_nodes);

    node->time_connected = GetTime();

    char addr_str[64];
    net_service_to_string(&addr_connect->svc, addr_str, sizeof(addr_str));
    printf("Connected to %s\n", addr_str);
    fflush(stdout);
    return node;
}

/* --- ban management --- */

bool is_banned(struct net_manager *nm, const struct net_addr *addr)
{
    zcl_mutex_lock(&nm->cs_banned);
    int64_t now = GetTime();
    for (size_t i = 0; i < nm->num_banned; i++) {
        if (net_addr_eq(&nm->banned[i].addr, addr) && now < nm->banned[i].ban_until) {
            zcl_mutex_unlock(&nm->cs_banned);
            return true;
        }
    }
    zcl_mutex_unlock(&nm->cs_banned);
    return false;
}

void ban_addr(struct net_manager *nm, const struct net_addr *addr,
              int64_t ban_offset, bool since_epoch)
{
    int64_t ban_time = GetTime() + 24 * 60 * 60;
    if (ban_offset > 0)
        ban_time = (since_epoch ? 0 : GetTime()) + ban_offset;

    zcl_mutex_lock(&nm->cs_banned);
    for (size_t i = 0; i < nm->num_banned; i++) {
        if (net_addr_eq(&nm->banned[i].addr, addr)) {
            if (nm->banned[i].ban_until < ban_time)
                nm->banned[i].ban_until = ban_time;
            zcl_mutex_unlock(&nm->cs_banned);
            return;
        }
    }

    if (nm->num_banned >= nm->banned_cap) {
        size_t newcap = nm->banned_cap ? nm->banned_cap * 2 : 64;
        struct ban_entry *tmp = realloc(nm->banned, newcap * sizeof(*tmp));
        if (!tmp) { zcl_mutex_unlock(&nm->cs_banned); return; }
        nm->banned = tmp;
        nm->banned_cap = newcap;
    }
    nm->banned[nm->num_banned].addr = *addr;
    nm->banned[nm->num_banned].prefix_len = net_addr_is_ipv4(addr) ? 32 : 128;
    nm->banned[nm->num_banned].ban_until = ban_time;
    nm->num_banned++;
    zcl_mutex_unlock(&nm->cs_banned);
}

bool unban_addr(struct net_manager *nm, const struct net_addr *addr)
{
    zcl_mutex_lock(&nm->cs_banned);
    for (size_t i = 0; i < nm->num_banned; i++) {
        if (net_addr_eq(&nm->banned[i].addr, addr)) {
            nm->banned[i] = nm->banned[nm->num_banned - 1];
            nm->num_banned--;
            zcl_mutex_unlock(&nm->cs_banned);
            return true;
        }
    }
    zcl_mutex_unlock(&nm->cs_banned);
    return false;
}

void clear_banned(struct net_manager *nm)
{
    zcl_mutex_lock(&nm->cs_banned);
    nm->num_banned = 0;
    zcl_mutex_unlock(&nm->cs_banned);
}

/* --- local address management --- */

static int find_local_host(struct net_manager *nm, const struct net_addr *addr)
{
    for (size_t i = 0; i < nm->num_local_hosts; i++)
        if (net_addr_eq(&nm->local_hosts[i], addr))
            return (int)i;
    return -1;
}

bool add_local(struct net_manager *nm, const struct net_service *addr, int score)
{
    if (!net_addr_is_routable(&addr->addr))
        return false;

    if (!nm->discover && score < LOCAL_MANUAL)
        return false;

    zcl_mutex_lock(&nm->cs_local_host);

    enum zcl_network net = net_addr_get_network(&addr->addr);
    if (nm->limited[net]) {
        zcl_mutex_unlock(&nm->cs_local_host);
        return false;
    }

    int idx = find_local_host(nm, &addr->addr);
    if (idx >= 0) {
        if (score >= nm->local_host_info[idx].score) {
            nm->local_host_info[idx].score = score + 1;
            nm->local_host_info[idx].port = addr->port;
        }
    } else {
        if (nm->num_local_hosts >= nm->local_hosts_cap) {
            size_t newcap = nm->local_hosts_cap ? nm->local_hosts_cap * 2 : 8;
            struct net_addr *ha = realloc(nm->local_hosts, newcap * sizeof(*ha));
            struct local_service_info *hi = realloc(nm->local_host_info,
                                                      newcap * sizeof(*hi));
            if (!ha || !hi) {
                zcl_mutex_unlock(&nm->cs_local_host);
                return false;
            }
            nm->local_hosts = ha;
            nm->local_host_info = hi;
            nm->local_hosts_cap = newcap;
        }
        size_t n = nm->num_local_hosts;
        nm->local_hosts[n] = addr->addr;
        nm->local_host_info[n].score = score;
        nm->local_host_info[n].port = addr->port;
        nm->num_local_hosts++;
    }

    zcl_mutex_unlock(&nm->cs_local_host);
    return true;
}

bool remove_local(struct net_manager *nm, const struct net_service *addr)
{
    zcl_mutex_lock(&nm->cs_local_host);
    int idx = find_local_host(nm, &addr->addr);
    if (idx >= 0) {
        nm->local_hosts[idx] = nm->local_hosts[nm->num_local_hosts - 1];
        nm->local_host_info[idx] = nm->local_host_info[nm->num_local_hosts - 1];
        nm->num_local_hosts--;
    }
    zcl_mutex_unlock(&nm->cs_local_host);
    return idx >= 0;
}

bool is_local(struct net_manager *nm, const struct net_service *addr)
{
    zcl_mutex_lock(&nm->cs_local_host);
    int idx = find_local_host(nm, &addr->addr);
    zcl_mutex_unlock(&nm->cs_local_host);
    return idx >= 0;
}

bool is_reachable_net(struct net_manager *nm, enum zcl_network net)
{
    zcl_mutex_lock(&nm->cs_local_host);
    bool result = !nm->limited[net];
    zcl_mutex_unlock(&nm->cs_local_host);
    return result;
}

bool is_reachable_addr(struct net_manager *nm, const struct net_addr *a)
{
    enum zcl_network net = net_addr_get_network(a);
    return is_reachable_net(nm, net);
}

void set_limited(struct net_manager *nm, enum zcl_network net, bool limited)
{
    if (net == NET_UNROUTABLE) return;
    zcl_mutex_lock(&nm->cs_local_host);
    nm->limited[net] = limited;
    zcl_mutex_unlock(&nm->cs_local_host);
}

/* --- bind/listen --- */

bool bind_listen_port(struct net_manager *nm, const struct net_service *addr,
                      bool whitelisted)
{
    struct sockaddr_storage ss;
    socklen_t sslen = sizeof(ss);
    memset(&ss, 0, sizeof(ss));

    if (net_addr_is_ipv4(&addr->addr)) {
        struct sockaddr_in *s4 = (struct sockaddr_in *)&ss;
        s4->sin_family = AF_INET;
        s4->sin_port = htons(addr->port);
        memcpy(&s4->sin_addr, addr->addr.ip + 12, 4);
        sslen = sizeof(*s4);
    } else {
        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&ss;
        s6->sin6_family = AF_INET6;
        s6->sin6_port = htons(addr->port);
        memcpy(&s6->sin6_addr, addr->addr.ip, 16);
        sslen = sizeof(*s6);
    }

    zcl_socket_t sock = socket(((struct sockaddr *)&ss)->sa_family,
                                SOCK_STREAM, IPPROTO_TCP);
    if (sock == ZCL_INVALID_SOCKET)
        return false;

    int one = 1;
#ifndef _WIN32
#ifdef SO_NOSIGPIPE
    setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#else
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
#endif

    if (!set_socket_nonblocking(sock, true)) {
        close_socket(&sock);
        return false;
    }

    if (!net_addr_is_ipv4(&addr->addr)) {
#ifdef IPV6_V6ONLY
        setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
#endif
    }

    if (bind(sock, (struct sockaddr *)&ss, sslen) == ZCL_SOCKET_ERROR) {
        close_socket(&sock);
        return false;
    }

    if (listen(sock, SOMAXCONN) == ZCL_SOCKET_ERROR) {
        close_socket(&sock);
        return false;
    }

    if (nm->num_listen_sockets >= nm->listen_sockets_cap) {
        size_t newcap = nm->listen_sockets_cap ? nm->listen_sockets_cap * 2 : 4;
        struct listen_socket *tmp = realloc(nm->listen_sockets, newcap * sizeof(*tmp));
        if (!tmp) { close_socket(&sock); return false; }
        nm->listen_sockets = tmp;
        nm->listen_sockets_cap = newcap;
    }
    nm->listen_sockets[nm->num_listen_sockets].socket = sock;
    nm->listen_sockets[nm->num_listen_sockets].whitelisted = whitelisted;
    nm->num_listen_sockets++;

    if (net_addr_is_routable(&addr->addr) && nm->discover && !whitelisted)
        add_local(nm, addr, LOCAL_BIND);

    return true;
}

/* --- accept connection --- */

bool accept_connection(struct net_manager *nm, const struct listen_socket *ls)
{
    struct sockaddr_storage ss;
    socklen_t sslen = sizeof(ss);
    zcl_socket_t sock = accept(ls->socket, (struct sockaddr *)&ss, &sslen);

    if (sock == ZCL_INVALID_SOCKET)
        return false;

    struct net_address addr;
    net_address_init(&addr);

    if (ss.ss_family == AF_INET) {
        struct sockaddr_in *s4 = (struct sockaddr_in *)&ss;
        memset(addr.svc.addr.ip, 0, 10);
        memset(addr.svc.addr.ip + 10, 0xff, 2);
        memcpy(addr.svc.addr.ip + 12, &s4->sin_addr, 4);
        addr.svc.port = ntohs(s4->sin_port);
    } else if (ss.ss_family == AF_INET6) {
        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&ss;
        memcpy(addr.svc.addr.ip, &s6->sin6_addr, 16);
        addr.svc.port = ntohs(s6->sin6_port);
    }

    bool is_whitelisted = ls->whitelisted;

    if (is_banned(nm, &addr.svc.addr) && !is_whitelisted) {
        close_socket(&sock);
        return false;
    }

    int inbound_count = 0;
    int max_inbound = nm->max_connections - MAX_OUTBOUND_CONNECTIONS;
    zcl_mutex_lock(&nm->cs_nodes);
    for (size_t i = 0; i < nm->num_nodes; i++)
        if (nm->nodes[i]->inbound)
            inbound_count++;
    zcl_mutex_unlock(&nm->cs_nodes);

    if (inbound_count >= max_inbound) {
        close_socket(&sock);
        return false;
    }

    int one = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    struct p2p_node *node = p2p_node_create(nm, sock, &addr, "", true);
    if (!node) {
        close_socket(&sock);
        return false;
    }
    p2p_node_add_ref(node);
    node->whitelisted = is_whitelisted;

    zcl_mutex_lock(&nm->cs_nodes);
    nm_add_node(nm, node);
    zcl_mutex_unlock(&nm->cs_nodes);

    return true;
}

/* --- socket handler loop (one iteration) --- */

void net_socket_handler_step(struct net_manager *nm)
{
    /* disconnect flagged nodes */
    zcl_mutex_lock(&nm->cs_nodes);
    for (size_t i = 0; i < nm->num_nodes; ) {
        struct p2p_node *node = nm->nodes[i];
        if (node->disconnect ||
            (p2p_node_get_ref(node) <= 0 && node->recv_msg_count == 0 &&
             node->send_size == 0)) {
            nm->nodes[i] = nm->nodes[nm->num_nodes - 1];
            nm->num_nodes--;

            p2p_node_close_socket(node);

            if (node->network_node || node->inbound)
                p2p_node_release(node);

            if (nm->num_disconnected >= nm->disconnected_cap) {
                size_t newcap = nm->disconnected_cap ?
                                nm->disconnected_cap * 2 : 32;
                struct p2p_node **tmp = realloc(nm->nodes_disconnected,
                                                 newcap * sizeof(*tmp));
                if (tmp) {
                    nm->nodes_disconnected = tmp;
                    nm->disconnected_cap = newcap;
                }
            }
            if (nm->num_disconnected < nm->disconnected_cap)
                nm->nodes_disconnected[nm->num_disconnected++] = node;
        } else {
            i++;
        }
    }
    zcl_mutex_unlock(&nm->cs_nodes);

    /* delete fully disconnected nodes with no refs */
    for (size_t i = 0; i < nm->num_disconnected; ) {
        struct p2p_node *node = nm->nodes_disconnected[i];
        if (p2p_node_get_ref(node) <= 0) {
            if (nm->signals.finalize_node)
                nm->signals.finalize_node(nm->signals.ctx, node->id);
            p2p_node_free(node);
            nm->nodes_disconnected[i] =
                nm->nodes_disconnected[nm->num_disconnected - 1];
            nm->num_disconnected--;
        } else {
            i++;
        }
    }

    /* select on sockets */
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 50000;

    fd_set fdset_recv, fdset_send, fdset_error;
    FD_ZERO(&fdset_recv);
    FD_ZERO(&fdset_send);
    FD_ZERO(&fdset_error);
    int max_fd = 0;
    bool have_fds = false;

    for (size_t i = 0; i < nm->num_listen_sockets; i++) {
        int fd = nm->listen_sockets[i].socket;
        FD_SET(fd, &fdset_recv);
        if (fd > max_fd) max_fd = fd;
        have_fds = true;
    }

    zcl_mutex_lock(&nm->cs_nodes);
    for (size_t i = 0; i < nm->num_nodes; i++) {
        struct p2p_node *node = nm->nodes[i];
        if (node->socket == ZCL_INVALID_SOCKET) continue;

        int fd = node->socket;
        FD_SET(fd, &fdset_error);
        if (fd > max_fd) max_fd = fd;
        have_fds = true;

        if (zcl_mutex_trylock(&node->cs_send)) {
            if (node->send_head) {
                FD_SET(fd, &fdset_send);
                zcl_mutex_unlock(&node->cs_send);
                continue;
            }
            zcl_mutex_unlock(&node->cs_send);
        }

        if (zcl_mutex_trylock(&node->cs_recv)) {
            if (node->recv_msg_count == 0 ||
                !net_message_complete(&node->recv_msgs[0]))
                FD_SET(fd, &fdset_recv);
            zcl_mutex_unlock(&node->cs_recv);
        }
    }
    zcl_mutex_unlock(&nm->cs_nodes);

    select(have_fds ? max_fd + 1 : 0, &fdset_recv, &fdset_send, &fdset_error,
           &timeout);

    /* accept connections */
    for (size_t i = 0; i < nm->num_listen_sockets; i++) {
        if (nm->listen_sockets[i].socket != ZCL_INVALID_SOCKET &&
            FD_ISSET(nm->listen_sockets[i].socket, &fdset_recv))
            accept_connection(nm, &nm->listen_sockets[i]);
    }

    /* service each socket */
    zcl_mutex_lock(&nm->cs_nodes);
    size_t n = nm->num_nodes;
    struct p2p_node **copy = malloc(n * sizeof(*copy));
    if (copy) {
        memcpy(copy, nm->nodes, n * sizeof(*copy));
        for (size_t i = 0; i < n; i++)
            p2p_node_add_ref(copy[i]);
    }
    zcl_mutex_unlock(&nm->cs_nodes);

    if (copy) {
        for (size_t i = 0; i < n; i++) {
            struct p2p_node *node = copy[i];
            if (node->socket == ZCL_INVALID_SOCKET) continue;

            /* receive */
            if (FD_ISSET(node->socket, &fdset_recv) ||
                FD_ISSET(node->socket, &fdset_error)) {
                if (zcl_mutex_trylock(&node->cs_recv)) {
                    char buf[0x10000];
                    ssize_t nrecv = recv(node->socket, buf, sizeof(buf),
                                          MSG_DONTWAIT);
                    if (nrecv > 0) {
                        if (!p2p_node_receive_bytes(node, buf, (unsigned int)nrecv,
                                                     nm->message_start)) {
                            printf("Message parse error from %s (recv %zd bytes)\n",
                                   node->addr_name, nrecv);
                            p2p_node_close_socket(node);
                        }
                        node->last_recv = GetTime();
                        node->recv_bytes += (uint64_t)nrecv;
                    } else if (nrecv == 0) {
                        p2p_node_close_socket(node);
                    } else {
                        int err = errno;
                        if (err != EAGAIN && err != EWOULDBLOCK &&
                            err != EINTR && err != EINPROGRESS)
                            p2p_node_close_socket(node);
                    }
                    zcl_mutex_unlock(&node->cs_recv);
                }
            }

            /* send */
            if (node->socket != ZCL_INVALID_SOCKET &&
                FD_ISSET(node->socket, &fdset_send)) {
                if (zcl_mutex_trylock(&node->cs_send)) {
                    socket_send_data(node);
                    zcl_mutex_unlock(&node->cs_send);
                }
            }

            /* inactivity */
            int64_t now = GetTime();
            if (now - node->time_connected > 60) {
                if (node->last_recv == 0 || node->last_send == 0) {
                    node->disconnect = true;
                } else if (now - node->last_send > TIMEOUT_INTERVAL) {
                    node->disconnect = true;
                } else if (now - node->last_recv >
                           (node->version > BIP0031_VERSION ?
                            TIMEOUT_INTERVAL : 90 * 60)) {
                    node->disconnect = true;
                } else if (node->ping_nonce_sent &&
                           node->ping_usec_start +
                           (int64_t)TIMEOUT_INTERVAL * 1000000 <
                           GetTimeMicros()) {
                    node->disconnect = true;
                }
            }
        }

        zcl_mutex_lock(&nm->cs_nodes);
        for (size_t i = 0; i < n; i++)
            p2p_node_release(copy[i]);
        zcl_mutex_unlock(&nm->cs_nodes);

        free(copy);
    }
}

/* --- addr db --- */

bool addr_db_write(const struct net_manager *nm, const char *datadir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/peers.dat", datadir);

    struct byte_stream s;
    stream_init(&s, 65536);

    stream_write(&s, nm->message_start, MESSAGE_START_SIZE);

    if (!addrman_serialize(&nm->addrman, &s)) {
        stream_free(&s);
        return false;
    }

    struct uint256 hash;
    hash256(s.data, s.size, hash.data);
    stream_write(&s, hash.data, 32);

    FILE *f = fopen(path, "wb");
    if (!f) { stream_free(&s); return false; }
    size_t written = fwrite(s.data, 1, s.size, f);
    fclose(f);
    stream_free(&s);

    return written == s.size || written > 0;
}

bool addr_db_read(struct net_manager *nm, const char *datadir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/peers.dat", datadir);

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size < (long)(MESSAGE_START_SIZE + 32)) {
        fclose(f);
        return false;
    }

    uint8_t *buf = malloc((size_t)file_size);
    if (!buf) { fclose(f); return false; }
    if (fread(buf, 1, (size_t)file_size, f) != (size_t)file_size) {
        free(buf);
        fclose(f);
        return false;
    }
    fclose(f);

    size_t data_size = (size_t)file_size - 32;
    struct uint256 stored_hash;
    memcpy(stored_hash.data, buf + data_size, 32);

    struct uint256 computed_hash;
    hash256(buf, data_size, computed_hash.data);

    if (!uint256_eq(&stored_hash, &computed_hash)) {
        free(buf);
        return false;
    }

    if (memcmp(buf, nm->message_start, MESSAGE_START_SIZE) != 0) {
        free(buf);
        return false;
    }

    struct byte_stream s;
    stream_init_from_data(&s, buf + MESSAGE_START_SIZE,
                          data_size - MESSAGE_START_SIZE);

    bool ok = addrman_deserialize(&nm->addrman, &s);
    stream_free(&s);
    free(buf);
    return ok;
}

unsigned int receive_flood_size(void) { return 5 * 1000 * 1000; }
unsigned int send_buffer_size(void) { return 1 * 1000 * 1000; }
