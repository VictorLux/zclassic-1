/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "net/msgprocessor.h"
#include "net/version.h"
#include "net/p2p_message.h"
#include "core/hash.h"
#include "core/random.h"
#include "core/serialize.h"
#include "consensus/upgrades.h"
#include "consensus/validation.h"
#include "validation/check_transaction.h"
#include "validation/process_block.h"
#include "storage/disk_block_io.h"
#include "util/timedata.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static void push_getheaders(struct msg_processor *mp, struct p2p_node *node);

void msg_processor_init(struct msg_processor *mp,
                         struct main_state *ms,
                         struct tx_mempool *mempool,
                         struct coins_view_cache *coins_tip,
                         const struct chain_params *params,
                         const char *datadir,
                         struct net_manager *net_mgr)
{
    mp->main_state = ms;
    mp->mempool = mempool;
    mp->coins_tip = coins_tip;
    mp->params = params;
    mp->datadir = datadir;
    mp->net_mgr = net_mgr;
}

int msg_get_height(void *ctx)
{
    struct msg_processor *mp = (struct msg_processor *)ctx;
    return active_chain_height(&mp->main_state->chain_active);
}

static void push_version(struct msg_processor *mp, struct p2p_node *node)
{
    struct version_message ver;
    version_message_init(&ver);
    ver.protocol_version = PROTOCOL_VERSION;
    ver.services = NODE_NETWORK;
    ver.timestamp = (int64_t)time(NULL);
    ver.addr_recv = node->addr;
    ver.nonce = GetRand(UINT64_MAX);
    snprintf(ver.sub_version, sizeof(ver.sub_version),
             "/ZClassic-C23:1.0.0/");
    ver.start_height = active_chain_height(&mp->main_state->chain_active);
    ver.relay = true;

    struct byte_stream s;
    stream_init(&s, 256);
    version_message_serialize(&ver, &s);

    p2p_node_begin_message(node, "version", mp->params->pchMessageStart);
    p2p_node_write_message_data(node, s.data, s.size);
    p2p_node_end_message(node);
    stream_free(&s);
}

static void push_verack(struct msg_processor *mp, struct p2p_node *node)
{
    p2p_node_begin_message(node, "verack", mp->params->pchMessageStart);
    p2p_node_end_message(node);
}

static bool process_version(struct msg_processor *mp, struct p2p_node *node,
                             struct byte_stream *s)
{
    if (node->version != 0) {
        printf("Peer %s: duplicate version message\n", node->addr_name);
        return false;
    }

    struct version_message ver;
    version_message_init(&ver);
    if (!version_message_deserialize(&ver, s))
        return false;

    if (ver.protocol_version < MIN_PEER_PROTO_VERSION) {
        printf("Peer %s: protocol version %d too old (min %d)\n",
               node->addr_name, ver.protocol_version, MIN_PEER_PROTO_VERSION);
        node->disconnect = true;
        return false;
    }

    node->version = ver.protocol_version;
    node->services = ver.services;
    strncpy(node->sub_ver, ver.sub_version, MAX_SUBVERSION_LENGTH - 1);
    node->sub_ver[MAX_SUBVERSION_LENGTH - 1] = '\0';
    strncpy(node->clean_sub_ver, ver.sub_version, MAX_SUBVERSION_LENGTH - 1);
    node->clean_sub_ver[MAX_SUBVERSION_LENGTH - 1] = '\0';
    node->starting_height = ver.start_height;
    node->time_offset = ver.timestamp - (int64_t)time(NULL);
    node->relay_txes = ver.relay;

    if (!node->inbound) {
        AddTimeData((const unsigned char *)node->addr_name,
                    (int)strlen(node->addr_name), node->time_offset);
    }

    push_verack(mp, node);

    if (node->inbound)
        push_version(mp, node);

    node->successfully_connected = true;

    /* Ask outbound peers for their address list */
    if (!node->inbound && !node->get_addr) {
        p2p_node_begin_message(node, "getaddr", mp->params->pchMessageStart);
        p2p_node_end_message(node);
        node->get_addr = true;
    }

