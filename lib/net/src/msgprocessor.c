/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "net/msgprocessor.h"
#include "net/addrman.h"
#include "net/file_service.h"
#include "models/peer.h"
#include "models/block.h"
#include "models/file_service.h"
#include "models/database.h"
#include "net/fast_sync.h"
#include "coins/utxo_commitment.h"
#include "net/p2p_game.h"
#include "net/version.h"
#include "net/p2p_message.h"
#include "services/header_sync_service.h"
#include "services/block_sync_service.h"
#include "core/hash.h"
#include "core/random.h"
#include "core/serialize.h"
#include "services/snapshot_sync_service.h"
#include "controllers/blockchain_controller.h"
#include "models/mmb_leaf_store.h"
#include "net/flyclient.h"
#include "chain/mmb.h"
#include "consensus/upgrades.h"
#include "consensus/validation.h"
#include "validation/check_transaction.h"
#include "validation/process_block.h"
#include "controllers/sync_controller.h"
#include "storage/disk_block_io.h"
#include "wallet/wallet.h"
#include "util/timedata.h"
#include "event/event.h"
#include "net/download.h"
#include "util/sync.h"
#include "config/boot_internal.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>

static void push_getheaders(struct msg_processor *mp, struct p2p_node *node);
static void push_getheaders_from(struct msg_processor *mp,
                                  struct p2p_node *node,
                                  struct block_index *from);
static void exec_getheaders_action(struct msg_processor *mp,
                                   struct p2p_node *node,
                                   const struct sync_getheaders_action *action);

/* Cached snapshot offer — pre-computed at startup, not in message handler.
 * Protected by g_offer_mutex to prevent struct tearing between the
 * background build thread (boot.c) and the P2P message handler. */
static struct snapshot_offer g_cached_offer;
static _Atomic bool g_cached_offer_valid = false;
static _Atomic uint64_t g_cached_offer_version = 0;
static pthread_mutex_t g_offer_mutex = PTHREAD_MUTEX_INITIALIZER;
struct fast_sync_rate_limiter g_rate_limiter = {0};

/* Cached manifest for parallel chunk sync (built in background at startup). */
struct sync_manifest g_cached_manifest;
_Atomic bool g_cached_manifest_valid = false;
static _Atomic uint64_t g_cached_manifest_version = 0;
static pthread_mutex_t g_manifest_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Global swarm coordinator — manages parallel UTXO chunk download.
 * Only active when we are syncing from multiple ZCL23 peers.
 * All access to g_swarm fields protected by g_swarm_mutex. */
static struct swarm_sync g_swarm __attribute__((used));
static _Atomic bool g_swarm_active = false;
static pthread_mutex_t g_swarm_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Snapshot sync service — global singleton in snapshot_sync_service.c */
static int64_t g_swarm_last_progress_time = 0;

/* Timeout for inflight chunk requests (30 seconds). */
#define SWARM_CHUNK_TIMEOUT_SECS 30

/* Progress display interval (5 seconds). */
#define SWARM_PROGRESS_INTERVAL_SECS 5

/* ── Block swarm: parallel block download coordinator ───────── */
/* Manages BitTorrent-style block piece download across multiple
 * ZCL23 peers. Legacy peers contribute blocks via normal getdata/block
 * which the coordinator assembles into verified pieces. */
static struct block_swarm g_block_swarm __attribute__((used));
static _Atomic bool g_block_swarm_active = false;
static pthread_mutex_t g_block_swarm_mutex = PTHREAD_MUTEX_INITIALIZER;
static int64_t g_block_swarm_last_progress = 0;

/* Cached block piece manifest (built in background).
 * Non-static: accessed from boot.c via extern. */
struct block_piece_manifest g_cached_block_manifest;
_Atomic bool g_cached_block_manifest_valid = false;
int32_t g_manifest_built_at_height = 0; /* height when manifest was last built */
static _Atomic uint64_t g_cached_block_manifest_version = 0;
static pthread_mutex_t g_block_manifest_mutex = PTHREAD_MUTEX_INITIALIZER;
/* Rebuild manifest when chain grows this many blocks beyond the cached one.
 * This ensures new peers always get a reasonably fresh manifest. */
#define MANIFEST_REFRESH_BLOCKS 1000

/* Global download manager — coordinates parallel block downloads.
 * Initialized once from msg_processor_init, accessed via pointer. */
static struct download_manager g_download_mgr;
static bool g_download_mgr_init = false;
static pthread_once_t g_download_mgr_once = PTHREAD_ONCE_INIT;

static void msg_download_mgr_init_once(void)
{
    dl_init(&g_download_mgr);
    g_download_mgr_init = true;
}

static void msg_manifest_reset(struct sync_manifest *manifest)
{
    if (!manifest)
        return;
    sync_manifest_free(manifest);
    memset(manifest, 0, sizeof(*manifest));
}

static void msg_block_manifest_reset(struct block_piece_manifest *manifest)
{
    if (!manifest)
        return;
    block_piece_manifest_free(manifest);
    memset(manifest, 0, sizeof(*manifest));
}

static bool msg_manifest_is_reasonable(const struct sync_manifest *manifest)
{
    if (!manifest)
        return false;
    if (manifest->num_chunks == 0)
        return false;
    if (manifest->chunk_size == 0)
        return false;
    if (!manifest->chunk_hashes)
        return false;
    return true;
}

static bool msg_block_manifest_is_reasonable(
    const struct block_piece_manifest *manifest)
{
    if (!manifest)
        return false;
    if (manifest->num_pieces == 0)
        return false;
    if (manifest->start_height > manifest->end_height)
        return false;
    if (!manifest->piece_hashes)
        return false;
    return true;
}

static struct node_db *msg_node_db(const struct msg_processor *mp)
{
    if (!mp || !mp->runtime)
        return NULL;
    return db_service_node_db(mp->runtime->db_service);
}

static struct snapshot_sync_service *msg_snapshot_sync(
    const struct msg_processor *mp)
{
    if (mp && mp->runtime && mp->runtime->snapshot_sync)
        return mp->runtime->snapshot_sync;
    if (snapsync_global_initialized())
        return snapsync_global();
    return NULL;
}

static struct snapshot_sync_service *msg_snapshot_sync_ensure(
    const struct msg_processor *mp)
{
    struct snapshot_sync_service *svc = msg_snapshot_sync(mp);
    struct node_db *ndb;

    if (svc)
        return svc;
    ndb = msg_node_db(mp);
    if (!ndb)
        return NULL;
    snapsync_global_ensure_init(ndb);
    return snapsync_global();
}

static struct wallet *msg_wallet(const struct msg_processor *mp)
{
    if (!mp || !mp->runtime)
        return NULL;
    return mp->runtime->wallet;
}

struct download_manager *msg_get_download_mgr(void)
{
    pthread_once(&g_download_mgr_once, msg_download_mgr_init_once);
    return &g_download_mgr;
}

#define get_download_mgr() msg_get_download_mgr()

/* Thread-safe accessor: update cached snapshot offer from boot.c */
void msg_processor_update_offer(const struct snapshot_offer *offer)
{
    if (!offer)
        return;
    pthread_mutex_lock(&g_offer_mutex);
    g_cached_offer = *offer;
    atomic_store(&g_cached_offer_valid, true);
    g_cached_offer_version++;
    pthread_mutex_unlock(&g_offer_mutex);
}

bool msg_processor_get_offer(struct snapshot_offer *offer)
{
    if (!offer)
        return false;

    pthread_mutex_lock(&g_offer_mutex);
    bool ok = atomic_load(&g_cached_offer_valid);
    if (ok)
        *offer = g_cached_offer;
    else
        memset(offer, 0, sizeof(*offer));
    pthread_mutex_unlock(&g_offer_mutex);
    return ok;
}

void msg_processor_invalidate_offer(void)
{
    pthread_mutex_lock(&g_offer_mutex);
    memset(&g_cached_offer, 0, sizeof(g_cached_offer));
    atomic_store(&g_cached_offer_valid, false);
    g_cached_offer_version++;
    pthread_mutex_unlock(&g_offer_mutex);
}

uint64_t msg_processor_offer_cache_version(void)
{
    pthread_mutex_lock(&g_offer_mutex);
    uint64_t version = g_cached_offer_version;
    pthread_mutex_unlock(&g_offer_mutex);
    return version;
}

bool msg_processor_publish_manifest(struct sync_manifest *manifest)
{
    if (!msg_manifest_is_reasonable(manifest))
        return false;

    pthread_mutex_lock(&g_manifest_mutex);
    if (atomic_load(&g_cached_manifest_valid))
        msg_manifest_reset(&g_cached_manifest);
    g_cached_manifest = *manifest;
    memset(manifest, 0, sizeof(*manifest));
    atomic_store(&g_cached_manifest_valid, true);
    g_cached_manifest_version++;
    pthread_mutex_unlock(&g_manifest_mutex);
    return true;
}

void msg_processor_invalidate_manifest(void)
{
    pthread_mutex_lock(&g_manifest_mutex);
    if (atomic_load(&g_cached_manifest_valid))
        msg_manifest_reset(&g_cached_manifest);
    atomic_store(&g_cached_manifest_valid, false);
    g_cached_manifest_version++;
    pthread_mutex_unlock(&g_manifest_mutex);
}

uint64_t msg_processor_manifest_cache_version(void)
{
    pthread_mutex_lock(&g_manifest_mutex);
    uint64_t version = g_cached_manifest_version;
    pthread_mutex_unlock(&g_manifest_mutex);
    return version;
}

bool msg_processor_get_manifest_header(struct sync_manifest *out)
{
    if (!out)
        return false;

    pthread_mutex_lock(&g_manifest_mutex);
    bool ok = atomic_load(&g_cached_manifest_valid);
    if (ok) {
        *out = g_cached_manifest;
        out->chunk_hashes = NULL;
    } else {
        memset(out, 0, sizeof(*out));
    }
    pthread_mutex_unlock(&g_manifest_mutex);
    return ok;
}

bool msg_processor_publish_block_manifest(struct block_piece_manifest *manifest,
                                         int32_t built_at_height)
{
    if (!msg_block_manifest_is_reasonable(manifest))
        return false;

    pthread_mutex_lock(&g_block_manifest_mutex);
    if (atomic_load(&g_cached_block_manifest_valid))
        msg_block_manifest_reset(&g_cached_block_manifest);
    g_cached_block_manifest = *manifest;
    memset(manifest, 0, sizeof(*manifest));
    g_manifest_built_at_height = built_at_height;
    atomic_store(&g_cached_block_manifest_valid, true);
    g_cached_block_manifest_version++;
    pthread_mutex_unlock(&g_block_manifest_mutex);
    return true;
}

