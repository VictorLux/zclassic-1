/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "net/msgprocessor.h"
#include "net/addrman.h"
#include "models/peer.h"
#include "models/database.h"
#include "net/fast_sync.h"
#include "net/p2p_game.h"
#include "net/version.h"
#include "net/p2p_message.h"
#include "core/hash.h"
#include "core/random.h"
#include "core/serialize.h"
#include "consensus/upgrades.h"
#include "consensus/validation.h"
#include "validation/check_transaction.h"
#include "validation/process_block.h"
#include "controllers/sync_controller.h"
#include "storage/disk_block_io.h"
#include "wallet/wallet.h"
#include "util/timedata.h"
#include "event/event.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

static void push_getheaders(struct msg_processor *mp, struct p2p_node *node);
static void push_getheaders_from(struct msg_processor *mp,
                                  struct p2p_node *node,
                                  struct block_index *from);

/* Cached snapshot offer — pre-computed at startup, not in message handler.
 * g_cached_offer_valid uses _Atomic to avoid data race between the
 * background build thread (boot.c) and the P2P message handler. */
struct snapshot_offer g_cached_offer;
_Atomic bool g_cached_offer_valid = false;
static struct fast_sync_rate_limiter g_rate_limiter = {0};

/* Cached manifest for parallel chunk sync (built in background at startup). */
struct sync_manifest g_cached_manifest;
_Atomic bool g_cached_manifest_valid = false;

/* Global swarm coordinator — manages parallel UTXO chunk download.
 * Only active when we are syncing from multiple ZCL23 peers. */
static struct swarm_sync g_swarm __attribute__((used));
static _Atomic bool g_swarm_active = false;
static int64_t g_swarm_last_progress_time = 0;

/* Timeout for inflight chunk requests (30 seconds). */
#define SWARM_CHUNK_TIMEOUT_SECS 30

/* Progress display interval (5 seconds). */
#define SWARM_PROGRESS_INTERVAL_SECS 5

/* Ring buffers for duplicate detection of recently seen blocks and txs. */
#define MAX_RECENT_BLOCKS 128
#define MAX_RECENT_TXS 4096
static struct uint256 g_recent_blocks[MAX_RECENT_BLOCKS];
static int g_recent_block_count = 0;
static struct uint256 g_recent_txs[MAX_RECENT_TXS];
static int g_recent_tx_count = 0;

static bool block_already_seen(const struct uint256 *hash) {
    int limit = g_recent_block_count < MAX_RECENT_BLOCKS
                ? g_recent_block_count : MAX_RECENT_BLOCKS;
    for (int i = 0; i < limit; i++) {
        if (uint256_eq(hash, &g_recent_blocks[i])) return true;
    }
    return false;
}

static void block_mark_seen(const struct uint256 *hash) {
    g_recent_blocks[g_recent_block_count % MAX_RECENT_BLOCKS] = *hash;
    g_recent_block_count++;
}

static bool tx_already_seen(const struct uint256 *hash) {
    int limit = g_recent_tx_count < MAX_RECENT_TXS
                ? g_recent_tx_count : MAX_RECENT_TXS;
    for (int i = 0; i < limit; i++) {
        if (uint256_eq(hash, &g_recent_txs[i])) return true;
    }
    return false;
}

static void tx_mark_seen(const struct uint256 *hash) {
    g_recent_txs[g_recent_tx_count % MAX_RECENT_TXS] = *hash;
    g_recent_tx_count++;
}

/* Expose internals for unit testing via weak-linked test helpers. */
bool msgprocessor_test_block_already_seen(const struct uint256 *hash) {
    return block_already_seen(hash);
}
void msgprocessor_test_block_mark_seen(const struct uint256 *hash) {
    block_mark_seen(hash);
}
void msgprocessor_test_reset_recent_blocks(void) {
    g_recent_block_count = 0;
    memset(g_recent_blocks, 0, sizeof(g_recent_blocks));
}
int msgprocessor_test_get_recent_block_count(void) {
    return g_recent_block_count;
}
bool msgprocessor_test_tx_already_seen(const struct uint256 *hash) {
    return tx_already_seen(hash);
}
void msgprocessor_test_tx_mark_seen(const struct uint256 *hash) {
    tx_mark_seen(hash);
}

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

/* Send our manifest to a ZCL23 peer. Called after version/verack handshake. */
static void push_manifest(struct msg_processor *mp, struct p2p_node *node)
{
    if (!g_cached_manifest_valid || node->swarm_manifest_sent)
        return;

    struct sync_manifest *m = &g_cached_manifest;
    struct byte_stream s;
    stream_init(&s, 80);
    stream_write_i32_le(&s, m->height);
    stream_write_bytes(&s, m->block_hash, 32);
    stream_write_u64_le(&s, m->num_utxos);
    stream_write_u32_le(&s, m->num_chunks);
    stream_write_u32_le(&s, m->chunk_size);
    stream_write_bytes(&s, m->merkle_root, 32);

    p2p_node_begin_message(node, MSG_MANIFEST, mp->params->pchMessageStart);
    p2p_node_write_message_data(node, s.data, s.size);
    p2p_node_end_message(node);
    stream_free(&s);

    node->swarm_manifest_sent = true;
    printf("Peer %s: sent manifest (h=%d, %u chunks)\n",
           node->addr_name, m->height, m->num_chunks);
    fflush(stdout);
}

/* Send a chunk request to a peer. */
static void push_chunk_request(struct msg_processor *mp,
                                struct p2p_node *node,
                                uint32_t chunk_index)
{
    struct byte_stream s;
    stream_init(&s, 4);
    stream_write_u32_le(&s, chunk_index);

    p2p_node_begin_message(node, MSG_CHUNK_REQ, mp->params->pchMessageStart);
    p2p_node_write_message_data(node, s.data, s.size);
    p2p_node_end_message(node);
    stream_free(&s);
}

static void push_version(struct msg_processor *mp, struct p2p_node *node)
{
    struct version_message ver;
    version_message_init(&ver);
    ver.protocol_version = PROTOCOL_VERSION;
    /* Match zclassicd services exactly: NODE_NETWORK | NODE_BLOOM */
    ver.services = NODE_NETWORK | NODE_BLOOM;
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

    printf("Sent version to %s: proto=%d h=%d subver=%s size=%zu\n",
           node->addr_name, ver.protocol_version, ver.start_height,
           ver.sub_version, s.size);
    fflush(stdout);
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

    event_emitf(EV_PEER_VERSION, (uint32_t)node->id,
                "proto=%d h=%d %s", ver.protocol_version,
                ver.start_height, ver.sub_version);
    peer_set_state_checked((uint32_t)node->id, &node->state,
                           PEER_VERSION_RECEIVED, "version msg received");

    if (!node->inbound) {
        AddTimeData((const unsigned char *)node->addr_name,
                    (int)strlen(node->addr_name), node->time_offset);
    }

    push_verack(mp, node);

    if (node->inbound)
        push_version(mp, node);

    /* For outbound connections, we already sent version; now we received
     * their version and sent verack. Mark connected once we also get their
     * verack (handled in process_verack). For inbound, the peer initiated,
     * so mark connected after we send our version+verack. */
    if (node->inbound) {
        peer_set_state_checked((uint32_t)node->id, &node->state,
                               PEER_HANDSHAKE_COMPLETE, "inbound version+verack");
    }

    /* Ask outbound peers for their address list */
    if (!node->inbound && !node->get_addr) {
        p2p_node_begin_message(node, "getaddr", mp->params->pchMessageStart);
        p2p_node_end_message(node);
        node->get_addr = true;
    }

    /* Send sendheaders — tells peer we prefer headers announcements
     * over inv. Critical for headers-first sync with legacy zclassicd. */
    p2p_node_begin_message(node, "sendheaders", mp->params->pchMessageStart);
    p2p_node_end_message(node);

    printf("Peer %s: version=%d subver=%s height=%d%s\n",
           node->addr_name, node->version, node->sub_ver,
           node->starting_height,
           peer_supports_fast_sync(node->services) ? " [ZCL23]" : "");

    /* Detect zclassic23 peers via subversion string.
     * Service bit detection is secondary — some peers filter unknown bits. */
    bool is_zcl23 = peer_supports_fast_sync(node->services) ||
                    strstr(node->sub_ver, "ZClassic-C23") != NULL;
    if (is_zcl23) {
        node->services |= NODE_ZCL23; /* mark for fast sync */
        node->swarm_inflight_chunk = -1;
        printf("Peer %s: supports zclassic23 fast sync [ZCL23]\n",
               node->addr_name);

        /* Exchange manifests — both peers announce what they have. */
        push_manifest(mp, node);
    }

    return true;
}