    printf("Peer %s: version=%d subver=%s height=%d\n",
           node->addr_name, node->version, node->sub_ver,
           node->starting_height);
    return true;
}

static bool process_verack(struct msg_processor *mp, struct p2p_node *node)
{
    (void)mp;
    node->recv_version = PROTOCOL_VERSION;
    printf("Peer %s: verack received\n", node->addr_name);
    return true;
}

static bool process_ping(struct msg_processor *mp, struct p2p_node *node,
                          struct byte_stream *s)
{
    uint64_t nonce = 0;
    if (node->version >= BIP0031_VERSION) {
        if (!stream_read_u64_le(s, &nonce))
            return false;
    }

    if (node->version >= BIP0031_VERSION) {
        struct byte_stream reply;
        stream_init(&reply, 8);
        stream_write_u64_le(&reply, nonce);

        p2p_node_begin_message(node, "pong", mp->params->pchMessageStart);
        p2p_node_write_message_data(node, reply.data, reply.size);
        p2p_node_end_message(node);
        stream_free(&reply);
    }
    return true;
}

static bool process_pong(struct p2p_node *node, struct byte_stream *s)
{
    uint64_t nonce = 0;
    if (!stream_read_u64_le(s, &nonce))
        return false;

    if (node->ping_nonce_sent != 0 && nonce == node->ping_nonce_sent) {
        int64_t now = (int64_t)time(NULL) * 1000000;
        int64_t rtt = now - node->ping_usec_start;
        if (rtt > 0) {
            node->ping_usec_time = rtt;
            if (node->min_ping_usec_time == 0 || rtt < node->min_ping_usec_time)
                node->min_ping_usec_time = rtt;
        }
        node->ping_nonce_sent = 0;
    }
    return true;
}

static bool process_addr(struct msg_processor *mp, struct p2p_node *node,
                          struct byte_stream *s)
{
    uint64_t count;
    if (!stream_read_compact_size(s, &count))
        return false;

    if (count > MAX_ADDR_TO_SEND) {
        printf("Peer %s: addr message too large (%llu)\n",
               node->addr_name, (unsigned long long)count);
        node->disconnect = true;
        return false;
    }

    struct net_addr source;
    net_addr_init(&source);
    memcpy(source.ip, node->addr.svc.addr.ip, 16);

    for (uint64_t i = 0; i < count; i++) {
        struct net_address addr;
        net_address_init(&addr);
        if (!net_address_deserialize(&addr, s, true))
            return false;

        if (mp->net_mgr)
            addrman_add(&mp->net_mgr->addrman, &addr, &source, 0);
    }
    return true;
}

static bool process_getblocks(struct msg_processor *mp, struct p2p_node *node,
                               struct byte_stream *s)
{
    struct block_locator locator;
    block_locator_init(&locator);
    if (!block_locator_deserialize(&locator, s)) {
        block_locator_free(&locator);
        return false;
    }

    struct uint256 hash_stop;
    if (!stream_read(s, hash_stop.data, 32)) {
        block_locator_free(&locator);
        return false;
    }

    struct block_index *pindex = NULL;
    struct active_chain *chain = &mp->main_state->chain_active;

    for (size_t i = 0; i < locator.num_hashes; i++) {
        struct block_index *found = block_map_find(
            &mp->main_state->map_block_index, &locator.vhave[i]);
        if (found && active_chain_contains(chain, found)) {
            pindex = found;
            break;
        }
    }
    block_locator_free(&locator);

    if (!pindex)
        pindex = active_chain_at(chain, 0);

    int limit = 500;
    struct block_index *tip = active_chain_tip(chain);

    if (pindex)
        pindex = active_chain_at(chain, pindex->nHeight + 1);

    for (; pindex && limit > 0; pindex = active_chain_at(chain, pindex->nHeight + 1)) {
        if (!pindex || !pindex->phashBlock)
            break;

        struct inv_item inv;
        inv_item_init_typed(&inv, MSG_BLOCK, pindex->phashBlock);
        p2p_node_push_inventory(node, &inv);
        limit--;

        if (!uint256_is_null(&hash_stop) &&
            uint256_eq(pindex->phashBlock, &hash_stop))
            break;

        if (pindex == tip)
            break;
    }