void msg_processor_invalidate_block_manifest(void)
{
    pthread_mutex_lock(&g_block_manifest_mutex);
    if (atomic_load(&g_cached_block_manifest_valid))
        msg_block_manifest_reset(&g_cached_block_manifest);
    g_manifest_built_at_height = 0;
    atomic_store(&g_cached_block_manifest_valid, false);
    g_cached_block_manifest_version++;
    pthread_mutex_unlock(&g_block_manifest_mutex);
}

uint64_t msg_processor_block_manifest_cache_version(void)
{
    pthread_mutex_lock(&g_block_manifest_mutex);
    uint64_t version = g_cached_block_manifest_version;
    pthread_mutex_unlock(&g_block_manifest_mutex);
    return version;
}

bool msg_processor_get_block_manifest_header(struct block_piece_manifest *out,
                                            int32_t *built_at_height)
{
    if (!out)
        return false;

    pthread_mutex_lock(&g_block_manifest_mutex);
    bool ok = atomic_load(&g_cached_block_manifest_valid);
    if (ok) {
        *out = g_cached_block_manifest;
        out->piece_hashes = NULL;
        if (built_at_height)
            *built_at_height = g_manifest_built_at_height;
    } else {
        memset(out, 0, sizeof(*out));
        if (built_at_height)
            *built_at_height = 0;
    }
    pthread_mutex_unlock(&g_block_manifest_mutex);
    return ok;
}

/* Serialize and send a snapshot offer to a peer.
 * Wire: height(4) + block_hash(32) + utxo_root(32) + mmr_root(32) +
 *       num_utxos(8) + total_bytes(8) + mmb_root(32) = 148 bytes
 * Older ZCL23 nodes read 116 bytes and ignore the trailing mmb_root. */
static void send_snapshot_offer_msg(struct p2p_node *node,
                                     const struct snapshot_offer *offer,
                                     const unsigned char *msg_start)
{
    uint64_t offer_version = msg_processor_offer_cache_version();
    uint64_t snapshot_version = fast_sync_snapshot_cache_version();

    p2p_node_begin_message(node, MSG_SNAPSHOT_OFFER, msg_start);
    struct byte_stream os;
    stream_init(&os, 152);
    stream_write_i32_le(&os, offer->height);
    stream_write_bytes(&os, offer->block_hash, 32);
    stream_write_bytes(&os, offer->utxo_root, 32);
    stream_write_bytes(&os, offer->mmr_root, 32);
    stream_write_u64_le(&os, offer->num_utxos);
    stream_write_u64_le(&os, offer->total_bytes);
    stream_write_bytes(&os, offer->mmb_root, 32); /* appended: backward compat */
    p2p_node_write_message_data(node, os.data, os.size);
    p2p_node_end_message(node);
    stream_free(&os);

    memcpy(node->zsync_offered_root, offer->utxo_root, 32);
    memcpy(node->zsync_offered_mmr, offer->mmr_root, 32);
    memcpy(node->zsync_offered_block, offer->block_hash, 32);
    node->zsync_offered_height = offer->height;
    node->zsync_offered_count = offer->num_utxos;
    node->zsync_offer_version = offer_version;
    node->zsync_snapshot_version = snapshot_version;
}

/* Take a thread-safe local copy of the cached offer */
/* Ring buffers for duplicate detection of recently seen blocks and txs. */
#define MAX_RECENT_BLOCKS 128
#define MAX_RECENT_TXS 4096
static struct uint256 g_recent_blocks[MAX_RECENT_BLOCKS];
static _Atomic int g_recent_block_count = 0;
static struct uint256 g_recent_txs[MAX_RECENT_TXS];
static _Atomic int g_recent_tx_count = 0;

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
bool msgprocessor_test_accept_block_for_processing(const struct uint256 *hash,
                                                   bool snapshot_active) {
    if (!hash)
        return false;
    if (snapshot_active)
        return false;
    if (block_already_seen(hash))
        return false;
    block_mark_seen(hash);
    return true;
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
                         struct net_manager *net_mgr,
                         const struct app_runtime_context *runtime)
{
    mp->main_state = ms;
    mp->mempool = mempool;
    mp->coins_tip = coins_tip;
    mp->params = params;
    mp->datadir = datadir;
    mp->net_mgr = net_mgr;
    mp->runtime = runtime;

    /* Initialize download manager once (before threads start) */
    msg_get_download_mgr();

    /* Build initial block piece manifest for swarm sync.
     * This enables serving block pieces to peers immediately. */
    if (ms && datadir) {
        int tip = active_chain_height(&ms->chain_active);
        struct block_piece_manifest header;
        if (tip > 1000 &&
            !msg_processor_get_block_manifest_header(&header, NULL)) {
            struct block_piece_manifest manifest;
            memset(&manifest, 0, sizeof(manifest));
            if (block_piece_manifest_build(datadir, 1, tip, &manifest)) {
                uint32_t num_pieces = manifest.num_pieces;
                msg_processor_publish_block_manifest(&manifest, tip);
                printf("Block manifest built: h=1..%d (%u pieces, SHA3 verified)\n",
                       tip, num_pieces);
            }
        }
    }
}

int msg_get_height(void *ctx)
{
    struct msg_processor *mp = (struct msg_processor *)ctx;
    return active_chain_height(&mp->main_state->chain_active);
}