static bool process_verack(struct msg_processor *mp, struct p2p_node *node)
{
    node->recv_version = PROTOCOL_VERSION;

    /* Outbound: handshake complete (we sent version, got version+verack).
     * Inbound: already marked in process_version. */
    if (!node->inbound && node->state < PEER_HANDSHAKE_COMPLETE) {
        peer_set_state_checked((uint32_t)node->id, &node->state,
                               PEER_HANDSHAKE_COMPLETE, "verack received");
        printf("Peer %s: handshake complete (outbound)\n", node->addr_name);
    } else if (node->state < PEER_HANDSHAKE_COMPLETE) {
        peer_set_state_checked((uint32_t)node->id, &node->state,
                               PEER_HANDSHAKE_COMPLETE, "verack received");
        printf("Peer %s: verack received\n", node->addr_name);
    }

    /* Mark peer as good in addrman — increases selection priority */
    if (mp->net_mgr) {
        addrman_good(&mp->net_mgr->addrman, &node->addr.svc,
                      (int64_t)time(NULL));
        addrman_connected(&mp->net_mgr->addrman, &node->addr.svc,
                           (int64_t)time(NULL));
    }

    /* Save peer to SQLite for explorer/browser visibility */
    extern struct node_db *g_active_node_db;
    if (g_active_node_db && g_active_node_db->db) {
        sqlite3_exec(g_active_node_db->db,
            "CREATE TABLE IF NOT EXISTS peers ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "ip BLOB NOT NULL,port INTEGER NOT NULL,"
            "services INTEGER NOT NULL DEFAULT 0,"
            "last_seen INTEGER NOT NULL,"
            "last_try INTEGER DEFAULT 0,attempts INTEGER DEFAULT 0,"
            "source BLOB,UNIQUE(ip,port))",
            NULL, NULL, NULL);
        sqlite3_stmt *ins = NULL;
        sqlite3_prepare_v2(g_active_node_db->db,
            "INSERT OR REPLACE INTO peers (ip,port,services,last_seen)"
            " VALUES(?,?,?,?)", -1, &ins, NULL);
        if (ins) {
            sqlite3_bind_blob(ins, 1, node->addr.svc.addr.ip, 16, SQLITE_STATIC);
            sqlite3_bind_int(ins, 2, node->addr.svc.port);
            sqlite3_bind_int64(ins, 3, (int64_t)node->services);
            sqlite3_bind_int64(ins, 4, (int64_t)time(NULL));
            sqlite3_step(ins);
            sqlite3_finalize(ins);
        }
    }
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
            /* Exponential moving average: new = 0.8 * old + 0.2 * sample */
            if (node->avg_latency_us == 0)
                node->avg_latency_us = rtt;
            else
                node->avg_latency_us =
                    (node->avg_latency_us * 4 + rtt) / 5;
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
        if (iter->pprev && iter->pprev->phashBlock)
            hdr.hashPrevBlock = *iter->pprev->phashBlock;
        else
            memset(&hdr.hashPrevBlock, 0, sizeof(hdr.hashPrevBlock));
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

    printf("Responding to getheaders from %s with %d headers (from %d)\n",
           node->addr_name, count,
           pindex ? pindex->nHeight + 1 : 0);

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

    uint64_t skipped_blocks = 0;
    uint64_t skipped_txs = 0;

    for (uint64_t i = 0; i < count; i++) {
        struct inv_item inv;
        if (!inv_item_deserialize(&inv, s)) {
            stream_free(&getdata);
            return false;
        }

        p2p_node_add_inventory_known(node, &inv);

        if (inv.type == MSG_BLOCK) {
            if (block_already_seen(&inv.hash)) {
                skipped_blocks++;
                continue;
            }
            struct block_index *bi = block_map_find(
                &mp->main_state->map_block_index, &inv.hash);
            struct block_index *tip = active_chain_tip(
                &mp->main_state->chain_active);
            /* Only request blocks via inv if we're close to the tip;
             * during IBD, rely on headers-first sync. */
            bool in_ibd = !tip || (node->starting_height > 0 &&
                          tip->nHeight < node->starting_height - 1000);
            if (!bi && !in_ibd) {
                inv_item_serialize(&inv, &getdata);
                request_count++;
            } else if (!bi && in_ibd) {
                /* Ask for headers instead */
                push_getheaders_from(mp, node, tip);
            }
            if (tip && bi && active_chain_contains(
                    &mp->main_state->chain_active, bi)) {
                node->hash_continue = inv.hash;
            }
        } else if (inv.type == MSG_TX) {
            if (tx_already_seen(&inv.hash)) {
                skipped_txs++;
                continue;
            }
            if (!tx_mempool_exists(mp->mempool, &inv.hash)) {
                inv_item_serialize(&inv, &getdata);
                request_count++;
            }
        }
    }

    if (skipped_blocks > 0 || skipped_txs > 0) {
        printf("Peer %s: inv dedup skipped %llu blocks, %llu txs\n",
               node->addr_name,
               (unsigned long long)skipped_blocks,
               (unsigned long long)skipped_txs);
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
        peer_misbehaving(mp->net_mgr, node, 20, "malformed block");
        block_free(&blk);
        return false;
    }

    struct uint256 hash;
    block_get_hash(&blk, &hash);

    if (block_already_seen(&hash)) {
        block_free(&blk);
        return true;
    }
    block_mark_seen(&hash);

    char hex[65];
    uint256_get_hex(&hash, hex);

    struct block_index *tip = active_chain_tip(&mp->main_state->chain_active);
    int tip_height = tip ? tip->nHeight : 0;
    printf("Peer %s: block %s (tip=%d)\n", node->addr_name, hex, tip_height);

    struct validation_state state;
    validation_state_init(&state);
    process_new_block(&state, mp->main_state, mp->coins_tip,
                      mp->params, &blk, false, mp->datadir);

    if (validation_state_is_valid(&state)) {
        node->last_block_time = (int64_t)time(NULL);
        node->blocks_received++;

        struct block_index *new_tip = active_chain_tip(
            &mp->main_state->chain_active);
        if (new_tip) {
            event_emitf(EV_BLOCK_CONNECTED, (uint32_t)node->id,
                        "h=%d", new_tip->nHeight);

            /* Check if we've caught up to the peer */
            if (new_tip->nHeight >= node->starting_height &&
                node->starting_height > 0) {
                enum sync_state ss = sync_get_state();
                if (ss == SYNC_BLOCKS_DOWNLOAD ||
                    ss == SYNC_CONNECTING_BLOCKS)
                    sync_set_state(SYNC_AT_TIP, "caught up to peer");
                if (node->state == PEER_SYNCING_BLOCKS ||
                    node->state == PEER_SYNCING_HEADERS)
                    peer_set_state_checked((uint32_t)node->id,
                                           &node->state, PEER_ACTIVE,
                                           "chain caught up");
            }

            if (new_tip->nHeight % 1000 == 0) {
                printf("Chain tip: height=%d from peer %s\n",
                       new_tip->nHeight, node->addr_name);
                fflush(stdout);
            }
        }
    } else {
        int dos = 0;
        if (validation_state_get_dos(&state, &dos) && dos > 0) {
            event_emitf(EV_BLOCK_REJECTED, (uint32_t)node->id,
                        "dos=%d %s", dos, state.reject_reason);
            printf("Peer %s: invalid block (dos=%d): %s\n",
                   node->addr_name, dos, state.reject_reason);
            peer_misbehaving(mp->net_mgr, node, dos,
                             state.reject_reason[0] ? state.reject_reason
                                                    : "invalid block");
        } else if (!validation_state_is_valid(&state)) {
            peer_misbehaving(mp->net_mgr, node, 10, "block validation failed");
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
        peer_misbehaving(mp->net_mgr, node, 10, "malformed tx");
        transaction_free(&tx);
        return false;
    }

    struct uint256 hash;
    transaction_compute_hash(&tx);
    hash = tx.hash;

    if (tx_already_seen(&hash)) {
        transaction_free(&tx);
        return true;
    }
    tx_mark_seen(&hash);

    if (accept_to_mempool(mp, &tx)) {
        event_emit(EV_TX_ACCEPTED, (uint32_t)node->id,
                   hash.data, 32);
        struct inv_item inv;
        inv_item_init_typed(&inv, MSG_TX, &hash);
        p2p_node_add_inventory_known(node, &inv);

        extern struct wallet *g_active_wallet;
        if (g_active_wallet) {
            wallet_sync_transaction(g_active_wallet, &tx, NULL);
            extern struct node_db *g_active_node_db;
            if (g_active_node_db)
                node_db_sync_wallet_tx(g_active_node_db, &tx,
                                       g_active_wallet, 0);
        }
    } else {
        event_emit(EV_TX_REJECTED, (uint32_t)node->id,
                   hash.data, 32);
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
        printf("Peer %s: headers count %llu exceeds maximum 2000\n",
               node->addr_name, (unsigned long long)count);
        peer_misbehaving(mp->net_mgr, node, 20, "too many headers");
        node->disconnect = true;
        return false;
    }

    struct uint256 last_hash;
    uint256_set_null(&last_hash);
    struct block_index *pindex_last = NULL;
    size_t accepted = 0;

    for (uint64_t i = 0; i < count; i++) {
        struct block_header hdr;
        block_header_init(&hdr);
        if (!block_header_deserialize(&hdr, s)) {
            printf("Peer %s: malformed header at index %llu\n",
                   node->addr_name, (unsigned long long)i);
            peer_misbehaving(mp->net_mgr, node, 20, "malformed header");
            return false;
        }

        uint64_t dummy;
        if (!stream_read_compact_size(s, &dummy)) {
            peer_misbehaving(mp->net_mgr, node, 20, "truncated header tx count");
            return false;
        }

        struct validation_state state;
        validation_state_init(&state);
        struct block_index *pindex = NULL;
        if (accept_block_header(&hdr, &state, mp->main_state,
                                mp->params, &pindex)) {
            accepted++;
            pindex_last = pindex;
            if (pindex && pindex->phashBlock)
                last_hash = *pindex->phashBlock;
        } else if (i < 3) {
            char hex[65];
            struct uint256 hh;
            block_header_get_hash(&hdr, &hh);
            uint256_get_hex(&hh, hex);
            printf("  header[%llu] REJECTED hash=%s reason=%s\n",
                   (unsigned long long)i, hex,
                   state.reject_reason[0] ? state.reject_reason : "unknown");
        }
    }

    if (accepted > 0) {
        event_emitf(EV_HEADERS_RECEIVED, (uint32_t)node->id,
                    "accepted=%zu total=%llu tip=%d",
                    accepted, (unsigned long long)count,
                    pindex_last ? pindex_last->nHeight : -1);

        /* Log progress: during IBD, only log every 10th batch */
        bool log_this = true;
        if (pindex_last && node->starting_height > 0 &&
            pindex_last->nHeight < node->starting_height - 2000) {
            static int headers_log_count = 0;
            log_this = (headers_log_count++ % 10 == 0);
        }
        if (log_this)
            printf("Peer %s: accepted %zu/%llu headers "
                   "(header tip=%d, chain tip=%d, peer=%d)\n",
                   node->addr_name, accepted, (unsigned long long)count,
                   pindex_last ? pindex_last->nHeight : -1,
                   active_chain_height(&mp->main_state->chain_active),
                   node->starting_height);
    }

    /* Request blocks for headers we accepted but don't have data for.
     * Walk backward to collect all needed blocks, then request in
     * forward order (oldest first) so chain_has_all_data succeeds. */
    if (accepted > 0) {
        struct block_index *tip = active_chain_tip(&mp->main_state->chain_active);
        int our_height = tip ? tip->nHeight : 0;

        struct block_index *bi = block_map_find(
            &mp->main_state->map_block_index, &last_hash);
        if (bi && bi->nHeight > our_height) {
            /* We have headers ahead of our chain — need block data */
            if (sync_get_state() == SYNC_HEADERS_DOWNLOAD)
                sync_set_state(SYNC_BLOCKS_DOWNLOAD,
                               "headers ahead, requesting blocks");
            /* Count how many blocks we need */
            size_t total_needed = 0;
            struct block_index *walk = bi;
            while (walk && walk->nHeight > our_height) {
                if (!(walk->nStatus & BLOCK_HAVE_DATA) && walk->phashBlock)
                    total_needed++;
                walk = walk->pprev;
            }

            /* Allocate and collect hashes (backward, then reverse) */
            size_t max_batch = total_needed < 512 ? total_needed : 512;
            struct uint256 *request_hashes = malloc(
                max_batch * sizeof(struct uint256));
            if (request_hashes) {
                size_t num_requests = 0;
                walk = bi;
                while (walk && walk->nHeight > our_height &&
                       num_requests < max_batch) {
                    if (!(walk->nStatus & BLOCK_HAVE_DATA) && walk->phashBlock)
                        request_hashes[num_requests++] = *walk->phashBlock;
                    walk = walk->pprev;
                }

                if (num_requests > 0) {
                    struct byte_stream getdata_msg;
                    stream_init(&getdata_msg, num_requests * 36 + 8);
                    uint64_t block_count = 0;

                    for (size_t i = num_requests; i > 0; i--) {
                        struct inv_item inv;
                        inv_item_init_typed(&inv, MSG_BLOCK,
                                            &request_hashes[i - 1]);
                        inv_item_serialize(&inv, &getdata_msg);
                        block_count++;
                    }

                    struct byte_stream msg;
                    stream_init(&msg, getdata_msg.size + 8);
                    stream_write_compact_size(&msg, block_count);
                    stream_write(&msg, getdata_msg.data, getdata_msg.size);

                    p2p_node_begin_message(node, "getdata",
                                           mp->params->pchMessageStart);
                    p2p_node_write_message_data(node, msg.data, msg.size);
                    p2p_node_end_message(node);
                    stream_free(&msg);
                    stream_free(&getdata_msg);
                }
                free(request_hashes);
            }
        }
    }

    /* Request more headers if we accepted any */
    if (accepted > 0 && pindex_last)
        push_getheaders_from(mp, node, pindex_last);

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
    if (!stream_read_u8(s, &code))
        return true;  /* truncated reject is non-fatal */
    (void)code;
    (void)msg_type;
    return true;
}

static bool process_feefilter(struct p2p_node *node, struct byte_stream *s)
{
    uint64_t fee_rate = 0;
    if (!stream_read_u64_le(s, &fee_rate))
        return false;
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

    while (node->recv_msg_count > 0 && !node->disconnect) {
        zcl_mutex_lock(&node->cs_recv);
        if (node->recv_msg_count == 0 ||
            !net_message_complete(&node->recv_msgs[0])) {
            zcl_mutex_unlock(&node->cs_recv);
            break;
        }

        struct net_message msg = node->recv_msgs[0];
        memmove(&node->recv_msgs[0], &node->recv_msgs[1],
                (node->recv_msg_count - 1) * sizeof(struct net_message));
        node->recv_msg_count--;
        zcl_mutex_unlock(&node->cs_recv);

        /* Verify message checksum (first 4 bytes of double-SHA256) */
        struct uint256 msg_hash;
        hash256(msg.recv_data ? msg.recv_data : (const unsigned char *)"",
                msg.data_pos, msg_hash.data);
        unsigned int expected;
        memcpy(&expected, msg_hash.data, 4);
        if (expected != msg.hdr.nChecksum) {
            char ccmd[COMMAND_SIZE + 1];
            msg_header_get_command(&msg.hdr, ccmd, sizeof(ccmd));
            event_emitf(EV_MSG_CHECKSUM_FAIL, (uint32_t)node->id,
                        "%s size=%u", ccmd, msg.hdr.nMessageSize);
            printf("Peer %s: checksum mismatch on '%s' (size=%u exp=%08x got=%08x)\n",
                   node->addr_name, ccmd, msg.hdr.nMessageSize,
                   expected, msg.hdr.nChecksum);
            net_message_free(&msg);
            continue;
        }

        char cmd[COMMAND_SIZE + 1];
        msg_header_get_command(&msg.hdr, cmd, sizeof(cmd));

        /* Log every message received */
        event_emitf(EV_MSG_RECEIVED, (uint32_t)node->id,
                    "%s size=%u", cmd, msg.hdr.nMessageSize);

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
            node->disconnect = true;
            stream_free(&s);
            net_message_free(&msg);
            break;
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
        } else if (strcmp(cmd, MSG_SNAPSHOT_OFFER) == 0) {
            /* Peer offers a UTXO snapshot — parse and decide */
            int32_t h; uint8_t bh[32], ur[32]; uint64_t nu, tb;
            if (stream_read_i32_le(&s, &h) &&
                stream_read_bytes(&s, bh, 32) &&
                stream_read_bytes(&s, ur, 32) &&
                stream_read_u64_le(&s, &nu) &&
                stream_read_u64_le(&s, &tb)) {
                int our_h = active_chain_height(&mp->main_state->chain_active);
                printf("Peer %s: snapshot offer h=%d utxos=%llu (%lluMB)\n",
                       node->addr_name, h, (unsigned long long)nu,
                       (unsigned long long)(tb / (1024*1024)));
                /* Sanity-check snapshot offer values */
                if (nu > 100000000ULL || tb > 100ULL * 1024 * 1024 * 1024) {
                    printf("Peer %s: snapshot offer out of range (utxos=%llu, bytes=%llu)\n",
                           node->addr_name, (unsigned long long)nu,
                           (unsigned long long)tb);
                    peer_misbehaving(mp->net_mgr, node, 20,
                                     "snapshot offer out of range");
                } else if (h > our_h + 100 &&
                           node->state != PEER_SNAPSHOT_RECEIVING) {
                    /* Accept — request the snapshot data */
                    printf("Peer %s: accepting snapshot offer (h=%d, %llu UTXOs)\n",
                           node->addr_name, h, (unsigned long long)nu);
                    node->zsync_receiving = true;
                    node->zsync_offset = 0;
                    peer_set_state_checked((uint32_t)node->id, &node->state,
                                           PEER_SNAPSHOT_RECEIVING,
                                           "accepted snapshot offer");
                    event_emitf(EV_SNAPSHOT_OFFER_RECEIVED, (uint32_t)node->id,
                                "h=%d utxos=%llu", h, (unsigned long long)nu);
                    sync_set_state(SYNC_SNAPSHOT_RECEIVE, "peer snapshot");
                    /* Send zsnapreq to trigger chunk streaming */
                    p2p_node_begin_message(node, MSG_SNAPSHOT_REQ,
                                            mp->params->pchMessageStart);
                    struct byte_stream rq;
                    stream_init(&rq, 4);
                    stream_write_i32_le(&rq, our_h);
                    p2p_node_write_message_data(node, rq.data, rq.size);
                    p2p_node_end_message(node);
                    stream_free(&rq);
                    fflush(stdout);
                }
            }
        } else if (strcmp(cmd, MSG_SNAPSHOT_REQ) == 0) {
            /* Peer requests our UTXO snapshot */
            int32_t from_h = 0;
            if (!stream_read_i32_le(&s, &from_h)) {
                printf("Peer %s: truncated zsnapreq\n", node->addr_name);
                peer_misbehaving(mp->net_mgr, node, 10, "truncated zsnapreq");
                net_message_free(&msg);
                continue;
            }

            /* Verify PoW before serving snapshot */
            struct fast_sync_pow pow;
            memset(&pow, 0, sizeof(pow));
            bool has_pow = (s.size - s.read_pos >= sizeof(pow.peer_id) + 8 + 8);
            if (has_pow) {
                if (!stream_read_bytes(&s, pow.peer_id, 32) ||
                    !stream_read_i64_le(&s, &pow.timestamp) ||
                    !stream_read_u64_le(&s, &pow.nonce)) {
                    has_pow = false;  /* truncated PoW data */
                }
            }
            if (!has_pow || !fast_sync_verify_pow(&pow)) {
                printf("Peer %s: snapshot request rejected, invalid PoW\n",
                       node->addr_name);
                peer_misbehaving(mp->net_mgr, node, 20,
                                 "zsnapreq without valid PoW");
            /* Rate limit check */
            } else if (!fast_sync_rate_check(&g_rate_limiter,
                                              node->addr.svc.addr.ip)) {
                printf("Peer %s: rate limited, rejecting snapshot request\n",
                       node->addr_name);
            } else if (g_cached_offer_valid) {
                /* Send cached offer and start serving chunks */
                node->zsync_serving = true;
                node->zsync_offset = 0;
                node->zsync_sent = 0;
                peer_set_state_checked((uint32_t)node->id, &node->state,
                                       PEER_SNAPSHOT_SERVING,
                                       "serving snapshot request");
                node->zsync_cursor_valid = false;
                memset(node->zsync_cursor_txid, 0, 32);
                node->zsync_cursor_vout = 0;
                node->zsync_total = g_cached_offer.num_utxos;
                printf("Peer %s: serving snapshot (h=%d, %llu UTXOs)\n",
                       node->addr_name, g_cached_offer.height,
                       (unsigned long long)g_cached_offer.num_utxos);
                p2p_node_begin_message(node, MSG_SNAPSHOT_OFFER,
                                        mp->params->pchMessageStart);
                struct byte_stream os;
                stream_init(&os, 80);
                stream_write_i32_le(&os, g_cached_offer.height);
                stream_write_bytes(&os, g_cached_offer.block_hash, 32);
                stream_write_bytes(&os, g_cached_offer.utxo_root, 32);
                stream_write_u64_le(&os, g_cached_offer.num_utxos);
                stream_write_u64_le(&os, g_cached_offer.total_bytes);
                p2p_node_write_message_data(node, os.data, os.size);
                p2p_node_end_message(node);
                stream_free(&os);
            } else {
                printf("Peer %s: snapshot not ready yet\n", node->addr_name);
            }
        } else if (strcmp(cmd, MSG_SNAPSHOT_DATA) == 0) {
            /* Receive UTXO chunk — validate and apply */
            uint32_t entries = 0;
            if (!stream_read_u32_le(&s, &entries) || entries > 1000 ||
                entries == 0 ||
                node->state != PEER_SNAPSHOT_RECEIVING) {
                printf("Peer %s: bad snapshot chunk (entries=%u, state=%s)\n",
                       node->addr_name, entries,
                       peer_state_name(node->state));
                node->disconnect = true;
            } else {
                char db_path[1024];
                snprintf(db_path, sizeof(db_path), "%s/node.db", mp->datadir);
                sqlite3 *db = NULL;
                int rc = sqlite3_open_v2(db_path, &db,
                                          SQLITE_OPEN_READWRITE, NULL);
                if (rc != SQLITE_OK) {
                    printf("Peer %s: snapshot db open failed: %s\n",
                           node->addr_name, sqlite3_errmsg(db));
                    if (db) sqlite3_close(db);
                    node->disconnect = true;
                } else {
                    char *errmsg = NULL;
                    rc = sqlite3_exec(db, "BEGIN", NULL, NULL, &errmsg);
                    if (rc != SQLITE_OK) {
                        printf("Peer %s: snapshot BEGIN failed: %s\n",
                               node->addr_name, errmsg ? errmsg : "unknown");
                        sqlite3_free(errmsg);
                        sqlite3_close(db);
                        node->disconnect = true;
                    } else {
                    sqlite3_stmt *ins = NULL;
                    rc = sqlite3_prepare_v2(db,
                        "INSERT OR IGNORE INTO utxos "
                        "(txid,vout,value,script,script_type,height) "
                        "VALUES(?,?,?,?,0,?)", -1, &ins, NULL);
                    if (rc != SQLITE_OK || !ins) {
                        printf("Peer %s: snapshot prepare failed: %s\n",
                               node->addr_name, sqlite3_errmsg(db));
                        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
                        sqlite3_close(db);
                        node->disconnect = true;
                    } else {
                    uint32_t applied = 0;
                    bool chunk_valid = true;
                    for (uint32_t i = 0; i < entries; i++) {
                        uint8_t txid[32];
                        int32_t vout, height;
                        int64_t value;
                        uint64_t slen;
                        if (!stream_read_bytes(&s, txid, 32) ||
                            !stream_read_i32_le(&s, &vout) ||
                            !stream_read_i64_le(&s, &value) ||
                            !stream_read_i32_le(&s, &height) ||
                            !stream_read_compact_size(&s, &slen)) {
                            printf("Peer %s: truncated UTXO in chunk\n",
                                   node->addr_name);
                            peer_misbehaving(mp->net_mgr, node, 20,
                                             "truncated snapshot chunk");
                            chunk_valid = false;
                            break;
                        }

                        /* Validate UTXO data — fail fast */
                        if (vout < 0 || value < 0 ||
                            value > 2100000000000000LL ||
                            height < 0 || height > 100000000 ||
                            slen > 520) {
                            printf("Peer %s: invalid UTXO in chunk "
                                   "(vout=%d val=%lld h=%d slen=%llu)\n",
                                   node->addr_name, vout, (long long)value,
                                   height, (unsigned long long)slen);
                            peer_misbehaving(mp->net_mgr, node, 50,
                                             "invalid UTXO data");
                            chunk_valid = false;
                            break;
                        }

                        uint8_t script[520];
                        if (slen > 0 &&
                            !stream_read_bytes(&s, script, (size_t)slen)) {
                            peer_misbehaving(mp->net_mgr, node, 20,
                                             "truncated script in chunk");
                            chunk_valid = false;
                            break;
                        }

                        sqlite3_reset(ins);
                        sqlite3_bind_blob(ins, 1, txid, 32, SQLITE_STATIC);
                        sqlite3_bind_int(ins, 2, vout);
                        sqlite3_bind_int64(ins, 3, value);
                        sqlite3_bind_blob(ins, 4, script, (int)slen,
                                           SQLITE_STATIC);
                        sqlite3_bind_int(ins, 5, height);
                        rc = sqlite3_step(ins);
                        if (rc != SQLITE_DONE && rc != SQLITE_CONSTRAINT) {
                            printf("Peer %s: snapshot insert failed: %s\n",
                                   node->addr_name, sqlite3_errmsg(db));
                            chunk_valid = false;
                            break;
                        }
                        applied++;
                    }
                    sqlite3_finalize(ins);

                    if (chunk_valid) {
                        rc = sqlite3_exec(db, "COMMIT", NULL, NULL, &errmsg);
                        if (rc != SQLITE_OK) {
                            printf("Peer %s: snapshot COMMIT failed: %s\n",
                                   node->addr_name,
                                   errmsg ? errmsg : "unknown");
                            sqlite3_free(errmsg);
                            node->disconnect = true;
                        }
                    } else {
                        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
                        node->disconnect = true;
                    }
                    sqlite3_close(db);

                    if (chunk_valid) {
                        node->zsync_offset += applied;
                        if (node->zsync_offset % 50000 < entries)
                            printf("Peer %s: received %llu UTXOs\n",
                                   node->addr_name,
                                   (unsigned long long)node->zsync_offset);
                    }
                    } /* ins prepared */
                    } /* BEGIN succeeded */
                }
            }
        } else if (strcmp(cmd, MSG_SNAPSHOT_END) == 0) {
            printf("Peer %s: snapshot transfer complete (%llu UTXOs)\n",
                   node->addr_name,
                   (unsigned long long)node->zsync_offset);
            node->zsync_receiving = false;
            event_emitf(EV_SNAPSHOT_COMPLETE, (uint32_t)node->id,
                        "utxos=%llu",
                        (unsigned long long)node->zsync_offset);
            peer_set_state_checked((uint32_t)node->id, &node->state,
                                   PEER_ACTIVE, "snapshot complete");
            sync_set_state(SYNC_HEADERS_DOWNLOAD,
                           "snapshot done, sync remaining headers");
            fflush(stdout);

        /* ── Parallel chunk sync messages ────────────────────── */

        } else if (strcmp(cmd, MSG_MANIFEST) == 0) {
            /* Peer sends their manifest — describes available chunks. */
            int32_t height = 0;
            uint8_t block_hash[32], merkle_root[32];
            uint64_t num_utxos = 0;
            uint32_t num_chunks = 0, chunk_size = 0;

            if (stream_read_i32_le(&s, &height) &&
                stream_read_bytes(&s, block_hash, 32) &&
                stream_read_u64_le(&s, &num_utxos) &&
                stream_read_u32_le(&s, &num_chunks) &&
                stream_read_u32_le(&s, &chunk_size) &&
                stream_read_bytes(&s, merkle_root, 32)) {

                node->swarm_manifest_received = true;
                int our_h = active_chain_height(&mp->main_state->chain_active);
                printf("Peer %s: manifest h=%d chunks=%u (%llu UTXOs)\n",
                       node->addr_name, height, num_chunks,
                       (unsigned long long)num_utxos);

                /* If peer is significantly ahead and we have no active swarm,
                 * initialize the swarm coordinator from their manifest. */
                if (height > our_h + 100 && !g_swarm_active &&
                    num_chunks > 0 && chunk_size > 0) {
                    struct sync_manifest peer_manifest = {
                        .height = height,
                        .num_utxos = num_utxos,
                        .num_chunks = num_chunks,
                        .chunk_size = chunk_size,
                        .chunk_hashes = NULL
                    };
                    memcpy(peer_manifest.block_hash, block_hash, 32);
                    memcpy(peer_manifest.merkle_root, merkle_root, 32);

                    if (swarm_sync_init(&g_swarm, &peer_manifest, mp->datadir)) {
                        g_swarm_active = true;
                        g_swarm_last_progress_time = (int64_t)time(NULL);
                        printf("Swarm sync started: %u chunks from h=%d\n",
                               num_chunks, height);
                        fflush(stdout);
                    }
                }
            }

        } else if (strcmp(cmd, MSG_CHUNK_REQ) == 0) {
            /* Peer requests a specific chunk by index — serve it. */
            uint32_t chunk_index = 0;
            if (!stream_read_u32_le(&s, &chunk_index)) {
                printf("Peer %s: bad zchunkreq\n", node->addr_name);
            } else if (!g_cached_manifest_valid) {
                printf("Peer %s: zchunkreq but no manifest ready\n",
                       node->addr_name);
            } else if (chunk_index >= g_cached_manifest.num_chunks) {
                printf("Peer %s: zchunkreq index %u out of range (%u)\n",
                       node->addr_name, chunk_index,
                       g_cached_manifest.num_chunks);
                peer_misbehaving(mp->net_mgr, node, 10,
                                 "zchunkreq out of range");
            } else if (!fast_sync_rate_check(&g_rate_limiter,
                                              node->addr.svc.addr.ip)) {
                printf("Peer %s: rate limited on chunk request\n",
                       node->addr_name);
            } else {
                struct utxo_chunk *chunk = calloc(1, sizeof(struct utxo_chunk));
                if (chunk &&
                    fast_sync_serve_chunk(mp->datadir, chunk_index, chunk)) {
                    /* Serialize chunk data into message. */
                    struct byte_stream cs;
                    stream_init(&cs, 65536);
                    stream_write_u32_le(&cs, chunk->chunk_index);
                    stream_write_u32_le(&cs, chunk->num_entries);
                    for (uint32_t i = 0; i < chunk->num_entries; i++) {
                        stream_write_bytes(&cs, chunk->entries[i].txid, 32);
                        stream_write_i32_le(&cs, (int32_t)chunk->entries[i].vout);
                        stream_write_i64_le(&cs, chunk->entries[i].value);
                        stream_write_i32_le(&cs, chunk->entries[i].height);
                        stream_write_u16_le(&cs, chunk->entries[i].script_len);
                        if (chunk->entries[i].script_len > 0)
                            stream_write_bytes(&cs, chunk->entries[i].script,
                                               chunk->entries[i].script_len);
                    }

                    p2p_node_begin_message(node, MSG_CHUNK_DATA,
                                            mp->params->pchMessageStart);
                    p2p_node_write_message_data(node, cs.data, cs.size);
                    p2p_node_end_message(node);
                    stream_free(&cs);
                }
                free(chunk);
            }

        } else if (strcmp(cmd, MSG_CHUNK_DATA) == 0) {
            /* Peer sends chunk data in response to our request. */
            uint32_t chunk_index = 0, num_entries = 0;
            if (!stream_read_u32_le(&s, &chunk_index) ||
                !stream_read_u32_le(&s, &num_entries) ||
                num_entries > 1000) {
                printf("Peer %s: bad zchunkdata header\n", node->addr_name);
                peer_misbehaving(mp->net_mgr, node, 20, "bad zchunkdata");
            } else if (!g_swarm_active) {
                printf("Peer %s: zchunkdata but no swarm active\n",
                       node->addr_name);
            } else {
                struct utxo_chunk *chunk = calloc(1, sizeof(struct utxo_chunk));
                if (chunk) {
                    chunk->chunk_index = chunk_index;
                    chunk->num_entries = num_entries;
                    bool parse_ok = true;

                    for (uint32_t i = 0; i < num_entries && parse_ok; i++) {
                        if (!stream_read_bytes(&s, chunk->entries[i].txid, 32))
                            { parse_ok = false; break; }
                        int32_t vout = 0;
                        if (!stream_read_i32_le(&s, &vout))
                            { parse_ok = false; break; }
                        chunk->entries[i].vout = (uint32_t)vout;
                        if (!stream_read_i64_le(&s, &chunk->entries[i].value))
                            { parse_ok = false; break; }
                        if (!stream_read_i32_le(&s, &chunk->entries[i].height))
                            { parse_ok = false; break; }
                        uint16_t slen = 0;
                        if (!stream_read_u16_le(&s, &slen))
                            { parse_ok = false; break; }
                        chunk->entries[i].script_len = slen;
                        if (slen > 128) slen = 128;
                        if (slen > 0 &&
                            !stream_read_bytes(&s, chunk->entries[i].script, slen))
                            { parse_ok = false; break; }
                    }

                    if (parse_ok) {
                        bool verified = swarm_sync_receive_chunk(
                            &g_swarm, chunk, node->id);
                        node->swarm_inflight_chunk = -1;

                        if (!verified) {
                            printf("Peer %s: chunk %u failed verification\n",
                                   node->addr_name, chunk_index);
                            peer_misbehaving(mp->net_mgr, node, 50,
                                             "bad chunk hash");
                        }

                        if (swarm_sync_is_complete(&g_swarm)) {
                            printf("Swarm sync complete: %u/%u chunks\n",
                                   g_swarm.chunks_complete,
                                   g_swarm.manifest.num_chunks);
                            swarm_sync_free(&g_swarm);
                            g_swarm_active = false;
                            fflush(stdout);
                        }
                    } else {
                        printf("Peer %s: truncated zchunkdata\n",
                               node->addr_name);
                        peer_misbehaving(mp->net_mgr, node, 20,
                                         "truncated zchunkdata");
                    }
                    free(chunk);
                }
            }

        } else if (strcmp(cmd, MSG_GAME) == 0) {
            /* P2P game message */
            uint8_t game_type = 0, position = 0;
            struct ttt_state peer_state;
            memset(&peer_state, 0, sizeof(peer_state));
            enum game_action action = game_deserialize(
                s.data + s.read_pos, s.size - s.read_pos,
                &game_type, &position, &peer_state);

            switch (action) {
            case GAME_INVITE:
                printf("Peer %s: game invite (type=%d)\n",
                       node->addr_name, game_type);
                /* Auto-accept for now */
                {
                    uint8_t resp[8];
                    size_t rn = game_serialize_accept(resp, sizeof(resp), 2);
                    p2p_node_begin_message(node, MSG_GAME,
                                            mp->params->pchMessageStart);
                    p2p_node_write_message_data(node, resp, rn);
                    p2p_node_end_message(node);
                    printf("Peer %s: auto-accepted game as O\n",
                           node->addr_name);
                }
                break;
            case GAME_ACCEPT:
                printf("Peer %s: game accepted\n", node->addr_name);
                break;
            case GAME_MOVE:
                printf("Peer %s: game move position=%d\n",
                       node->addr_name, position);
                /* Measure latency from timestamp in message */
                if (s.size - s.read_pos >= 11) {
                    int64_t send_ts = 0;
                    memcpy(&send_ts, s.data + s.read_pos + 3, 8);
                    struct timeval tv;
                    gettimeofday(&tv, NULL);
                    int64_t now_us = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
                    int64_t latency = now_us - send_ts;
                    if (latency > 0 && latency < 60000000)
                        printf("Peer %s: P2P latency = %lld us (%.1f ms)\n",
                               node->addr_name, (long long)latency,
                               (double)latency / 1000.0);
                }
                break;
            case GAME_STATE:
                {
                    char board[256];
                    ttt_render(&peer_state, board, sizeof(board));
                    printf("Peer %s: game state\n%s\n",
                           node->addr_name, board);
                }
                break;
            case GAME_RESULT:
                printf("Peer %s: game result\n", node->addr_name);
                break;
            default:
                break;
            }
            fflush(stdout);
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
    if (!chain || active_chain_height(chain) < 0) {
        loc->vhave = NULL;
        loc->num_hashes = 0;
        return;
    }

    struct uint256 tmp[64];
    size_t idx = 0;
    int step = 1;

    struct block_index *tip = active_chain_tip(chain);
    struct block_index *bi = tip;
    while (bi && bi->phashBlock && idx < 63) {
        tmp[idx++] = *bi->phashBlock;
        if (bi->nHeight <= 0) break;
        int target = bi->nHeight - step;
        if (target < 0) target = 0;
        /* Walk pprev to target height */
        while (bi->pprev && bi->pprev->nHeight > target)
            bi = bi->pprev;
        if (bi->pprev)
            bi = bi->pprev;
        else
            break;
        if (idx >= 10) step *= 2;
    }

    if (idx == 0) { loc->vhave = NULL; loc->num_hashes = 0; return; }
    if (tip)
        printf("Locator: %zu hashes, tip h=%d, last h=%d\n",
               idx, tip->nHeight, bi ? bi->nHeight : -1);
    fflush(stdout);
    loc->vhave = calloc(idx, sizeof(struct uint256));
    if (!loc->vhave) { loc->num_hashes = 0; return; }
    memcpy(loc->vhave, tmp, idx * sizeof(struct uint256));
    loc->num_hashes = idx;
}

static void build_block_locator_from_index(struct block_locator *loc,
                                            struct block_index *pindex)
{
    /* Collect hashes into a temporary array, then allocate */
    struct uint256 tmp[64];
    size_t idx = 0;
    int step = 1;
    struct block_index *p = pindex;

    while (p && p->phashBlock && idx < 63) {
        tmp[idx++] = *p->phashBlock;
        if (p->nHeight <= 0) break;
        int target = p->nHeight - step;
        if (target < 0) target = 0;
        while (p && p->nHeight > target) {
            if (!p->pprev) break;
            p = p->pprev;
        }
        if (!p || !p->phashBlock) break;
        if (idx > 10) step *= 2;
    }

    loc->vhave = calloc(idx, sizeof(struct uint256));
    if (!loc->vhave) { loc->num_hashes = 0; return; }
    memcpy(loc->vhave, tmp, idx * sizeof(struct uint256));
    loc->num_hashes = idx;
}

static void push_getheaders_from(struct msg_processor *mp,
                                  struct p2p_node *node,
                                  struct block_index *from)
{
    if (from && !from->phashBlock) return;

    struct block_locator loc;
    block_locator_init(&loc);

    if (from)
        build_block_locator_from_index(&loc, from);
    else
        build_block_locator(&loc, &mp->main_state->chain_active);

    /* If locator is empty, skip — chain not ready */
    if (loc.num_hashes == 0) {
        /* Empty chain — send genesis hash so peer knows to start from block 1 */
        loc.vhave = malloc(sizeof(struct uint256));
        if (loc.vhave) {
            loc.vhave[0] = mp->params->consensus.hashGenesisBlock;
            loc.num_hashes = 1;
        } else {
            block_locator_free(&loc);
            return;
        }
    }

    /* Ensure genesis hash is always at the end of the locator. */
    bool has_genesis = false;
    for (size_t i = 0; i < loc.num_hashes; i++) {
        if (uint256_eq(&loc.vhave[i], &mp->params->consensus.hashGenesisBlock)) {
            has_genesis = true;
            break;
        }
    }
    if (!has_genesis && loc.num_hashes > 0) {
        struct uint256 *new_vhave = realloc(loc.vhave,
            (loc.num_hashes + 1) * sizeof(struct uint256));
        if (new_vhave) {
            loc.vhave = new_vhave;
            loc.vhave[loc.num_hashes] = mp->params->consensus.hashGenesisBlock;
            loc.num_hashes++;
        }
    }

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

static void push_getheaders(struct msg_processor *mp, struct p2p_node *node)
{
    push_getheaders_from(mp, node, NULL);
}

bool msg_send_messages(void *ctx, struct p2p_node *node, bool send_trickle)
{
    struct msg_processor *mp = (struct msg_processor *)ctx;

    /* Outbound nodes: send version to initiate handshake */
    if (node->state < PEER_HANDSHAKE_COMPLETE) {
        if (!node->inbound && node->send_bytes == 0) {
            push_version(mp, node);
            peer_set_state_checked((uint32_t)node->id, &node->state,
                                   PEER_VERSION_SENT, "outbound version sent");
        }
        return true;
    }

    /* Transition to ACTIVE if still at HANDSHAKE_COMPLETE */
    if (node->state == PEER_HANDSHAKE_COMPLETE)
        peer_set_state_checked((uint32_t)node->id, &node->state,
                               PEER_ACTIVE, "ready for sync");

    /* Offer fast sync to ZCL23 peers that are behind us */
    if (peer_supports_fast_sync(node->services) &&
        node->state != PEER_SNAPSHOT_SERVING &&
        node->state != PEER_SNAPSHOT_RECEIVING &&
        node->zsync_sent == 0 && /* only offer once */
        g_cached_offer_valid) {
        int our_h = msg_get_height(mp);
        if (our_h > 100 &&
            (node->starting_height < 0 ||
             our_h > node->starting_height + 100)) {
            node->zsync_sent = UINT64_MAX; /* mark: offered */
            event_emitf(EV_SNAPSHOT_OFFER_SENT, (uint32_t)node->id,
                        "h=%d utxos=%llu", g_cached_offer.height,
                        (unsigned long long)g_cached_offer.num_utxos);
            printf("Peer %s: offering snapshot (us=%d, peer=%d)\n",
                   node->addr_name, our_h, node->starting_height);
            p2p_node_begin_message(node, MSG_SNAPSHOT_OFFER,
                                    mp->params->pchMessageStart);
            struct byte_stream os;
            stream_init(&os, 80);
            stream_write_i32_le(&os, g_cached_offer.height);
            stream_write_bytes(&os, g_cached_offer.block_hash, 32);
            stream_write_bytes(&os, g_cached_offer.utxo_root, 32);
            stream_write_u64_le(&os, g_cached_offer.num_utxos);
            stream_write_u64_le(&os, g_cached_offer.total_bytes);
            p2p_node_write_message_data(node, os.data, os.size);
            p2p_node_end_message(node);
            stream_free(&os);
            fflush(stdout);
        }
    }

    /* Initiate sync, and periodically re-request if behind. */
    {
        bool should_sync = false;
        if (node->state == PEER_ACTIVE && !node->inbound) {
            node->sync_started = true; /* keep for backward compat */
            peer_set_state_checked((uint32_t)node->id, &node->state,
                                   PEER_SYNCING_HEADERS, "IBD start");
            if (sync_get_state() == SYNC_IDLE ||
                sync_get_state() == SYNC_FINDING_PEERS)
                sync_set_state(SYNC_HEADERS_DOWNLOAD, "first outbound peer");
            should_sync = true;
        }

        int our_height = msg_get_height(mp);
        bool in_ibd = (node->starting_height > 0 &&
                       our_height < node->starting_height - 144);

        /* Re-request headers aggressively during IBD (10s), slower at tip (60s).
         * This is critical: legacy zclassicd sends at most 2000 headers per
         * getheaders response — for a 3M block chain, we need ~1500 rounds. */
        int64_t now_send = (int64_t)time(NULL);
        int64_t resync_interval = in_ibd ? 10 : 60;
        if (node->state >= PEER_SYNCING_HEADERS && !node->inbound &&
            node->starting_height > our_height &&
            now_send - node->last_getheaders_time > resync_interval) {
            should_sync = true;
            node->last_getheaders_time = now_send;
        }
        if (should_sync) {
            struct block_index *tip = active_chain_tip(
                &mp->main_state->chain_active);
            if (tip && tip->phashBlock) {
                if (in_ibd) {
                    /* Only log every 10th request during IBD to avoid spam */
                    static int getheaders_log_count = 0;
                    if (getheaders_log_count++ % 10 == 0)
                        printf("IBD getheaders to %s (height=%d, peer=%d, "
                               "behind=%d)\n",
                               node->addr_name, tip->nHeight,
                               node->starting_height,
                               node->starting_height - tip->nHeight);
                } else {
                    printf("Sending getheaders to %s (height=%d, peer=%d)\n",
                           node->addr_name, tip->nHeight,
                           node->starting_height);
                }
            }
            push_getheaders(mp, node);
        }
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

    /* Stream fast sync UTXO chunks if serving this peer */
    if (node->state == PEER_SNAPSHOT_SERVING && g_cached_offer_valid) {
        /* Send up to 10 chunks per tick (non-blocking) */
        char db_path[1024];
        snprintf(db_path, sizeof(db_path), "%s/node.db", mp->datadir);
        sqlite3 *db = NULL;
        if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) {
            /* Use keyset pagination (WHERE > cursor) instead of OFFSET
             * to avoid O(n) skip cost on large UTXO sets */
            sqlite3_stmt *sel_first = NULL, *sel_keyset = NULL;
            sqlite3_prepare_v2(db,
                "SELECT txid, vout, value, script, height "
                "FROM utxos ORDER BY txid, vout LIMIT 500",
                -1, &sel_first, NULL);
            sqlite3_prepare_v2(db,
                "SELECT txid, vout, value, script, height "
                "FROM utxos WHERE (txid > ?) OR (txid = ? AND vout > ?) "
                "ORDER BY txid, vout LIMIT 500", -1, &sel_keyset, NULL);

            for (int batch = 0; batch < 10; batch++) {
                sqlite3_stmt *sel;
                if (node->zsync_cursor_valid) {
                    sel = sel_keyset;
                    sqlite3_reset(sel);
                    sqlite3_bind_blob(sel, 1, node->zsync_cursor_txid, 32, SQLITE_STATIC);
                    sqlite3_bind_blob(sel, 2, node->zsync_cursor_txid, 32, SQLITE_STATIC);
                    sqlite3_bind_int(sel, 3, node->zsync_cursor_vout);
                } else {
                    sel = sel_first;
                    sqlite3_reset(sel);
                }

                struct byte_stream chunk;
                stream_init(&chunk, 32768);
                uint32_t entries = 0;
                /* Reserve space for entry count */
                stream_write_u32_le(&chunk, 0);

                while (sqlite3_step(sel) == SQLITE_ROW && entries < 500) {
                    const void *txid = sqlite3_column_blob(sel, 0);
                    int32_t vout = sqlite3_column_int(sel, 1);
                    int64_t value = sqlite3_column_int64(sel, 2);
                    const void *script = sqlite3_column_blob(sel, 3);
                    int slen = sqlite3_column_bytes(sel, 3);
                    int32_t height = sqlite3_column_int(sel, 4);

                    if (txid) stream_write_bytes(&chunk, txid, 32);
                    else { uint8_t z[32] = {0}; stream_write_bytes(&chunk, z, 32); }
                    stream_write_i32_le(&chunk, vout);
                    stream_write_i64_le(&chunk, value);
                    stream_write_i32_le(&chunk, height);
                    if (slen > 520) slen = 520;
                    stream_write_compact_size(&chunk, (uint64_t)slen);
                    if (script && slen > 0)
                        stream_write_bytes(&chunk, script, (size_t)slen);
                    entries++;

                    /* Update keyset cursor for next batch */
                    if (txid) memcpy(node->zsync_cursor_txid, txid, 32);
                    node->zsync_cursor_vout = vout;
                    node->zsync_cursor_valid = true;
                }

                if (entries == 0) {
                    /* Done — send end marker */
                    stream_free(&chunk);
                    p2p_node_begin_message(node, MSG_SNAPSHOT_END,
                                            mp->params->pchMessageStart);
                    p2p_node_end_message(node);
                    node->zsync_serving = false;
                    peer_set_state_checked((uint32_t)node->id, &node->state,
                                           PEER_ACTIVE, "snapshot serve done");
                    printf("Peer %s: snapshot complete (%llu UTXOs sent)\n",
                           node->addr_name,
                           (unsigned long long)node->zsync_offset);
                    fflush(stdout);
                    break;
                }

                /* Patch entry count at offset 0 */
                chunk.data[0] = (uint8_t)(entries & 0xFF);
                chunk.data[1] = (uint8_t)((entries >> 8) & 0xFF);
                chunk.data[2] = (uint8_t)((entries >> 16) & 0xFF);
                chunk.data[3] = (uint8_t)((entries >> 24) & 0xFF);

                p2p_node_begin_message(node, MSG_SNAPSHOT_DATA,
                                        mp->params->pchMessageStart);
                p2p_node_write_message_data(node, chunk.data, chunk.size);
                p2p_node_end_message(node);
                stream_free(&chunk);

                node->zsync_offset += entries;
                node->zsync_sent++;

                if (node->zsync_sent % 100 == 0) {
                    printf("Peer %s: sent %llu/%llu UTXOs\n",
                           node->addr_name,
                           (unsigned long long)node->zsync_offset,
                           (unsigned long long)g_cached_offer.num_utxos);
                    fflush(stdout);
                }
            }
            sqlite3_finalize(sel_first);
            sqlite3_finalize(sel_keyset);
            sqlite3_close(db);
        }
    }

    /* ── Swarm parallel chunk sync coordinator ────────────── */
    /* For each connected ZCL23 peer with no inflight chunk, assign one
     * and send a zchunkreq. Also handle timeouts on stale requests. */
    if (g_swarm_active && peer_supports_fast_sync(node->services) &&
        node->swarm_manifest_received &&
        node->state >= PEER_HANDSHAKE_COMPLETE) {

        /* Handle timeout: if this peer's chunk is stale, re-queue it */
        if (node->swarm_inflight_chunk >= 0) {
            int64_t now_sw = (int64_t)time(NULL);
            if (now_sw - node->swarm_chunk_req_time > SWARM_CHUNK_TIMEOUT_SECS) {
                uint32_t ci = (uint32_t)node->swarm_inflight_chunk;
                if (ci < g_swarm.manifest.num_chunks &&
                    g_swarm.chunk_states[ci] == CHUNK_INFLIGHT) {
                    g_swarm.chunk_states[ci] = CHUNK_NEEDED;
                    g_swarm.chunk_peer[ci] = -1;
                    if (g_swarm.chunks_inflight > 0)
                        g_swarm.chunks_inflight--;
                    printf("Peer %s: chunk %u timed out, re-queuing\n",
                           node->addr_name, ci);
                }
                node->swarm_inflight_chunk = -1;
            }
        }

        /* If peer has no inflight chunk, assign the next needed one */
        if (node->swarm_inflight_chunk < 0) {
            int32_t ci = swarm_sync_assign_chunk(&g_swarm, node->id);
            if (ci >= 0) {
                node->swarm_inflight_chunk = ci;
                node->swarm_chunk_req_time = (int64_t)time(NULL);
                push_chunk_request(mp, node, (uint32_t)ci);
            }
        }

        /* Progress display (rate-limited to every 5 seconds).
         * Only print from the first peer's tick to avoid duplicates. */
        int64_t now_prog = (int64_t)time(NULL);
        if (now_prog - g_swarm_last_progress_time >= SWARM_PROGRESS_INTERVAL_SECS) {
            g_swarm_last_progress_time = now_prog;

            /* Count serving peers */
            int serving_peers = 0;
            if (mp->net_mgr) {
                for (size_t i = 0; i < mp->net_mgr->num_nodes; i++) {
                    struct p2p_node *n = mp->net_mgr->nodes[i];
                    if (n && peer_supports_fast_sync(n->services) &&
                        n->swarm_manifest_received)
                        serving_peers++;
                }
            }

            printf("Sync: %d%% (%u/%u chunks, %u inflight, %d peers serving)\n",
                   swarm_sync_progress(&g_swarm),
                   g_swarm.chunks_complete, g_swarm.manifest.num_chunks,
                   g_swarm.chunks_inflight, serving_peers);
            fflush(stdout);
        }
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