    return true;
}

static bool process_getheaders(struct msg_processor *mp, struct p2p_node *node,
                                struct byte_stream *s)
{
    struct block_locator locator;
    block_locator_init(&locator);
    if (!block_locator_deserialize(&locator, s)) {
        block_locator_free(&locator);
        return false;
    }

    struct uint256 hash_stop;
    if (!stream_read(s, hash_stop.data, 32)) {
        block_locator_free(&locator);
        return false;
    }

    struct active_chain *chain = &mp->main_state->chain_active;
    struct block_index *pindex = NULL;

    if (locator.num_hashes == 0) {
        pindex = block_map_find(&mp->main_state->map_block_index, &hash_stop);
        if (!pindex) {
            block_locator_free(&locator);
            return true;
        }
    } else {
        for (size_t i = 0; i < locator.num_hashes; i++) {
            struct block_index *found = block_map_find(
                &mp->main_state->map_block_index, &locator.vhave[i]);
            if (found && active_chain_contains(chain, found)) {
                pindex = found;
                break;
            }
        }
    }
    block_locator_free(&locator);

    /* Count headers to send */
    int count = 0;
    struct block_index *iter = pindex ?
        active_chain_at(chain, pindex->nHeight + 1) :
        active_chain_at(chain, 0);

    while (iter && count < 2000) {
        count++;
        if (!uint256_is_null(&hash_stop) && iter->phashBlock &&
            uint256_eq(iter->phashBlock, &hash_stop))
            break;
        iter = active_chain_at(chain, iter->nHeight + 1);
    }

    struct byte_stream headers;
    stream_init(&headers, 4096);
    stream_write_compact_size(&headers, (uint64_t)count);

    iter = pindex ? active_chain_at(chain, pindex->nHeight + 1) :
                    active_chain_at(chain, 0);
    for (int i = 0; i < count && iter; i++) {
        struct block_header hdr;
        block_header_init(&hdr);
        hdr.nVersion = iter->nVersion;
        hdr.hashPrevBlock = iter->pprev ? *iter->pprev->phashBlock :
                            (struct uint256){0};
        hdr.hashMerkleRoot = iter->hashMerkleRoot;
        hdr.hashFinalSaplingRoot = iter->hashFinalSaplingRoot;
        hdr.nTime = iter->nTime;
        hdr.nBits = iter->nBits;
        hdr.nNonce = iter->nNonce;
        memcpy(hdr.nSolution, iter->nSolution, iter->nSolutionSize);
        hdr.nSolutionSize = iter->nSolutionSize;

        block_header_serialize(&hdr, &headers);
        stream_write_compact_size(&headers, 0);
        iter = active_chain_at(chain, iter->nHeight + 1);
    }

    p2p_node_begin_message(node, "headers", mp->params->pchMessageStart);
    p2p_node_write_message_data(node, headers.data, headers.size);
    p2p_node_end_message(node);
    stream_free(&headers);
    return true;
}

static bool process_inv(struct msg_processor *mp, struct p2p_node *node,
                         struct byte_stream *s)
{
    uint64_t count;
    if (!stream_read_compact_size(s, &count))
        return false;

    if (count > MAX_INV_SZ) {
        printf("Peer %s: inv message too large (%llu)\n",
               node->addr_name, (unsigned long long)count);
        node->disconnect = true;
        return false;
    }

    struct byte_stream getdata;
    stream_init(&getdata, 256);
    uint64_t request_count = 0;