/* Send our manifest to a ZCL23 peer. Called after version/verack handshake. */
static void push_manifest(struct msg_processor *mp, struct p2p_node *node)
{
    struct sync_manifest m;

    if (node->swarm_manifest_sent ||
        !msg_processor_get_manifest_header(&m))
        return;
    struct byte_stream s;
    stream_init(&s, 80);
    stream_write_i32_le(&s, m.height);
    stream_write_bytes(&s, m.block_hash, 32);
    stream_write_u64_le(&s, m.num_utxos);
    stream_write_u32_le(&s, m.num_chunks);
    stream_write_u32_le(&s, m.chunk_size);
    stream_write_bytes(&s, m.merkle_root, 32);

    p2p_node_begin_message(node, MSG_MANIFEST, mp->params->pchMessageStart);
    p2p_node_write_message_data(node, s.data, s.size);
    p2p_node_end_message(node);
    stream_free(&s);

    node->swarm_manifest_sent = true;
    printf("Peer %s: sent manifest (h=%d, %u chunks)\n",
           node->addr_name, m.height, m.num_chunks);
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

/* Send our block piece manifest to a ZCL23 peer.
 * SAFETY: never call this for legacy peers — they will ignore it,
 * but we avoid sending unknown messages to be a good network citizen. */
static void push_block_manifest(struct msg_processor *mp,
                                 struct p2p_node *node)
{
    struct block_piece_manifest m;

    if (node->blk_manifest_sent ||
        !msg_processor_get_block_manifest_header(&m, NULL))
        return;
    if (!peer_supports_fast_sync(node->services))
        return; /* guard: only send to ZCL23 peers */
    struct byte_stream s;
    stream_init(&s, 80);
    stream_write_i32_le(&s, m.start_height);
    stream_write_i32_le(&s, m.end_height);
    stream_write_u32_le(&s, m.num_pieces);
    stream_write_bytes(&s, m.tip_hash, 32);
    stream_write_bytes(&s, m.merkle_root, 32);

    p2p_node_begin_message(node, MSG_BLOCK_MANIFEST,
                            mp->params->pchMessageStart);
    p2p_node_write_message_data(node, s.data, s.size);
    p2p_node_end_message(node);
    stream_free(&s);

    node->blk_manifest_sent = true;
    printf("Peer %s: sent block manifest (h=%d..%d, %u pieces)\n",
           node->addr_name, m.start_height, m.end_height, m.num_pieces);
}

/* Send a block piece request to a peer. */
static void push_block_piece_request(struct msg_processor *mp,
                                      struct p2p_node *node,
                                      uint32_t piece_index)
{
    struct byte_stream s;
    stream_init(&s, 4);
    stream_write_u32_le(&s, piece_index);

    p2p_node_begin_message(node, MSG_BLOCK_REQ,
                            mp->params->pchMessageStart);
    p2p_node_write_message_data(node, s.data, s.size);
    p2p_node_end_message(node);
    stream_free(&s);
}

/* push_block_bitmap removed — receiver handler for MSG_BLOCK_BITMAP
 * still active (line ~2286). Sender will be added when block swarm
 * bitmap exchange is fully integrated. */

static void push_version(struct msg_processor *mp, struct p2p_node *node)
{
    struct version_message ver;
    version_message_init(&ver);
    ver.protocol_version = PROTOCOL_VERSION;
    /* Match zclassicd services exactly: NODE_NETWORK | NODE_BLOOM */
    ver.services = NODE_NETWORK | NODE_BLOOM;
    ver.timestamp = (int64_t)time(NULL);
    ver.addr_recv = node->addr;
    ver.nonce = mp->net_mgr->local_host_nonce;
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
        event_emitf(EV_PEER_MISBEHAVE, (uint32_t)node->id,
                    "duplicate version from %s", node->addr_name);
        return false;
    }

    struct version_message ver;
    version_message_init(&ver);
    if (!version_message_deserialize(&ver, s))
        return false;

    if (ver.protocol_version < MIN_PEER_PROTO_VERSION) {
        event_emitf(EV_PEER_MISBEHAVE, (uint32_t)node->id,
                    "proto %d too old (min %d) %s",
                    ver.protocol_version, MIN_PEER_PROTO_VERSION,
                    node->addr_name);
        node->disconnect = true;
        return false;
    }

    /* Self-connection detection: if the peer's nonce matches our own
     * local_host_nonce, we are connecting to ourselves. Disconnect. */
    if (ver.nonce == mp->net_mgr->local_host_nonce &&
        mp->net_mgr->local_host_nonce != 0) {
        event_emitf(EV_TCP_DISCONNECTED, (uint32_t)node->id,
                    "self-connection %s", node->addr_name);
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

    /* Ignore duplicate version messages from peers already past handshake */
    if (node->state >= PEER_HANDSHAKE_COMPLETE) {
        printf("Peer %s: ignoring duplicate version (already %s)\n",
               node->addr_name, peer_state_name(node->state));
        return true;
    }
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

    event_emitf(EV_PEER_VERSION, (uint32_t)node->id,
                "%s v=%d h=%d %s%s",
                node->addr_name, node->version, node->starting_height,
                node->sub_ver,
                peer_supports_fast_sync(node->services) ? " [ZCL23]" : "");

    /* Detect zclassic23 peers via subversion string.
     * Service bit detection is secondary — some peers filter unknown bits. */
    bool is_zcl23 = peer_supports_fast_sync(node->services) ||
                    strstr(node->sub_ver, "ZClassic-C23") != NULL;
    if (is_zcl23) {
        node->services |= NODE_ZCL23; /* mark for fast sync */
        node->swarm_inflight_chunk = -1;
        for (int pi = 0; pi < 4; pi++)
            node->blk_pipeline[pi].piece_index = -1;
        printf("Peer %s: supports zclassic23 fast sync [ZCL23]\n",
               node->addr_name);

        /* Exchange UTXO manifests — both peers announce what they have. */
        push_manifest(mp, node);

        /* Exchange block piece manifests for parallel block sync. */
        push_block_manifest(mp, node);

        /* Advertise file service port. The peer knows our IP from the
         * TCP connection — we just tell them which port to connect to
         * for the fast file service. They cache it in SQLite for
         * sticky reconnection across restarts.
         * Message: "zfileaddr" with [2-byte port]. */
        {
            uint8_t faddr[2];
            uint16_t fport = fs_server_get_port();
            memcpy(faddr, &fport, 2);

            struct byte_stream fs_msg;
            stream_init(&fs_msg, 4);
            stream_write_bytes(&fs_msg, faddr, 2);
            p2p_node_begin_message(node, "zfileaddr",
                                    mp->params->pchMessageStart);
            p2p_node_write_message_data(node, fs_msg.data, fs_msg.size);
            p2p_node_end_message(node);
            stream_free(&fs_msg);
        }
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

    /* Aggressive peer exchange with ZCL23 nodes — don't wait for getaddr.
     * Push all known addresses immediately so both nodes build their
     * address books fast. This is the key to low-friction peer discovery:
     * every ZCL23 handshake floods addresses in both directions. */
    if (peer_supports_fast_sync(node->services) && mp->net_mgr) {
        struct net_address addrs[2500];
        size_t num = addrman_get_addr(&mp->net_mgr->addrman, addrs, 2500);
        if (num > 0) {
            struct byte_stream addr_msg;
            stream_init(&addr_msg, num * 30 + 8);
            stream_write_compact_size(&addr_msg, num);
            for (size_t i = 0; i < num; i++)
                net_address_serialize(&addrs[i], &addr_msg, true);
            p2p_node_begin_message(node, "addr",
                                    mp->params->pchMessageStart);
            p2p_node_write_message_data(node, addr_msg.data, addr_msg.size);
            p2p_node_end_message(node);
            stream_free(&addr_msg);
            printf("Peer %s: pushed %zu addresses (ZCL23 peer exchange)\n",
                   node->addr_name, num);
        }
    }

    /* Save peer via ActiveRecord model */
    struct node_db *ndb = msg_node_db(mp);
    if (ndb && ndb->open) {
        struct db_peer peer;
        memset(&peer, 0, sizeof(peer));
        memcpy(peer.ip, node->addr.svc.addr.ip, 16);
        peer.port = node->addr.svc.port;
        peer.services = node->services;
        peer.last_seen = (int64_t)time(NULL);
        peer.is_zcl23 = peer_supports_fast_sync(node->services);
        db_peer_save(ndb, &peer);
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
        event_emitf(EV_PEER_MISBEHAVE, (uint32_t)node->id,
                    "inv too large (%llu) from %s",
                    (unsigned long long)count, node->addr_name);
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
            if (block_already_seen(&inv.hash))
                continue;
            /* Don't request blocks during snapshot sync */
            if (snapsync_is_active())
                continue;
            struct block_index *bi = block_map_find(
                &mp->main_state->map_block_index, &inv.hash);
            struct block_index *tip = active_chain_tip(
                &mp->main_state->chain_active);
            /* Only request blocks via inv if we're close to the tip;
             * during IBD, rely on headers-first sync. */
            bool in_ibd = !tip || (node->starting_height > 0 &&
                          tip->nHeight < node->starting_height - 1000);
            bool need_data = !bi || !(bi->nStatus & BLOCK_HAVE_DATA);
            if (need_data && !in_ibd) {
                inv_item_serialize(&inv, &getdata);
                request_count++;
            } else if (need_data && in_ibd) {
                /* Ask for headers instead */
                push_getheaders_from(mp, node, tip);
            }
            if (tip && bi && active_chain_contains(
                    &mp->main_state->chain_active, bi)) {
                node->hash_continue = inv.hash;
            }
        } else if (inv.type == MSG_TX) {
            if (tx_already_seen(&inv.hash))
                continue;
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
                    /* Verify block hash before serving — never send
                     * corrupted data that would get us banned */
                    struct uint256 disk_hash;
                    block_get_hash(&blk, &disk_hash);
                    if (uint256_cmp(&disk_hash, &inv.hash) != 0) {
                        char exp[65], got[65];
                        uint256_get_hex(&inv.hash, exp);
                        uint256_get_hex(&disk_hash, got);
                        fprintf(stderr, "SAFETY: refusing to serve block "
                                "h=%d — hash mismatch!\n"
                                "  requested: %s\n  disk:      %s\n",
                                bi->nHeight, exp, got);
                        block_free(&blk);
                        goto skip_block_serve;
                    }

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
            skip_block_serve:
            (void)0;
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
    /* Pre-check: reject oversized block messages before deserialization.
     * Prevents allocation DoS from crafted messages. */
    if (s->size > 2000000) {
        event_emitf(EV_PEER_MISBEHAVE, (uint32_t)node->id,
                    "oversized block msg %zu bytes", s->size);
        peer_misbehaving(mp->net_mgr, node, 100, "oversized block");
        return false;
    }

    struct block blk;
    block_init(&blk);
    if (!block_deserialize(&blk, s)) {
        event_emitf(EV_MSG_DESERIALIZATION_FAIL, (uint32_t)node->id, "block");
        peer_misbehaving(mp->net_mgr, node, 20, "malformed block");
        block_free(&blk);
        return false;
    }

    struct uint256 hash;
    block_get_hash(&blk, &hash);

    /* Mark received in download manager (removes from in-flight) */
    struct download_manager *dm = get_download_mgr();
    dl_mark_received(dm, &hash);

    /* Track block bytes for MB/s throughput reporting */
    dl_add_bytes_received(dm, s->size);

    /* Defer block processing while snapshot sync is active (any state).
     * During NEGOTIATING: blocks fail at height 0, accumulate dos points.
     * During RECEIVING: starves P2P socket reads.
     * During VERIFYING: SHA3 computation needs uncontested SQLite. */
    if (snapsync_is_active()) {
        block_free(&blk);
        return true;
    }

    if (block_already_seen(&hash)) {
        block_free(&blk);
        return true;
    }
    block_mark_seen(&hash);

    struct validation_state state;
    validation_state_init(&state);
    process_new_block(&state, mp->main_state, mp->coins_tip,
                      mp->params, &blk, false, mp->datadir);

    if (!validation_state_is_valid(&state)) {
        char hex[65];
        uint256_get_hex(&hash, hex);
        event_emitf(EV_BLOCK_REJECTED, (uint32_t)node->id,
                    "hash=%s reason=%s", hex,
                    state.reject_reason[0] ? state.reject_reason : "unknown");

        /* When a block fails validation during IBD (likely a fork block),
         * re-request headers from this peer starting at our current tip.
         * This forces the peer to send us the correct chain of headers,
         * which will include the valid block at the failed height. */
        {
            struct sync_getheaders_action action = {0};
            syncsvc_plan_invalid_block_getheaders(&action, sync_get_state());
            exec_getheaders_action(mp, node, &action);
        }
    }

    if (validation_state_is_valid(&state)) {
        struct block_index *new_tip = active_chain_tip(
            &mp->main_state->chain_active);
        if (new_tip) {
            struct sync_block_acceptance acceptance;
            node->last_block_time = (int64_t)time(NULL);
            node->blocks_received++;
            syncsvc_note_valid_block(&acceptance, node, sync_get_state(),
                                     new_tip->nHeight,
                                     mp->main_state->pindex_best_header
                                         ? mp->main_state->pindex_best_header->nHeight
                                         : new_tip->nHeight);
            event_emitf(EV_BLOCK_CONNECTED, (uint32_t)node->id,
                        "h=%d", new_tip->nHeight);

            if (acceptance.reached_peer_tip) {
                if (acceptance.should_set_sync_state) {
                    sync_set_state(acceptance.next_sync_state,
                                   "caught up to peer");
                }
                if (acceptance.should_set_flush_policy)
                    set_flush_policy(3600, 500000, 100);
                if (acceptance.should_update_peer_state) {
                    peer_set_state_checked((uint32_t)node->id, &node->state,
                                           acceptance.next_peer_state,
                                           "chain caught up");
                }
                /* Start deferred HTTPS server now that it's safe */
                extern void https_deferred_check(void);
                https_deferred_check();
                if (acceptance.should_emit_tip_updated)
                    event_emitf(EV_TIP_UPDATED, 0,
                                "AT_TIP height=%d",
                                new_tip->nHeight);
            }

            /* Progress logged by IBD progress timer (every 30s) */

            /* Refresh block manifest when chain grows beyond cached range.
             * Only at tip — during IBD, SQLite is still catching up and
             * manifest build can crash on partial data. We're a client
             * during IBD, not serving pieces to peers. */
            bool should_refresh_manifest = false;
            if (sync_get_state() == SYNC_AT_TIP && new_tip->nHeight > 1000) {
                struct block_piece_manifest header;
                int32_t built_at = 0;
                bool has_manifest =
                    msg_processor_get_block_manifest_header(&header,
                                                            &built_at);
                should_refresh_manifest =
                    !has_manifest ||
                    new_tip->nHeight - built_at >= MANIFEST_REFRESH_BLOCKS;
            }
            if (should_refresh_manifest) {
                /* Rebuild in a detached thread to avoid blocking message processing */
                static _Atomic bool g_manifest_rebuilding = false;
                if (!atomic_exchange(&g_manifest_rebuilding, true)) {
                    struct block_piece_manifest new_m;
                    memset(&new_m, 0, sizeof(new_m));
                    if (block_piece_manifest_build(mp->datadir, 1,
                            new_tip->nHeight, &new_m)) {
                        uint32_t num_pieces = new_m.num_pieces;
                        msg_processor_publish_block_manifest(
                            &new_m, new_tip->nHeight);
                        event_emitf(EV_SYNC_STATE_CHANGE, 0, "manifest refreshed to h=%d (%u pieces)",
                                    new_tip->nHeight, num_pieces);
                    }
                    atomic_store(&g_manifest_rebuilding, false);
                }
            }

            /* Relay accepted block to all connected peers (not during IBD).
             * At the tip, we act as a full relay node. During IBD, relaying
             * would flood peers with old blocks they already have. */
            if (sync_get_state() == SYNC_AT_TIP && new_tip->phashBlock) {
                struct inv_item blk_inv;
                inv_item_init_typed(&blk_inv, MSG_BLOCK, new_tip->phashBlock);
                /* Push to all peers except the one who sent it */
                if (mp->net_mgr) {
                    zcl_mutex_lock(&mp->net_mgr->cs_nodes);
                    for (size_t pi = 0; pi < mp->net_mgr->num_nodes; pi++) {
                        struct p2p_node *peer = mp->net_mgr->nodes[pi];
                        if (peer->id != node->id &&
                            peer->state >= PEER_HANDSHAKE_COMPLETE &&
                            !peer->disconnect)
                            p2p_node_push_inventory(peer, &blk_inv);
                    }
                    zcl_mutex_unlock(&mp->net_mgr->cs_nodes);
                }
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
            /* DoS=0 but invalid: orphan block or parent-failed.
             * Don't penalize peer — this is normal during sync. */
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
        event_emitf(EV_MSG_DESERIALIZATION_FAIL, (uint32_t)node->id, "tx");
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

        /* Relay accepted tx to all connected peers */
        if (mp->net_mgr) {
            zcl_mutex_lock(&mp->net_mgr->cs_nodes);
            for (size_t pi = 0; pi < mp->net_mgr->num_nodes; pi++) {
                struct p2p_node *peer = mp->net_mgr->nodes[pi];
                if (peer->id != node->id &&
                    peer->state >= PEER_HANDSHAKE_COMPLETE &&
                    !peer->disconnect && peer->relay_txes)
                    p2p_node_push_inventory(peer, &inv);
            }
            zcl_mutex_unlock(&mp->net_mgr->cs_nodes);
        }

        {
            struct wallet *wallet = msg_wallet(mp);
            if (wallet) {
                wallet_sync_transaction(wallet, &tx, NULL);
                {
                    struct node_db *ndb = msg_node_db(mp);
                    if (ndb)
                        node_db_sync_wallet_tx(ndb, &tx, wallet, 0);
                }
            }
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
    /* Defer header processing during any snapshot sync state — header parsing
     * and block index updates consume CPU and starve P2P reads.
     * During NEGOTIATING: headers trigger getblocks which compete. */
    if (snapsync_is_active())
        return true;

    uint64_t count;
    if (!stream_read_compact_size(s, &count))
        return false;

    if (count > 2000) {
        event_emitf(EV_PEER_MISBEHAVE, (uint32_t)node->id,
                    "headers count %llu exceeds 2000 from %s",
                    (unsigned long long)count, node->addr_name);
        peer_misbehaving(mp->net_mgr, node, 20, "too many headers");
        node->disconnect = true;
        return false;
    }

    struct uint256 last_hash;
    uint256_set_null(&last_hash);
    struct block_index *pindex_last = NULL;
    struct sync_header_processing_plan header_plan = {0};
    size_t accepted = 0;

    for (uint64_t i = 0; i < count; i++) {
        struct block_header hdr;
        block_header_init(&hdr);
        if (!block_header_deserialize(&hdr, s)) {
            event_emitf(EV_HEADERS_REJECTED, (uint32_t)node->id,
                        "malformed header[%llu] from %s",
                        (unsigned long long)i, node->addr_name);
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
            event_emitf(EV_HEADERS_REJECTED, (uint32_t)node->id,
                        "header[%llu] %s reason=%s",
                        (unsigned long long)i, hex,
                        state.reject_reason[0] ? state.reject_reason
                                               : "unknown");
        }
    }

    {
        struct block_index *tip = active_chain_tip(&mp->main_state->chain_active);
        int our_height = tip ? tip->nHeight : 0;
        struct block_index *bi = block_map_find(
            &mp->main_state->map_block_index, &last_hash);
        size_t max_collect = 512;
        struct uint256 *hashes = malloc(max_collect * sizeof(struct uint256));
        int32_t *heights = malloc(max_collect * sizeof(int32_t));

        if (!hashes || !heights) {
            fprintf(stderr, "msgprocessor: malloc failed for block request "
                    "arrays (%zu entries)\n", max_collect);
            free(hashes); free(heights);
            hashes = NULL; heights = NULL;
        }

        syncsvc_plan_header_processing(&header_plan, accepted, count,
                                       pindex_last, sync_get_state(),
                                       bi, tip, our_height,
                                       hashes, heights, max_collect);

        if (header_plan.batch.should_warn_all_rejected) {
            /* All headers rejected — this stalls sync. Log prominently. */
            event_emitf(EV_HEADERS_REJECTED, (uint32_t)node->id,
                        "all %llu headers rejected", (unsigned long long)count);
            printf("WARNING: Peer %s: all %llu headers rejected — sync stalled!\n",
                   node->addr_name, (unsigned long long)count);
        }

        if (header_plan.batch.should_emit_received) {
            event_emitf(EV_HEADERS_RECEIVED, (uint32_t)node->id,
                        "accepted=%zu total=%llu tip=%d",
                        accepted, (unsigned long long)count,
                        pindex_last ? pindex_last->nHeight : -1);

            if (syncsvc_should_log_accepted_headers(node, pindex_last))
                printf("Peer %s: accepted %zu/%llu headers "
                       "(header tip=%d, chain tip=%d, peer=%d)\n",
                       node->addr_name, accepted, (unsigned long long)count,
                       pindex_last ? pindex_last->nHeight : -1,
                       active_chain_height(&mp->main_state->chain_active),
                       node->starting_height);
        }

        /* One-shot block file scan: if block files exist on disk (from
         * file_service) but weren't scanned at boot (empty index at boot),
         * scan them now that we have headers. This marks downloaded blocks
         * as BLOCK_HAVE_DATA so we don't re-download them from P2P. */
        if (header_plan.should_scan_block_files) {
            char bfp[576];
            snprintf(bfp, sizeof(bfp), "%s/blocks/blk00000.dat", mp->datadir);
            struct stat bfst;
            if (stat(bfp, &bfst) == 0 && bfst.st_size > 0) {
                printf("P2P trigger: scanning block files for HAVE_DATA...\n");
                int scan_m = scan_block_files_mark_data(
                    mp->main_state, mp->datadir, mp->params);
                struct sync_chain_activation activation = {0};
                syncsvc_build_block_file_scan_activation(&activation, scan_m);
                if (activation.should_activate) {
                    printf("P2P block file scan: %d blocks marked\n", scan_m);
                    struct validation_state vs;
                    validation_state_init(&vs);
                    activate_best_chain(&vs, mp->main_state, mp->coins_tip,
                                        mp->params, NULL, mp->datadir);
                }
            }
        }
        if (header_plan.should_queue_needed_blocks) {
            if (header_plan.should_set_sync_state)
                sync_set_state(header_plan.next_sync_state,
                               "headers ahead, requesting blocks");
            if (!header_plan.download.needed_blocks.chains_from_tip)
                printf("headers: skip block queue — chain doesn't reach "
                       "tip h=%d\n", our_height);

            {
                struct download_manager *dm = get_download_mgr();
                size_t queued = dl_queue_blocks(dm, hashes, heights,
                                                header_plan.queue_count);
                if (queued > 0)
                    event_emitf(EV_BLOCK_REQUESTED, (uint32_t)node->id,
                                "queued=%zu total_needed=%zu",
                                queued, header_plan.queue_count);
            }

            {
                struct sync_chain_activation activation = {0};
                syncsvc_build_header_processing_activation(&activation,
                                                          &header_plan);
                if (activation.should_activate) {
                /* All blocks already have data — trigger chain activation.
                 * This happens after restart: blocks on disk from before,
                 * headers re-received, but activate_best_chain only runs
                 * on block receipt. Without this, the node stalls with
                 * headers ahead but no block requests (all HAVE_DATA). */
                struct validation_state vs;
                validation_state_init(&vs);
                activate_best_chain(&vs, mp->main_state, mp->coins_tip,
                                    mp->params, NULL, mp->datadir);
                }
            }
        }
        free(hashes);
        free(heights);
    }

    /* Request more headers if we accepted any */
    if (header_plan.batch.should_request_more_headers)
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
    uint64_t reason_len = 0;
    char reason[256] = {0};
    if (stream_read_compact_size(s, &reason_len) && reason_len > 0 &&
        reason_len < sizeof(reason))
        stream_read_bytes(s, (unsigned char *)reason, reason_len);
    printf("Peer %s: reject %s (code=%d) %s\n",
           node->addr_name, msg_type, code, reason);
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
    uint64_t count;
    if (!stream_read_compact_size(s, &count))
        return false;

    struct download_manager *dm = get_download_mgr();
    for (uint64_t i = 0; i < count; i++) {
        struct inv_item inv;
        if (!inv_item_deserialize(&inv, s))
            return false;
        if (inv.type == MSG_BLOCK) {
            char hex[65];
            uint256_get_hex(&inv.hash, hex);
            printf("Peer %s: notfound block %s\n", node->addr_name, hex);
            /* Re-queue so another peer can try */
            dl_peer_disconnected(dm, (uint32_t)node->id);
        }
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
                        "%s size=%u exp=%08x got=%08x",
                        ccmd, msg.hdr.nMessageSize,
                        expected, msg.hdr.nChecksum);
            fprintf(stderr, "Peer %s: checksum mismatch on '%s' (size=%u exp=%08x got=%08x)\n",
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
        } else if (strcmp(cmd, "zfileaddr") == 0) {
            /* Peer advertises their file service port.
             * We know their IP from the TCP connection.
             * Save to SQLite for sticky reconnection. */
            uint8_t faddr[2];
            if (stream_read_bytes(&s, faddr, 2)) {
                uint16_t fport;
                memcpy(&fport, faddr, 2);
                /* Use peer's IP from the P2P connection */
                uint8_t fip[16];
                memcpy(fip, node->addr.svc.addr.ip, 16);

                struct node_db *ndb = msg_node_db(mp);
                if (ndb && ndb->open) {
                    struct db_file_service fs;
                    memset(&fs, 0, sizeof(fs));
                    memcpy(fs.ip, fip, 16);
                    fs.port = fport;
                    fs.p2p_port = node->addr.svc.port;
                    fs.last_seen = (int64_t)time(NULL);
                    fs.is_zcl23 = true;
                    db_file_service_save(ndb, &fs);
                }
                char ipbuf[64];
                net_addr_to_string(&node->addr.svc.addr, ipbuf, sizeof(ipbuf));
                printf("Peer %s: file service at port %d (saved)\n",
                       ipbuf, fport);
            }
        } else if (strcmp(cmd, MSG_SNAPSHOT_OFFER) == 0) {
            /* ── Route: zsnapshot → snapsync_handle_offer ──────── */
            struct snapshot_offer_params params;
            if (snapsync_parse_offer_params(&params, &s)) {
                params.peer_id = (uint32_t)node->id;
                params.our_height = active_chain_height(
                    &mp->main_state->chain_active);

                /* Additional gate: must not already be receiving, must
                 * not be at tip, peer state must be compatible */
                if (node->state == PEER_SNAPSHOT_RECEIVING ||
                    sync_get_state() == SYNC_AT_TIP) {
                    /* silently ignore */
                } else {
                    struct snapshot_sync_service *svc =
                        msg_snapshot_sync_ensure(mp);
                    if (svc) {
                        enum snapsync_offer_result result =
                            snapsync_handle_offer(svc, &params);

                        switch (result) {
                        case SNAPSYNC_OFFER_ACCEPTED: {
                            struct snapsync_offer_acceptance accepted = {0};
                            snapsync_build_offer_acceptance(&accepted);
                            if (accepted.should_store_offer_details) {
                                memcpy(node->zsync_offered_root, params.utxo_root, 32);
                                memcpy(node->zsync_offered_mmr, params.mmr_root, 32);
                                memcpy(node->zsync_offered_block, params.block_hash, 32);
                                node->zsync_offered_height = params.height;
                            }
                            if (accepted.should_reset_offset)
                                node->zsync_offset = 0;
                            if (accepted.should_update_peer_state)
                                peer_set_state_checked((uint32_t)node->id, &node->state,
                                    accepted.peer_state, "accepted snapshot offer");
                            event_emitf(EV_SNAPSHOT_OFFER_RECEIVED, (uint32_t)node->id,
                                "h=%d utxos=%llu", params.height,
                                (unsigned long long)params.num_utxos);
                            if (accepted.should_set_sync_state)
                                sync_set_state(accepted.sync_state, "peer snapshot");

                            struct snapsync_offer_followup followup = {0};
                            snapsync_build_offer_followup(&followup, svc);
                            if (followup.action ==
                                SNAPSYNC_FOLLOWUP_SEND_FC_CHALLENGE) {
                                /* Send FlyClient challenge — verify chain
                                 * before requesting snapshot data */
                                p2p_node_begin_message(node, MSG_FC_CHALLENGE,
                                    mp->params->pchMessageStart);
                                struct byte_stream fc;
                                stream_init(&fc, 72);
                                snapsync_write_fc_challenge(svc, &fc);
                                p2p_node_write_message_data(node, fc.data, fc.size);
                                p2p_node_end_message(node);
                                stream_free(&fc);
                                printf("[snapsync] Sent FlyClient challenge to %s\n",
                                       node->addr_name);
                            } else if (followup.action ==
                                       SNAPSYNC_FOLLOWUP_SEND_SNAPSHOT_REQ) {
                                /* No MMB — send zsnapreq directly */
                                struct byte_stream rq;
                                stream_init(&rq, 52);
                                if (snapsync_write_snapshot_request(
                                        &rq, params.our_height,
                                        node->addr.svc.addr.ip)) {
                                    p2p_node_begin_message(node, MSG_SNAPSHOT_REQ,
                                        mp->params->pchMessageStart);
                                    p2p_node_write_message_data(node, rq.data, rq.size);
                                    p2p_node_end_message(node);
                                }
                                stream_free(&rq);
                            }
                            break;
                        }
                        case SNAPSYNC_OFFER_REJECTED_RANGE:
                            peer_misbehaving(mp->net_mgr, node, 20,
                                "snapshot offer out of range");
                            break;
                        case SNAPSYNC_OFFER_REJECTED_NO_MMR:
                            peer_misbehaving(mp->net_mgr, node, 10,
                                "snapshot without MMR proof");
                            break;
                        default:
                            break; /* not ahead or busy — ignore silently */
                        }
                    }
                }
            }

        } else if (strcmp(cmd, MSG_SNAPSHOT_REQ) == 0) {
            /* ── Route: zsnapreq → snapsync_validate_serve_request ─ */
            int32_t from_h = 0;
            if (!stream_read_i32_le(&s, &from_h)) {
                peer_misbehaving(mp->net_mgr, node, 10, "truncated zsnapreq");
                net_message_free(&msg);
                continue;
            }

            /* Pass remaining bytes (PoW data) to controller */
            size_t pow_len = s.size - s.read_pos;
            const uint8_t *pow_data = pow_len > 0 ? s.data + s.read_pos : NULL;
            enum snapsync_serve_result srv = snapsync_validate_serve_request(
                pow_data, pow_len, node->addr.svc.addr.ip);

            switch (srv) {
            case SNAPSYNC_SERVE_OK:
                {
                    struct snapshot_offer offer;
                    if (msg_processor_get_offer(&offer)) {
                    uint64_t current_offer_version =
                        msg_processor_offer_cache_version();
                    uint64_t current_snapshot_version =
                        fast_sync_snapshot_cache_version();
                    bool stale_offer =
                        node->zsync_offered_height <= 0 ||
                        node->zsync_offered_count == 0 ||
                        node->zsync_offer_version != current_offer_version ||
                        node->zsync_snapshot_version != current_snapshot_version ||
                        node->zsync_offered_height != offer.height ||
                        node->zsync_offered_count != offer.num_utxos ||
                        memcmp(node->zsync_offered_root, offer.utxo_root, 32) != 0 ||
                        memcmp(node->zsync_offered_block, offer.block_hash, 32) != 0;
                    if (stale_offer) {
                        printf("Peer %s: stale snapshot request "
                               "(offered h=%d/%llu offer_v=%llu snap_v=%llu, "
                               "current h=%d/%llu offer_v=%llu snap_v=%llu); "
                               "re-offering latest snapshot\n",
                               node->addr_name,
                               node->zsync_offered_height,
                               (unsigned long long)node->zsync_offered_count,
                               (unsigned long long)node->zsync_offer_version,
                               (unsigned long long)node->zsync_snapshot_version,
                               offer.height,
                               (unsigned long long)offer.num_utxos,
                               (unsigned long long)current_offer_version,
                               (unsigned long long)current_snapshot_version);
                        send_snapshot_offer_msg(node, &offer,
                                                mp->params->pchMessageStart);
                        break;
                    }
                    struct snapsync_serve_start serve = {0};
                    snapsync_build_serve_start(&serve, node->zsync_offered_count);
                    if (serve.should_reset_progress) {
                        node->zsync_offset = 0;
                        node->zsync_sent = 0;
                        node->zsync_file_offset = 0;
                        node->zsync_file_size = 0;
                    }
                    if (serve.should_update_peer_state)
                        peer_set_state_checked((uint32_t)node->id, &node->state,
                            serve.peer_state, "serving snapshot request");
                    if (serve.should_reset_cursor) {
                        node->zsync_cursor_valid = false;
                        memset(node->zsync_cursor_txid, 0, 32);
                        node->zsync_cursor_vout = 0;
                    }
                    node->zsync_total = node->zsync_offered_count;
                    printf("Peer %s: serving snapshot (h=%d, %llu UTXOs)\n",
                           node->addr_name, node->zsync_offered_height,
                           (unsigned long long)node->zsync_offered_count);
                    } else {
                        printf("Peer %s: snapshot not ready yet\n",
                               node->addr_name);
                    }
                }
                break;
            case SNAPSYNC_SERVE_BAD_POW:
                peer_misbehaving(mp->net_mgr, node, 20,
                    "zsnapreq without valid PoW");
                break;
            case SNAPSYNC_SERVE_RATE_LIMITED:
                printf("Peer %s: rate limited\n", node->addr_name);
                break;
            default:
                break;
            }

        } else if (strcmp(cmd, MSG_SNAPSHOT_DATA) == 0) {
            /* ── Route: zsnapdata → snapsync_apply_chunk ───────── */
            struct snapshot_sync_service *svc = msg_snapshot_sync_ensure(mp);
            int applied = svc ? snapsync_apply_chunk(svc,
                s.data + s.read_pos, s.size - s.read_pos) : -1;
            if (applied < 0)
                peer_misbehaving(mp->net_mgr, node, 10, "bad snapshot chunk");
            else
                node->zsync_offset += (uint64_t)applied;

        } else if (strcmp(cmd, MSG_SNAPSHOT_END) == 0) {
            /* ── Route: zsnapend → snapsync_handle_end ─────────── */
            struct snapshot_sync_service *svc = msg_snapshot_sync(mp);
            if (!svc) {
                /* nothing to finalize */
            } else {
                struct snapsync_end_result end_result = {0};
                bool verified = snapsync_handle_end(svc,
                                                    (uint32_t)node->id);
                snapsync_build_end_result(&end_result, verified);
                if (end_result.verified) {
                if (end_result.should_update_peer_state) {
                    peer_set_state_checked((uint32_t)node->id, &node->state,
                        end_result.peer_state, "snapshot verified");
                }

                /* Set chain tip to snapshot height */
                if (end_result.should_activate_tip) {
                    int activated_height = snapsync_activate_verified_tip(
                        svc, mp->main_state);
                    if (activated_height >= 0)
                        printf("[snapshot] Chain tip set to height %d\n",
                               activated_height);
                }
                if (end_result.should_set_sync_state) {
                    sync_set_state(end_result.sync_state,
                        "snapshot verified, sync remaining headers");
                }
                } else {
                peer_misbehaving(mp->net_mgr, node, 100,
                    "snapshot SHA3 verification failed");
                }
            }

        /* ── FlyClient chain verification messages ────────────── */

        } else if (strcmp(cmd, MSG_FC_CHALLENGE) == 0) {
            /* ── Route: zfcchallenge → build and send proofs ───── */
            struct fc_challenge challenge;
            memset(&challenge, 0, sizeof(challenge));
            if (stream_read_bytes(&s, challenge.seed, 32) &&
                stream_read_u64_le(&s, &challenge.chain_length) &&
                stream_read_bytes(&s, challenge.mmb_root, 32)) {

                /* Build FlyClient response with real MMB proofs.
                 * Uses mmb_leaf_store (mmap'd flat file of all leaf
                 * hashes) for mmb_prove() + block index for leaf data. */
                struct mmb *mmb = rpc_blockchain_get_mmb();
                extern struct mmb_leaf_store g_mmb_leaf_store;
                const uint8_t (*all_hashes)[32] =
                    mmb_leaf_store_all(&g_mmb_leaf_store);

                if (mmb && mmb->num_leaves > 0 && all_hashes) {
                    /* Build response: leaves from block index,
                     * proofs from mmb_prove() via leaf store */
                    struct fc_response resp;
                    if (snapsync_build_fc_response(
                            &resp, &challenge,
                            &mp->main_state->chain_active,
                            &g_mmb_leaf_store)) {
                        /* Send zfcproofs */
                        p2p_node_begin_message(node, MSG_FC_PROOFS,
                            mp->params->pchMessageStart);
                        struct byte_stream fp;
                        stream_init(&fp, 4 + resp.num_samples * 2048);
                        snapsync_write_fc_response(&fp, &resp);
                        p2p_node_write_message_data(node, fp.data, fp.size);
                        p2p_node_end_message(node);
                        stream_free(&fp);
                        printf("Peer %s: sent %u FlyClient proofs\n",
                               node->addr_name, resp.num_samples);
                    }
                } else {
                    printf("Peer %s: FlyClient challenge but no MMB data\n",
                           node->addr_name);
                }
            }

        } else if (strcmp(cmd, MSG_FC_PROOFS) == 0) {
            /* ── Route: zfcproofs → snapsync_verify_flyclient ──── */
            struct fc_response resp;
            if (!snapsync_parse_fc_response(&resp, &s)) {
                peer_misbehaving(mp->net_mgr, node, 20,
                    "truncated FlyClient proofs");
            } else {
                struct snapsync_verify_result verify_result = {0};
                struct snapshot_sync_service *svc = msg_snapshot_sync(mp);
                if (svc) {
                    snapsync_build_verify_result(
                        &verify_result,
                        snapsync_verify_flyclient(svc, &resp));
                }
                if (verify_result.should_send &&
                    verify_result.action == SNAPSYNC_FOLLOWUP_SEND_SNAPSHOT_REQ) {
                    /* FlyClient passed — now send zsnapreq */
                    int our_h = active_chain_height(
                        &mp->main_state->chain_active);
                    struct byte_stream rq;
                    stream_init(&rq, 52);
                    if (snapsync_write_snapshot_request(
                            &rq, our_h, node->addr.svc.addr.ip)) {
                        p2p_node_begin_message(node, MSG_SNAPSHOT_REQ,
                            mp->params->pchMessageStart);
                        p2p_node_write_message_data(node, rq.data, rq.size);
                        p2p_node_end_message(node);
                    }
                    stream_free(&rq);
                } else {
                    peer_misbehaving(mp->net_mgr, node, 100,
                        "FlyClient chain verification failed");
                }
            }

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

                    zcl_mutex_lock(&g_swarm_mutex);
                    if (swarm_sync_init(&g_swarm, &peer_manifest, mp->datadir)) {
                        g_swarm_last_progress_time = (int64_t)time(NULL);
                        g_swarm_active = true;
                        printf("Swarm sync started: %u chunks from h=%d\n",
                               num_chunks, height);
                    }
                    zcl_mutex_unlock(&g_swarm_mutex);
                }
            }

        } else if (strcmp(cmd, MSG_CHUNK_REQ) == 0) {
            /* Peer requests a specific chunk by index — serve it. */
            uint32_t chunk_index = 0;
            struct sync_manifest manifest;
            if (!stream_read_u32_le(&s, &chunk_index)) {
                printf("Peer %s: bad zchunkreq\n", node->addr_name);
            } else if (!msg_processor_get_manifest_header(&manifest)) {
                printf("Peer %s: zchunkreq but no manifest ready\n",
                       node->addr_name);
            } else if (chunk_index >= manifest.num_chunks) {
                printf("Peer %s: zchunkreq index %u out of range (%u)\n",
                       node->addr_name, chunk_index,
                       manifest.num_chunks);
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
                        if (slen > 128) {
                            /* Script too large for entry — reject chunk.
                             * Don't silently truncate, that corrupts UTXOs. */
                            parse_ok = false; break;
                        }
                        chunk->entries[i].script_len = slen;
                        if (slen > 0 &&
                            !stream_read_bytes(&s, chunk->entries[i].script, slen))
                            { parse_ok = false; break; }
                    }

                    if (parse_ok) {
                        zcl_mutex_lock(&g_swarm_mutex);
                        bool verified = swarm_sync_receive_chunk(
                            &g_swarm, chunk, node->id);
                        node->swarm_inflight_chunk = -1;

                        if (!verified) {
                            zcl_mutex_unlock(&g_swarm_mutex);
                            fprintf(stderr, "Peer %s: chunk %u failed verification\n",
                                   node->addr_name, chunk_index);
                            peer_misbehaving(mp->net_mgr, node, 50,
                                             "bad chunk hash");
                        } else if (swarm_sync_is_complete(&g_swarm)) {
                            printf("Swarm sync complete: %u/%u chunks\n",
                                   g_swarm.chunks_complete,
                                   g_swarm.manifest.num_chunks);

                            /* Verify SHA3 UTXO commitment matches the
                             * snapshot offer's root hash. This catches
                             * any data corruption during transfer. */
                            {
                                struct node_db *ndb = msg_node_db(mp);
                                if (ndb && ndb->db) {
                                uint8_t local_root[32];
                                uint64_t local_count = 0;
                                utxo_commitment_sha3_compute(
                                    ndb->db,
                                    local_root, &local_count);
                                if (memcmp(local_root,
                                           g_swarm.manifest.merkle_root,
                                           32) == 0) {
                                    printf("SHA3 UTXO verification: PASSED "
                                           "(%lu UTXOs)\n",
                                           (unsigned long)local_count);
                                } else {
                                    printf("SHA3 UTXO verification: FAILED "
                                           "— snapshot data corrupted!\n");
                                }
                            }
                            }

                            swarm_sync_free(&g_swarm);
                            g_swarm_active = false;
                            zcl_mutex_unlock(&g_swarm_mutex);
                        } else {
                            zcl_mutex_unlock(&g_swarm_mutex);
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

        /* ── Block swarm messages (parallel block download) ──── */

        } else if (strcmp(cmd, MSG_BLOCK_MANIFEST) == 0) {
            /* Peer sends their block piece manifest.
             * DEFENSIVE: validate all fields before trusting any data. */
            int32_t start_h = 0, end_h = 0;
            uint32_t num_pieces = 0;
            uint8_t tip_hash[32], merkle_root[32];

            if (stream_read_i32_le(&s, &start_h) &&
                stream_read_i32_le(&s, &end_h) &&
                stream_read_u32_le(&s, &num_pieces) &&
                stream_read_bytes(&s, tip_hash, 32) &&
                stream_read_bytes(&s, merkle_root, 32)) {

                /* Sanity: heights must be positive and consistent */
                if (start_h < 0 || end_h < start_h || num_pieces == 0 ||
                    num_pieces > 100000) {
                    printf("Peer %s: invalid block manifest "
                           "(start=%d end=%d pieces=%u)\n",
                           node->addr_name, start_h, end_h, num_pieces);
                    peer_misbehaving(mp->net_mgr, node, 20,
                                     "invalid block manifest params");
                } else {
                    /* Verify piece count is consistent with height range */
                    uint32_t expected = (uint32_t)((end_h - start_h +
                        BLOCKS_PER_PIECE) / BLOCKS_PER_PIECE);
                    if (num_pieces != expected) {
                        fprintf(stderr, "Peer %s: block manifest piece count mismatch "
                               "(got %u, expected %u for h=%d..%d)\n",
                               node->addr_name, num_pieces, expected,
                               start_h, end_h);
                        peer_misbehaving(mp->net_mgr, node, 10,
                                         "block manifest piece count wrong");
                    } else {
                        node->blk_manifest_received = true;
                        node->blk_peer_height = end_h;
                    }
                }

                int our_h = active_chain_height(&mp->main_state->chain_active);
                if (node->blk_manifest_received)
                    printf("Peer %s: block manifest h=%d..%d (%u pieces)\n",
                           node->addr_name, start_h, end_h, num_pieces);

                /* If peer is ahead and no active block swarm, start one. */
                if (node->blk_manifest_received &&
                    end_h > our_h + BLOCKS_PER_PIECE &&
                    !g_block_swarm_active && num_pieces > 0) {
                    struct block_piece_manifest pm = {
                        .start_height = start_h,
                        .end_height = end_h,
                        .num_pieces = num_pieces,
                        .piece_hashes = NULL
                    };
                    memcpy(pm.tip_hash, tip_hash, 32);
                    memcpy(pm.merkle_root, merkle_root, 32);

                    pthread_mutex_lock(&g_block_swarm_mutex);
                    if (block_swarm_init(&g_block_swarm, &pm, mp->datadir)) {
                        g_block_swarm_active = true;
                        g_block_swarm_last_progress = (int64_t)time(NULL);
                        printf("Block swarm started: %u pieces, h=%d..%d\n",
                               num_pieces, start_h, end_h);
                    }
                    pthread_mutex_unlock(&g_block_swarm_mutex);
                }
            }

        } else if (strcmp(cmd, MSG_BLOCK_REQ) == 0) {
            /* Peer requests a specific block piece by index.
             * SAFETY: only serve if we have a valid manifest and the
             * piece index is in range. Rate-limited like chunk requests. */
            uint32_t piece_index = 0;
            struct block_piece_manifest bm;
            if (!stream_read_u32_le(&s, &piece_index)) {
                printf("Peer %s: bad zblkreq\n", node->addr_name);
            } else if (!msg_processor_get_block_manifest_header(&bm, NULL)) {
                printf("Peer %s: zblkreq but no block manifest\n",
                       node->addr_name);
            } else if (piece_index >= bm.num_pieces) {
                printf("Peer %s: zblkreq %u out of range (%u)\n",
                       node->addr_name, piece_index,
                       bm.num_pieces);
                peer_misbehaving(mp->net_mgr, node, 10,
                                 "zblkreq out of range");
            } else if (!fast_sync_rate_check(&g_rate_limiter,
                                              node->addr.svc.addr.ip)) {
                printf("Peer %s: rate limited on block piece\n",
                       node->addr_name);
            } else {
                /* Serve the piece: send block hashes for this piece range.
                 * The requester can then verify against their manifest. */
                int32_t piece_start = bm.start_height
                    + (int32_t)(piece_index * BLOCKS_PER_PIECE);
                int32_t piece_end = piece_start + BLOCKS_PER_PIECE - 1;
                if (piece_end > bm.end_height)
                    piece_end = bm.end_height;

                /* Read block hashes via ActiveRecord model */
                uint8_t piece_hashes[BLOCKS_PER_PIECE][32];
                int block_count = db_block_hashes_in_range(
                    msg_node_db(mp), piece_start, piece_end,
                    piece_hashes, BLOCKS_PER_PIECE);
                if (block_count > 0) {
                    struct byte_stream bs_msg;
                    stream_init(&bs_msg, 4 + 4 + 32 * (size_t)block_count);
                    stream_write_u32_le(&bs_msg, piece_index);
                    stream_write_u32_le(&bs_msg, (uint32_t)block_count);
                    for (int bi = 0; bi < block_count; bi++)
                        stream_write_bytes(&bs_msg, piece_hashes[bi], 32);
                    p2p_node_begin_message(node, MSG_BLOCK_DATA,
                                            mp->params->pchMessageStart);
                    p2p_node_write_message_data(node, bs_msg.data, bs_msg.size);
                    p2p_node_end_message(node);
                    stream_free(&bs_msg);
                }
            }

        } else if (strcmp(cmd, MSG_BLOCK_DATA) == 0) {
            /* Peer sends block piece data (block hashes for a piece).
             * DEFENSIVE: validate piece_index, block_count, and hash. */
            uint32_t piece_index = 0, block_count = 0;
            if (!stream_read_u32_le(&s, &piece_index) ||
                !stream_read_u32_le(&s, &block_count) ||
                block_count == 0 || block_count > BLOCKS_PER_PIECE) {
                printf("Peer %s: bad zblkdata (piece=%u count=%u)\n",
                       node->addr_name, piece_index, block_count);
                peer_misbehaving(mp->net_mgr, node, 20,
                                 "bad zblkdata header");
            } else if (!g_block_swarm_active) {
                printf("Peer %s: zblkdata piece=%u but no block swarm\n",
                       node->addr_name, piece_index);
            } else {
                /* Read block hashes */
                uint8_t (*blk_hashes)[32] = calloc(block_count, 32);
                bool parse_ok = true;
                if (blk_hashes) {
                    for (uint32_t i = 0; i < block_count && parse_ok; i++) {
                        if (!stream_read_bytes(&s, blk_hashes[i], 32))
                            parse_ok = false;
                    }
                }

                if (parse_ok && blk_hashes) {
                    /* DEFENSIVE: bounds check before touching swarm */
                    pthread_mutex_lock(&g_block_swarm_mutex);
                    if (piece_index >= g_block_swarm.manifest.num_pieces) {
                        pthread_mutex_unlock(&g_block_swarm_mutex);
                        printf("Peer %s: zblkdata piece %u out of range "
                               "(max %u)\n", node->addr_name, piece_index,
                               g_block_swarm.manifest.num_pieces);
                        peer_misbehaving(mp->net_mgr, node, 20,
                                         "zblkdata piece out of range");
                        free(blk_hashes);
                        goto _blkdata_done;
                    }

                    /* Compute piece hash and verify against manifest.
                     * SHA3-256 of (piece_index || count || block_hashes[]).
                     * This is the core integrity check — if the hash doesn't
                     * match the manifest, the peer sent bad data. */
                    uint8_t computed_hash[32];
                    block_piece_hash(
                        (const uint8_t (*)[32])blk_hashes,
                        block_count, piece_index, computed_hash);

                    bool verified = false;
                    if (g_block_swarm.manifest.piece_hashes) {
                        verified = memcmp(computed_hash,
                            g_block_swarm.manifest.piece_hashes[piece_index],
                            32) == 0;
                    }

                    if (verified) {
                        block_swarm_receive_piece(&g_block_swarm,
                                                   piece_index, node->id);
                        /* Clear pipeline slot */
                        for (int pi = 0; pi < 4; pi++) {
                            if (node->blk_pipeline[pi].piece_index ==
                                (int32_t)piece_index) {
                                node->blk_pipeline[pi].piece_index = -1;
                                break;
                            }
                        }

                        if (block_swarm_is_complete(&g_block_swarm)) {
                            printf("Block swarm complete: %u/%u pieces\n",
                                   g_block_swarm.pieces_complete,
                                   g_block_swarm.manifest.num_pieces);
                            block_swarm_free(&g_block_swarm);
                            g_block_swarm_active = false;
                        }
                    } else {
                        block_swarm_fail_piece(&g_block_swarm, piece_index);
                        fprintf(stderr, "Peer %s: block piece %u failed verification\n",
                               node->addr_name, piece_index);
                        peer_misbehaving(mp->net_mgr, node, 50,
                                         "bad block piece hash");
                    }
                    pthread_mutex_unlock(&g_block_swarm_mutex);
                } else {
                    printf("Peer %s: truncated zblkdata\n", node->addr_name);
                    peer_misbehaving(mp->net_mgr, node, 20,
                                     "truncated zblkdata");
                }
                free(blk_hashes);
            }
            _blkdata_done:;

        } else if (strcmp(cmd, MSG_BLOCK_BITMAP) == 0) {
            /* Peer sends their piece availability bitmap.
             * DEFENSIVE: validate length is reasonable. */
            uint32_t bitmap_len = 0;
            if (!stream_read_u32_le(&s, &bitmap_len) ||
                bitmap_len == 0 || bitmap_len > 65536) {
                printf("Peer %s: bad zblkbitmap len=%u\n",
                       node->addr_name, bitmap_len);
            } else {
                uint8_t *bitmap = calloc(bitmap_len, 1);
                if (bitmap && stream_read_bytes(&s, bitmap, bitmap_len)) {
                    /* Store on peer for rarest-first selection */
                    free(node->blk_bitmap);
                    node->blk_bitmap = bitmap;
                    node->blk_bitmap_len = bitmap_len;

                    /* Update global availability counts */
                    if (g_block_swarm_active) {
                        pthread_mutex_lock(&g_block_swarm_mutex);
                        block_swarm_update_availability(&g_block_swarm,
                                                         bitmap, bitmap_len);
                        pthread_mutex_unlock(&g_block_swarm_mutex);
                    }
                } else {
                    free(bitmap);
                    printf("Peer %s: truncated zblkbitmap\n",
                           node->addr_name);
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
        }

        stream_free(&s);
        net_message_free(&msg);

        if (!ok) {
            printf("Peer %s: error processing %s\n", node->addr_name, cmd);
        }
    }
    return true;
}

static void push_getheaders_from(struct msg_processor *mp,
                                  struct p2p_node *node,
                                  struct block_index *from)
{
    if (from && !from->phashBlock) return;

    /* Don't request headers during any snapshot sync state. */
    if (snapsync_is_active())
        return;

    struct block_locator loc;
    if (!syncsvc_build_getheaders_locator(&loc, &mp->main_state->chain_active,
                                          from,
                                          &mp->params->consensus.hashGenesisBlock))
        return;

    struct byte_stream s;
    stream_init(&s, 512);
    if (!getheaders_serialize(&s, &loc, NULL)) {
        stream_free(&s);
        block_locator_free(&loc);
        return;
    }

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

static void exec_getheaders_action(struct msg_processor *mp,
                                   struct p2p_node *node,
                                   const struct sync_getheaders_action *action)
{
    struct block_index *tip;

    if (!mp || !node || !action || !action->should_send)
        return;

    tip = active_chain_tip(&mp->main_state->chain_active);
    switch (action->anchor) {
    case SYNC_HEADER_REQUEST_TIP_PARENT:
        if (tip && tip->pprev)
            push_getheaders_from(mp, node, tip->pprev);
        else
            push_getheaders(mp, node);
        break;
    case SYNC_HEADER_REQUEST_TIP:
    case SYNC_HEADER_REQUEST_EXPLICIT:
    default:
        push_getheaders(mp, node);
        break;
    }
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
        node->zsync_sent == 0) { /* only offer once */
        int our_h = msg_get_height(mp);
        if (our_h > 100 &&
            (node->starting_height < 0 ||
             our_h > node->starting_height + 100)) {
            struct snapshot_offer offer;
            if (msg_processor_get_offer(&offer)) {
                node->zsync_sent = UINT64_MAX; /* mark: offered */
                event_emitf(EV_SNAPSHOT_OFFER_SENT, (uint32_t)node->id,
                            "h=%d utxos=%llu", offer.height,
                            (unsigned long long)offer.num_utxos);
                printf("Peer %s: offering snapshot (us=%d, peer=%d)\n",
                       node->addr_name, our_h, node->starting_height);
                send_snapshot_offer_msg(node, &offer,
                                        mp->params->pchMessageStart);
            }
        }
    }

    /* Initiate sync, and periodically re-request if behind. */
    {
        bool should_sync = syncsvc_begin_peer_sync(node);
        struct sync_getheaders_action periodic = {0};

        int our_height = msg_get_height(mp);
        bool in_ibd = syncsvc_is_initial_block_download(node, our_height);

        /* Re-request headers aggressively during IBD (10s), slower at tip (60s).
         * This is critical: legacy zclassicd sends at most 2000 headers per
         * getheaders response — for a 3M block chain, we need ~1500 rounds. */
        int64_t now_send = (int64_t)time(NULL);
        syncsvc_plan_periodic_getheaders(&periodic, node, our_height, now_send);
        if (periodic.should_send && !snapsync_is_active()) {
            should_sync = true;
            syncsvc_note_headers_requested(node, now_send);
        }
        if (should_sync) {
            struct block_index *tip = active_chain_tip(
                &mp->main_state->chain_active);
            switch (syncsvc_header_log_mode(node, tip, in_ibd)) {
            case SYNC_HEADER_LOG_IBD:
                printf("IBD getheaders to %s (height=%d, peer=%d, "
                       "behind=%d)\n",
                       node->addr_name, tip->nHeight,
                       node->starting_height,
                       node->starting_height - tip->nHeight);
                break;
            case SYNC_HEADER_LOG_TIP:
                printf("Sending getheaders to %s (height=%d, peer=%d)\n",
                       node->addr_name, tip->nHeight,
                       node->starting_height);
                break;
            case SYNC_HEADER_LOG_NONE:
            default:
                break;
            }
            exec_getheaders_action(mp, node, &periodic);
        }
    }

    /* ── Download manager: assign queued blocks to this peer ────── */
    {
        struct download_manager *dm = get_download_mgr();
        int64_t now_dl = (int64_t)time(NULL);

        /* Check timeouts (cheap — linear scan of active slots) */
        size_t timed_out = dl_check_timeouts(dm, now_dl);
        if (timed_out > 0)
            event_emitf(EV_BLOCK_REQUESTED, 0,
                        "timeouts=%zu reassigned to queue", timed_out);

        /* Assign blocks from queue to this peer if they have capacity */
        {
            struct uint256 assign_hashes[DL_WINDOW_SIZE];
            struct sync_block_batch batch;
            syncsvc_assign_peer_blocks(&batch, dm, node, assign_hashes,
                                       DL_WINDOW_SIZE);
            if (batch.assigned > 0) {
                struct byte_stream getdata_msg;
                stream_init(&getdata_msg, batch.assigned * 36 + 8);
                if (getdata_blocks_serialize(&getdata_msg, assign_hashes,
                                             batch.assigned)) {
                    p2p_node_begin_message(node, "getdata",
                                           mp->params->pchMessageStart);
                    p2p_node_write_message_data(node, getdata_msg.data,
                                                getdata_msg.size);
                    p2p_node_end_message(node);
                }
                stream_free(&getdata_msg);

                {
                    char hex[65];
                    uint256_get_hex(&assign_hashes[0], hex);
                    printf("getdata: %zu blocks to %s (first=%s)\n",
                           batch.assigned, node->addr_name, hex);
                }
                event_emitf(EV_BLOCK_REQUESTED, (uint32_t)node->id,
                            "assigned=%zu inflight=%zu",
                            batch.assigned,
                            batch.in_flight_before + batch.assigned);
            }

            /* Stall detection: if queue is empty, in-flight is zero,
             * and we're not at tip — find alternative blocks to download.
             * Scan block_map for blocks at tip+1 with header data but
             * no block data, that aren't failed. Queue them. */
            uint64_t dl_queued = 0, dl_inflight = 0;
            dl_get_stats(dm, NULL, NULL, NULL, &dl_inflight, &dl_queued);
            struct sync_stall_recovery recovery;
            if (syncsvc_build_stall_recovery(&recovery, mp->main_state, node,
                                             dl_queued, dl_inflight, now_dl)) {
                if (recovery.should_log) {
                    printf("STALL: h=%d entries_at_%d=%zu (data=%zu fail=%zu)\n",
                           recovery.chain_height, recovery.next_height,
                           recovery.entries_at_next, recovery.entries_with_data,
                           recovery.entries_failed);
                }
                {
                    int cleared = 0;
                    syncsvc_apply_stall_recovery(&recovery, mp->main_state,
                                                 dm, &cleared);
                    if (recovery.alt_count > 0) {
                        printf("Stall recovery: queued %zu alt blocks\n",
                               recovery.alt_count);
                    } else if (cleared > 0) {
                        printf("Stall recovery: reset %d blocks at h=%d for re-download\n",
                               cleared, recovery.next_height);
                    }
                }
                if (recovery.should_recover) {
                    struct block_index *tip = active_chain_tip(
                        &mp->main_state->chain_active);
                    struct sync_getheaders_action action = {0};
                    syncsvc_plan_recovery_getheaders(&action, &recovery, tip);
                    exec_getheaders_action(mp, node, &action);
                }
                syncsvc_free_stall_recovery(&recovery);
            }
        }
    }

    /* ── IBD progress log (every 30s, first outbound peer only) ── */
    {
        static int64_t last_progress_log = 0;
        int64_t now_prog = (int64_t)time(NULL);
        if (!node->inbound && node->id == 0 &&
            now_prog - last_progress_log >= 30) {
            last_progress_log = now_prog;
            struct download_manager *dm2 = get_download_mgr();
            int h = msg_get_height(mp);
            int header_tip = mp->main_state->pindex_best_header
                ? mp->main_state->pindex_best_header->nHeight : h;
            struct sync_progress_snapshot progress;
            syncsvc_collect_progress(&progress, dm2, sync_get_state(),
                                     h, header_tip, node->last_block_time,
                                     now_prog);
            if (progress.should_log_progress) {
                printf("IBD: chain=%d headers=%d sync=%s "
                       "dl[req=%llu recv=%llu flight=%llu queue=%llu "
                       "timeout=%llu] %.2f GB @ %.1f MB/s\n",
                       progress.chain_height, progress.header_height,
                       sync_state_name(progress.sync_state),
                       (unsigned long long)progress.requested,
                       (unsigned long long)progress.received,
                       (unsigned long long)progress.in_flight,
                       (unsigned long long)progress.queued,
                       (unsigned long long)progress.timed_out,
                       progress.gib_received, progress.mbps_avg);
            }

            /* Tip-stale watchdog: if we're at the tip but haven't
             * received a new block in 10 minutes, re-request headers
             * from all outbound peers. Handles the case where all
             * peers went silent or our inv relay is broken. */
            {
                struct sync_getheaders_action stale = {0};
                syncsvc_plan_tip_stale_getheaders(&stale, &progress,
                                                  node, now_prog);
                if (stale.should_send) {
                    printf("Tip stale: no new block for %llds, "
                           "re-requesting headers\n",
                           (long long)progress.tip_stale_seconds);
                    exec_getheaders_action(mp, node, &stale);
                }
            }
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

    /* Stream fast sync UTXO chunks if serving this peer.
     * Zero-copy from in-memory buffer — no file I/O, no SQL.
     * Snapshot pre-loaded into RAM at startup (~97 MB). */
    if (node->state == PEER_SNAPSHOT_SERVING) {
        struct snapshot_offer offer;
        int64_t buf_size = 0;
        const uint8_t *buf = fast_sync_get_snapshot_buf(&buf_size);
        if (buf && buf_size > 0 && msg_processor_get_offer(&offer)) {
            uint64_t current_offer_version = msg_processor_offer_cache_version();
            uint64_t current_snapshot_version =
                fast_sync_snapshot_cache_version();
            bool stale_offer =
                node->zsync_offered_height <= 0 ||
                node->zsync_offered_count == 0 ||
                node->zsync_offer_version != current_offer_version ||
                node->zsync_snapshot_version != current_snapshot_version ||
                node->zsync_offered_height != offer.height ||
                node->zsync_offered_count != offer.num_utxos ||
                memcmp(node->zsync_offered_root, offer.utxo_root, 32) != 0 ||
                memcmp(node->zsync_offered_block, offer.block_hash, 32) != 0;
            if (stale_offer) {
                printf("Peer %s: snapshot changed while serving; "
                       "resetting to re-offer latest snapshot\n",
                       node->addr_name);
                node->zsync_offset = 0;
                node->zsync_sent = 0;
                node->zsync_file_offset = 0;
                node->zsync_file_size = 0;
                peer_set_state_checked((uint32_t)node->id, &node->state,
                                       PEER_ACTIVE,
                                       "snapshot serve stale offer");
                return true;
            }
            /* Send chunks from memory, respecting TCP flow control.
             * Stop when send buffer exceeds 2MB to avoid backlog. */
            for (int batch = 0; batch < 200; batch++) {
                struct snapsync_serve_step step;
                if (!snapsync_prepare_serve_step(&step, node, buf, buf_size))
                    break;
                if (step.action == SNAPSYNC_SERVE_ACTION_NONE)
                    break;
                if (step.action == SNAPSYNC_SERVE_ACTION_SEND_END) {
                    /* EOF — all UTXOs sent */
                    struct snapsync_serve_complete complete = {0};
                    snapsync_build_serve_complete(&complete);
                    p2p_node_begin_message(node, MSG_SNAPSHOT_END,
                                            mp->params->pchMessageStart);
                    p2p_node_end_message(node);
                    if (complete.should_update_peer_state) {
                        peer_set_state_checked((uint32_t)node->id, &node->state,
                                               complete.peer_state,
                                               "snapshot serve done");
                    }
                    printf("Peer %s: snapshot complete (%llu UTXOs, "
                           "%llu chunks sent)\n",
                           node->addr_name,
                           (unsigned long long)node->zsync_offset,
                           (unsigned long long)node->zsync_sent);
                    break;
                }

                /* Send chunk directly from memory — true zero-copy */
                p2p_node_begin_message(node, MSG_SNAPSHOT_DATA,
                                        mp->params->pchMessageStart);
                p2p_node_write_message_data(node, buf + step.chunk_offset,
                                            step.chunk_len);
                p2p_node_end_message(node);

                if (node->zsync_sent % 100 == 0) {
                    printf("Peer %s: sent %llu/%llu UTXOs (%.0f%%)\n",
                           node->addr_name,
                           (unsigned long long)node->zsync_offset,
                           (unsigned long long)node->zsync_total,
                           node->zsync_total > 0 ?
                               100.0 * (double)node->zsync_offset / (double)node->zsync_total : 0);
                }
            }
        } else {
            fprintf(stderr, "Peer %s: no snapshot in memory\n", node->addr_name);
            peer_set_state_checked((uint32_t)node->id, &node->state,
                                   PEER_ACTIVE, "no snapshot buffer");
        }
    }

    /* ── Swarm parallel chunk sync coordinator ────────────── */
    /* For each connected ZCL23 peer with no inflight chunk, assign one
     * and send a zchunkreq. Also handle timeouts on stale requests. */
    if (g_swarm_active && peer_supports_fast_sync(node->services) &&
        node->swarm_manifest_received &&
        node->state >= PEER_HANDSHAKE_COMPLETE) {

        zcl_mutex_lock(&g_swarm_mutex);

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

        /* Progress display (rate-limited to every 5 seconds) */
        int64_t now_prog = (int64_t)time(NULL);
        if (now_prog - g_swarm_last_progress_time >= SWARM_PROGRESS_INTERVAL_SECS) {
            g_swarm_last_progress_time = now_prog;

            int progress = swarm_sync_progress(&g_swarm);
            uint32_t complete = g_swarm.chunks_complete;
            uint32_t total = g_swarm.manifest.num_chunks;
            uint32_t inflight = g_swarm.chunks_inflight;
            zcl_mutex_unlock(&g_swarm_mutex);

            /* Count serving peers (no swarm lock needed) */
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
                   progress, complete, total, inflight, serving_peers);
        } else {
            zcl_mutex_unlock(&g_swarm_mutex);
        }
    }

    /* ── Block swarm coordinator: parallel block piece download ── */
    /* Only for ZCL23 peers with completed handshake. Legacy peers
     * contribute via normal getdata/block (handled by download manager). */
    if (g_block_swarm_active && peer_supports_fast_sync(node->services) &&
        node->blk_manifest_received &&
        node->state >= PEER_HANDSHAKE_COMPLETE) {

        pthread_mutex_lock(&g_block_swarm_mutex);

        /* Handle timeouts on this peer's pipeline */
        int64_t now_bs = (int64_t)time(NULL);
        for (int pi = 0; pi < PIECE_PIPELINE_DEPTH; pi++) {
            int32_t pidx = node->blk_pipeline[pi].piece_index;
            if (pidx >= 0 &&
                now_bs - node->blk_pipeline[pi].request_time >
                    SWARM_CHUNK_TIMEOUT_SECS) {
                if ((uint32_t)pidx < g_block_swarm.manifest.num_pieces &&
                    g_block_swarm.piece_states[pidx] == CHUNK_INFLIGHT) {
                    g_block_swarm.piece_states[pidx] = CHUNK_NEEDED;
                    g_block_swarm.piece_peer[pidx] = -1;
                    if (g_block_swarm.pieces_inflight > 0)
                        g_block_swarm.pieces_inflight--;
                }
                node->blk_pipeline[pi].piece_index = -1;
            }
        }

        /* Fill empty pipeline slots with new piece assignments */
        for (int pi = 0; pi < PIECE_PIPELINE_DEPTH; pi++) {
            if (node->blk_pipeline[pi].piece_index >= 0)
                continue; /* slot occupied */

            int32_t pidx = block_swarm_assign_piece(
                &g_block_swarm, node->id, node->blk_bitmap);
            if (pidx < 0)
                break; /* no more pieces to assign */

            node->blk_pipeline[pi].piece_index = pidx;
            node->blk_pipeline[pi].request_time = now_bs;

            pthread_mutex_unlock(&g_block_swarm_mutex);
            push_block_piece_request(mp, node, (uint32_t)pidx);
            pthread_mutex_lock(&g_block_swarm_mutex);
        }

        /* Progress display (rate-limited) */
        int64_t now_bp = (int64_t)time(NULL);
        if (now_bp - g_block_swarm_last_progress >=
            SWARM_PROGRESS_INTERVAL_SECS) {
            g_block_swarm_last_progress = now_bp;
            int bprog = block_swarm_progress(&g_block_swarm);
            uint32_t bcomplete = g_block_swarm.pieces_complete;
            uint32_t btotal = g_block_swarm.manifest.num_pieces;
            uint32_t binflight = g_block_swarm.pieces_inflight;
            bool endgame = g_block_swarm.endgame;
            pthread_mutex_unlock(&g_block_swarm_mutex);

            printf("BlockSync: %d%% (%u/%u pieces, %u inflight%s)\n",
                   bprog, bcomplete, btotal, binflight,
                   endgame ? " [endgame]" : "");
        } else {
            pthread_mutex_unlock(&g_block_swarm_mutex);
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