    for (uint64_t i = 0; i < count; i++) {
        struct inv_item inv;
        if (!inv_item_deserialize(&inv, s)) {
            stream_free(&getdata);
            return false;
        }

        p2p_node_add_inventory_known(node, &inv);

        if (inv.type == MSG_BLOCK) {
            struct block_index *bi = block_map_find(
                &mp->main_state->map_block_index, &inv.hash);
            if (!bi) {
                inv_item_serialize(&inv, &getdata);
                request_count++;
            }
            struct block_index *tip = active_chain_tip(
                &mp->main_state->chain_active);
            if (tip && bi && active_chain_contains(
                    &mp->main_state->chain_active, bi)) {
                node->hash_continue = inv.hash;
            }
        } else if (inv.type == MSG_TX) {
            if (!tx_mempool_exists(mp->mempool, &inv.hash)) {
                inv_item_serialize(&inv, &getdata);
                request_count++;
            }
        }
    }

    if (request_count > 0) {
        struct byte_stream msg;
        stream_init(&msg, getdata.size + 8);
        stream_write_compact_size(&msg, request_count);
        stream_write(&msg, getdata.data, getdata.size);

        p2p_node_begin_message(node, "getdata", mp->params->pchMessageStart);
        p2p_node_write_message_data(node, msg.data, msg.size);
        p2p_node_end_message(node);
        stream_free(&msg);
    }
    stream_free(&getdata);
    return true;
}

static bool process_getdata(struct msg_processor *mp, struct p2p_node *node,
                             struct byte_stream *s)
{
    uint64_t count;
    if (!stream_read_compact_size(s, &count))
        return false;

    if (count > MAX_INV_SZ) {
        node->disconnect = true;
        return false;
    }

    struct inv_item not_found[64];
    size_t not_found_count = 0;

    for (uint64_t i = 0; i < count; i++) {
        struct inv_item inv;
        if (!inv_item_deserialize(&inv, s))
            return false;

        bool sent = false;
        if (inv.type == MSG_BLOCK) {
            struct block_index *bi = block_map_find(
                &mp->main_state->map_block_index, &inv.hash);
            if (bi && (bi->nStatus & BLOCK_HAVE_DATA)) {
                struct block blk;
                block_init(&blk);

                if (read_block_from_disk_index(&blk, bi, mp->datadir)) {
                    struct byte_stream blk_data;
                    stream_init(&blk_data, 1024 * 1024);
                    if (block_serialize(&blk, &blk_data)) {
                        p2p_node_begin_message(node, "block",
                                               mp->params->pchMessageStart);
                        p2p_node_write_message_data(node, blk_data.data,
                                                    blk_data.size);
                        p2p_node_end_message(node);
                        sent = true;
                    }
                    stream_free(&blk_data);
                }
                block_free(&blk);
            }
        } else if (inv.type == MSG_TX) {
            struct transaction tx;
            transaction_init(&tx);
            if (tx_mempool_lookup(mp->mempool, &inv.hash, &tx)) {
                struct byte_stream tx_data;
                stream_init(&tx_data, 512);
                transaction_serialize(&tx, &tx_data);

                p2p_node_begin_message(node, "tx",
                                       mp->params->pchMessageStart);
                p2p_node_write_message_data(node, tx_data.data, tx_data.size);
                p2p_node_end_message(node);
                stream_free(&tx_data);
                sent = true;
            }
            transaction_free(&tx);
        }

        if (!sent && not_found_count < 64)
            not_found[not_found_count++] = inv;
    }

    /* Send notfound for items we couldn't serve */
    if (not_found_count > 0) {
        struct byte_stream nf;
        stream_init(&nf, not_found_count * 36 + 8);
        stream_write_compact_size(&nf, not_found_count);
        for (size_t i = 0; i < not_found_count; i++)
            inv_item_serialize(&not_found[i], &nf);

        p2p_node_begin_message(node, "notfound",
                               mp->params->pchMessageStart);
        p2p_node_write_message_data(node, nf.data, nf.size);
        p2p_node_end_message(node);
        stream_free(&nf);
    }

    return true;
}

static bool process_block_msg(struct msg_processor *mp, struct p2p_node *node,
                               struct byte_stream *s)
{
    struct block blk;
    block_init(&blk);
    if (!block_deserialize(&blk, s)) {
        printf("Peer %s: failed to deserialize block\n", node->addr_name);
        block_free(&blk);
        return false;
    }

    struct uint256 hash;
    block_get_hash(&blk, &hash);
    char hex[65];
    uint256_get_hex(&hash, hex);
    printf("Peer %s: received block %s\n", node->addr_name, hex);

    struct validation_state state;
    validation_state_init(&state);
    process_new_block(&state, mp->main_state, mp->coins_tip,
                      mp->params, &blk, false, mp->datadir);

    if (!validation_state_is_valid(&state)) {
        int dos = 0;
        if (validation_state_get_dos(&state, &dos) && dos > 0) {
            printf("Peer %s: invalid block (dos=%d): %s\n",
                   node->addr_name, dos, state.reject_reason);
        }
    }

    block_free(&blk);
    return true;
}

static bool accept_to_mempool(struct msg_processor *mp,
                               struct transaction *tx)
{
    struct validation_state state;
    validation_state_init(&state);

    if (!check_transaction(tx, &state))
        return false;

    struct uint256 hash;
    transaction_compute_hash((struct transaction *)tx);
    hash = tx->hash;

    if (tx_mempool_exists(mp->mempool, &hash))
        return false;

    int tip_height = active_chain_height(&mp->main_state->chain_active);
    uint32_t branch_id = consensus_current_epoch_branch_id(
        tip_height + 1, &mp->params->consensus);

    int64_t fee = 0;
    double priority = 0.0;
    bool spends_coinbase = false;

    struct mempool_entry entry;
    mempool_entry_init(&entry, tx, fee, (int64_t)time(NULL), priority,
                       (unsigned int)(tip_height + 1),
                       tx_mempool_has_no_inputs_of(mp->mempool, tx),
                       spends_coinbase, branch_id);

    bool accepted = tx_mempool_add_unchecked(mp->mempool, &hash, &entry);
    if (!accepted)
        mempool_entry_free(&entry);

    return accepted;
}

static bool process_tx_msg(struct msg_processor *mp, struct p2p_node *node,
                            struct byte_stream *s)
{
    struct transaction tx;
    transaction_init(&tx);
    if (!transaction_deserialize(&tx, s)) {
        printf("Peer %s: failed to deserialize tx\n", node->addr_name);
        transaction_free(&tx);
        return false;
    }

    struct uint256 hash;
    transaction_compute_hash(&tx);
    hash = tx.hash;

    if (accept_to_mempool(mp, &tx)) {
        struct inv_item inv;
        inv_item_init_typed(&inv, MSG_TX, &hash);
        p2p_node_add_inventory_known(node, &inv);

        char hex[65];
        uint256_get_hex(&hash, hex);
        printf("Peer %s: accepted tx %s to mempool\n", node->addr_name, hex);
    }

    transaction_free(&tx);
    return true;
}

static bool process_headers(struct msg_processor *mp, struct p2p_node *node,
                             struct byte_stream *s)
{
    uint64_t count;
    if (!stream_read_compact_size(s, &count))
        return false;

    if (count > 2000) {
        node->disconnect = true;
        return false;
    }

    struct uint256 last_hash;
    uint256_set_null(&last_hash);
    size_t accepted = 0;

    for (uint64_t i = 0; i < count; i++) {
        struct block_header hdr;
        block_header_init(&hdr);
        if (!block_header_deserialize(&hdr, s))
            return false;

        uint64_t dummy;
        if (!stream_read_compact_size(s, &dummy))
            return false;

        struct validation_state state;
        validation_state_init(&state);
        struct block_index *pindex = NULL;
        if (accept_block_header(&hdr, &state, mp->main_state,
                                mp->params, &pindex)) {
            accepted++;
            if (pindex && pindex->phashBlock)
                last_hash = *pindex->phashBlock;
        }
    }

    if (accepted > 0)
        printf("Peer %s: accepted %zu headers\n", node->addr_name, accepted);

    /* Request blocks for headers we accepted but don't have data for */
    if (accepted > 0) {
        struct block_index *tip = active_chain_tip(&mp->main_state->chain_active);
        int our_height = tip ? tip->nHeight : 0;

        struct block_index *bi = block_map_find(
            &mp->main_state->map_block_index, &last_hash);
        if (bi && bi->nHeight > our_height) {
            /* Request blocks via getdata */
            struct byte_stream getdata_msg;
            stream_init(&getdata_msg, 4096);
            uint64_t block_count = 0;

            struct block_index *walk = bi;
            struct uint256 request_hashes[128];
            size_t num_requests = 0;

            while (walk && walk->nHeight > our_height &&
                   num_requests < 128) {
                if (!(walk->nStatus & BLOCK_HAVE_DATA))
                    request_hashes[num_requests++] = *walk->phashBlock;
                walk = walk->pprev;
            }

            /* Send in forward order */
            for (size_t i = num_requests; i > 0; i--) {
                struct inv_item inv;
                inv_item_init_typed(&inv, MSG_BLOCK, &request_hashes[i - 1]);
                inv_item_serialize(&inv, &getdata_msg);
                block_count++;
            }

            if (block_count > 0) {
                struct byte_stream msg;
                stream_init(&msg, getdata_msg.size + 8);
                stream_write_compact_size(&msg, block_count);
                stream_write(&msg, getdata_msg.data, getdata_msg.size);

                p2p_node_begin_message(node, "getdata",
                                       mp->params->pchMessageStart);
                p2p_node_write_message_data(node, msg.data, msg.size);
                p2p_node_end_message(node);
                stream_free(&msg);
            }
            stream_free(&getdata_msg);
        }
    }

    /* If we got 2000 headers (max batch), ask for more */
    if (count == 2000)
        push_getheaders(mp, node);

    return true;
}

static bool process_getaddr(struct msg_processor *mp, struct p2p_node *node)
{
    if (node->sent_addr)
        return true;
    node->sent_addr = true;

    if (!mp->net_mgr)
        return true;

    struct net_address addrs[MAX_ADDR_TO_SEND];
    size_t num = addrman_get_addr(&mp->net_mgr->addrman, addrs,
                                   MAX_ADDR_TO_SEND);

    if (num > 0) {
        struct byte_stream addr_msg;
        stream_init(&addr_msg, num * 30 + 8);
        stream_write_compact_size(&addr_msg, (uint64_t)num);
        for (size_t i = 0; i < num; i++)
            net_address_serialize(&addrs[i], &addr_msg, true);

        p2p_node_begin_message(node, "addr", mp->params->pchMessageStart);
        p2p_node_write_message_data(node, addr_msg.data, addr_msg.size);
        p2p_node_end_message(node);
        stream_free(&addr_msg);
    }
    return true;
}

static bool process_mempool(struct msg_processor *mp, struct p2p_node *node)
{
    struct uint256 hashes[MAX_INV_SZ];
    size_t num = 0;
    tx_mempool_query_hashes(mp->mempool, hashes, MAX_INV_SZ, &num);

    for (size_t i = 0; i < num; i++) {
        struct inv_item inv;
        inv_item_init_typed(&inv, MSG_TX, &hashes[i]);
        p2p_node_push_inventory(node, &inv);
    }
    return true;
}

static bool process_sendheaders(struct p2p_node *node)
{
    node->prefer_headers = true;
    return true;
}

static bool process_reject(struct p2p_node *node, struct byte_stream *s)
{
    (void)node;
    uint64_t msg_len;
    if (!stream_read_compact_size(s, &msg_len))
        return true;
    char msg_type[32] = {0};
    if (msg_len > 0 && msg_len < sizeof(msg_type))
        stream_read_bytes(s, (unsigned char *)msg_type, msg_len);
    uint8_t code = 0;
    stream_read_u8(s, &code);
    (void)code;
    (void)msg_type;
    return true;
}

static bool process_feefilter(struct p2p_node *node, struct byte_stream *s)
{
    uint64_t fee_rate = 0;
    stream_read_u64_le(s, &fee_rate);
    (void)fee_rate;
    (void)node;
    return true;
}

static bool process_notfound(struct p2p_node *node, struct byte_stream *s)
{
    (void)node;
    uint64_t count;
    if (!stream_read_compact_size(s, &count))
        return false;
    for (uint64_t i = 0; i < count; i++) {
        struct inv_item inv;
        if (!inv_item_deserialize(&inv, s))
            return false;
    }
    return true;
}

bool msg_process_messages(void *ctx, struct p2p_node *node)
{
    struct msg_processor *mp = (struct msg_processor *)ctx;

    while (node->recv_msg_count > 0) {
        zcl_mutex_lock(&node->cs_recv);
        if (node->recv_msg_count == 0) {
            zcl_mutex_unlock(&node->cs_recv);
            break;
        }

        struct net_message msg = node->recv_msgs[0];
        memmove(&node->recv_msgs[0], &node->recv_msgs[1],
                (node->recv_msg_count - 1) * sizeof(struct net_message));
        node->recv_msg_count--;
        zcl_mutex_unlock(&node->cs_recv);

        if (!net_message_complete(&msg)) {
            net_message_free(&msg);
            continue;
        }

        char cmd[COMMAND_SIZE + 1];
        msg_header_get_command(&msg.hdr, cmd, sizeof(cmd));

        struct byte_stream s;
        stream_init_from_data(&s, msg.recv_data, msg.data_pos);

        bool ok = true;

        if (strcmp(cmd, "version") == 0) {
            ok = process_version(mp, node, &s);
        } else if (strcmp(cmd, "verack") == 0) {
            ok = process_verack(mp, node);
        } else if (node->version == 0) {
            printf("Peer %s: received %s before version\n",
                   node->addr_name, cmd);
            ok = false;
        } else if (strcmp(cmd, "ping") == 0) {
            ok = process_ping(mp, node, &s);
        } else if (strcmp(cmd, "pong") == 0) {
            ok = process_pong(node, &s);
        } else if (strcmp(cmd, "addr") == 0) {
            ok = process_addr(mp, node, &s);
        } else if (strcmp(cmd, "inv") == 0) {
            ok = process_inv(mp, node, &s);
        } else if (strcmp(cmd, "getdata") == 0) {
            ok = process_getdata(mp, node, &s);
        } else if (strcmp(cmd, "getblocks") == 0) {
            ok = process_getblocks(mp, node, &s);
        } else if (strcmp(cmd, "getheaders") == 0) {
            ok = process_getheaders(mp, node, &s);
        } else if (strcmp(cmd, "block") == 0) {
            ok = process_block_msg(mp, node, &s);
        } else if (strcmp(cmd, "tx") == 0) {
            ok = process_tx_msg(mp, node, &s);
        } else if (strcmp(cmd, "headers") == 0) {
            ok = process_headers(mp, node, &s);
        } else if (strcmp(cmd, "getaddr") == 0) {
            ok = process_getaddr(mp, node);
        } else if (strcmp(cmd, "mempool") == 0) {
            ok = process_mempool(mp, node);
        } else if (strcmp(cmd, "sendheaders") == 0) {
            ok = process_sendheaders(node);
        } else if (strcmp(cmd, "reject") == 0) {
            ok = process_reject(node, &s);
        } else if (strcmp(cmd, "feefilter") == 0) {
            ok = process_feefilter(node, &s);
        } else if (strcmp(cmd, "notfound") == 0) {
            ok = process_notfound(node, &s);
        }

        stream_free(&s);
        net_message_free(&msg);

        if (!ok) {
            printf("Peer %s: error processing %s\n", node->addr_name, cmd);
        }
    }
    return true;
}

static void build_block_locator(struct block_locator *loc,
                                 const struct active_chain *chain)
{
    int height = active_chain_height(chain);
    int step = 1;
    size_t count = 0;

    /* Count entries needed */
    int h = height;
    while (h >= 0) {
        count++;
        if (h == 0) break;
        h -= step;
        if (h < 0) h = 0;
        if (count >= 10) step *= 2;
    }

    loc->vhave = calloc(count, sizeof(struct uint256));
    if (!loc->vhave) { loc->num_hashes = 0; return; }
    loc->num_hashes = count;

    h = height;
    step = 1;
    size_t idx = 0;
    while (h >= 0 && idx < count) {
        struct block_index *bi = active_chain_at(chain, h);
        if (bi && bi->phashBlock)
            loc->vhave[idx] = *bi->phashBlock;
        idx++;
        if (h == 0) break;
        h -= step;
        if (h < 0) h = 0;
        if (idx >= 10) step *= 2;
    }
}

static void push_getheaders(struct msg_processor *mp, struct p2p_node *node)
{
    struct block_locator loc;
    block_locator_init(&loc);
    build_block_locator(&loc, &mp->main_state->chain_active);

    struct byte_stream s;
    stream_init(&s, 512);
    block_locator_serialize(&loc, &s);
    struct uint256 zero;
    uint256_set_null(&zero);
    stream_write_bytes(&s, zero.data, 32);

    p2p_node_begin_message(node, "getheaders", mp->params->pchMessageStart);
    p2p_node_write_message_data(node, s.data, s.size);
    p2p_node_end_message(node);
    stream_free(&s);
    block_locator_free(&loc);
}

bool msg_send_messages(void *ctx, struct p2p_node *node, bool send_trickle)
{
    struct msg_processor *mp = (struct msg_processor *)ctx;

    /* Outbound nodes: send version to initiate handshake */
    if (!node->successfully_connected) {
        if (!node->inbound && node->send_bytes == 0)
            push_version(mp, node);
        return true;
    }

    /* Initiate sync with this peer if not done yet */
    if (!node->sync_started && !node->inbound) {
        node->sync_started = true;
        push_getheaders(mp, node);
    }

    /* Send ping */
    int64_t now = (int64_t)time(NULL);
    if (node->ping_nonce_sent == 0 &&
        now - node->last_send > PING_INTERVAL) {
        uint64_t nonce = GetRand(UINT64_MAX);
        node->ping_nonce_sent = nonce;
        node->ping_usec_start = now * 1000000;

        struct byte_stream ping;
        stream_init(&ping, 8);
        stream_write_u64_le(&ping, nonce);

        p2p_node_begin_message(node, "ping", mp->params->pchMessageStart);
        p2p_node_write_message_data(node, ping.data, ping.size);
        p2p_node_end_message(node);
        stream_free(&ping);
    }

    /* Send inventory on trickle */
    if (send_trickle) {
        zcl_mutex_lock(&node->cs_inventory);
        if (node->inventory_to_send_count > 0) {
            struct byte_stream inv_msg;
            stream_init(&inv_msg, 256);
            uint64_t count = 0;

            for (size_t i = 0; i < node->inventory_to_send_count; i++) {
                inv_item_serialize(&node->inventory_to_send[i], &inv_msg);
                count++;
            }

            if (count > 0) {
                struct byte_stream msg;
                stream_init(&msg, inv_msg.size + 8);
                stream_write_compact_size(&msg, count);
                stream_write(&msg, inv_msg.data, inv_msg.size);

                p2p_node_begin_message(node, "inv",
                                       mp->params->pchMessageStart);
                p2p_node_write_message_data(node, msg.data, msg.size);
                p2p_node_end_message(node);
                stream_free(&msg);
            }
            stream_free(&inv_msg);

            node->inventory_to_send_count = 0;
        }
        zcl_mutex_unlock(&node->cs_inventory);
    }

    /* Send addresses */
    if (node->addr_to_send_count > 0) {
        struct byte_stream addr_msg;
        stream_init(&addr_msg, 512);
        uint64_t count = node->addr_to_send_count;
        if (count > MAX_ADDR_TO_SEND)
            count = MAX_ADDR_TO_SEND;
        stream_write_compact_size(&addr_msg, count);

        for (size_t i = 0; i < count; i++)
            net_address_serialize(&node->addr_to_send[i], &addr_msg, true);

        p2p_node_begin_message(node, "addr", mp->params->pchMessageStart);
        p2p_node_write_message_data(node, addr_msg.data, addr_msg.size);
        p2p_node_end_message(node);
        stream_free(&addr_msg);

        node->addr_to_send_count = 0;
    }

    return true;
}
