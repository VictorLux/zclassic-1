/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "net/msgprocessor.h"
#include "net/msg_internal.h"
#include "net/addrman.h"
#include "storage/disk_block_io.h"
#include "services/chain_activation_controller.h"
#include <signal.h>
extern volatile sig_atomic_t g_shutdown_requested;
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
#include "net/peer_scoring.h"
#include "services/chain_state_repository.h"
#include "services/header_sync_service.h"
#include "services/block_sync_service.h"
#include "core/hash.h"
#include "core/random.h"
#include "core/serialize.h"
#include "bloom/bloom.h"
#include "net/compact_blocks.h"
#include "net/dandelion.h"
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
#include "util/safe_alloc.h"
#include "util/log_macros.h"
#include "services/sync_watchdog_service.h"
#include "net/connman.h"
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>

/* push_getheaders, push_getheaders_from, exec_getheaders_action
 * are declared in msg_internal.h and defined in msg_headers.c */

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

/* ── Header sync stall tracking (used by msg_send_messages) ───── */
static int g_header_stall_last_height = 0;
static int64_t g_header_stall_last_advance = 0;

/* Dandelion++ state (g_dandelion, g_dandelion_init) defined in msg_tx.c */

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
        LOG_FAIL("net", "manifest is NULL");
    if (manifest->num_chunks == 0)
        LOG_FAIL("net", "manifest has zero chunks");
    if (manifest->chunk_size == 0)
        LOG_FAIL("net", "manifest has zero chunk_size");
    if (!manifest->chunk_hashes)
        LOG_FAIL("net", "manifest chunk_hashes is NULL");
    return true;
}

static bool msg_block_manifest_is_reasonable(
    const struct block_piece_manifest *manifest)
{
    if (!manifest)
        LOG_FAIL("net", "block manifest is NULL");
    if (manifest->num_pieces == 0)
        LOG_FAIL("net", "block manifest has zero pieces");
    if (manifest->start_height > manifest->end_height)
        LOG_FAIL("net", "block manifest start_height %d > end_height %d",
                 manifest->start_height, manifest->end_height);
    if (!manifest->piece_hashes)
        LOG_FAIL("net", "block manifest piece_hashes is NULL");
    return true;
}

struct node_db *msg_node_db(const struct msg_processor *mp)
{
    if (!mp || !mp->runtime)
        LOG_NULL("net", "mp or mp->runtime is NULL");
    return db_service_node_db(mp->runtime->db_service);
}

struct snapshot_sync_service *msg_snapshot_sync(
    const struct msg_processor *mp)
{
    if (mp && mp->runtime && mp->runtime->snapshot_sync)
        return mp->runtime->snapshot_sync;
    if (snapsync_global_initialized())
        return snapsync_global();
    LOG_NULL("net", "no snapshot sync service available");
}

struct snapshot_sync_service *msg_snapshot_sync_ensure(
    const struct msg_processor *mp)
{
    struct snapshot_sync_service *svc = msg_snapshot_sync(mp);
    struct node_db *ndb;

    if (svc)
        return svc;
    ndb = msg_node_db(mp);
    if (!ndb)
        LOG_NULL("net", "node_db unavailable for snapshot sync init");
    snapsync_global_ensure_init(ndb);
    return snapsync_global();
}

struct wallet *msg_wallet(const struct msg_processor *mp)
{
    if (!mp || !mp->runtime)
        LOG_NULL("net", "mp or mp->runtime is NULL in msg_wallet");
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
        LOG_FAIL("net", "offer output pointer is NULL");

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
        LOG_FAIL("net", "cannot publish unreasonable manifest");

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
        LOG_FAIL("net", "manifest output pointer is NULL");

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
        LOG_FAIL("net", "cannot publish unreasonable block manifest");

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
        LOG_FAIL("net", "block manifest output pointer is NULL");

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
void send_snapshot_offer_msg(struct p2p_node *node,
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

bool block_already_seen(const struct uint256 *hash) {
    int limit = g_recent_block_count < MAX_RECENT_BLOCKS
                ? g_recent_block_count : MAX_RECENT_BLOCKS;
    for (int i = 0; i < limit; i++) {
        if (uint256_eq(hash, &g_recent_blocks[i])) return true;
    }
    return false;
}

void block_mark_seen(const struct uint256 *hash) {
    g_recent_blocks[g_recent_block_count % MAX_RECENT_BLOCKS] = *hash;
    g_recent_block_count++;
}

void block_clear_seen(const struct uint256 *hash) {
    int limit = g_recent_block_count < MAX_RECENT_BLOCKS
                ? g_recent_block_count : MAX_RECENT_BLOCKS;
    for (int i = 0; i < limit; i++) {
        if (uint256_eq(hash, &g_recent_blocks[i])) {
            memset(&g_recent_blocks[i], 0, sizeof(struct uint256));
            return;
        }
    }
}

bool tx_already_seen(const struct uint256 *hash) {
    int limit = g_recent_tx_count < MAX_RECENT_TXS
                ? g_recent_tx_count : MAX_RECENT_TXS;
    for (int i = 0; i < limit; i++) {
        if (uint256_eq(hash, &g_recent_txs[i])) return true;
    }
    return false;
}

void tx_mark_seen(const struct uint256 *hash) {
    g_recent_txs[g_recent_tx_count % MAX_RECENT_TXS] = *hash;
    g_recent_tx_count++;
}

static bool msg_should_ignore_snapshot_offer(enum snapshot_sync_state snapsync_state,
                                             uint32_t serving_peer_id,
                                             enum peer_state peer_state,
                                             uint32_t peer_id,
                                             enum sync_state sync_state)
{
    (void)serving_peer_id;

    if (sync_state == SYNC_AT_TIP)
        return true;
    if (peer_state == PEER_SNAPSHOT_RECEIVING)
        return true;
    if (snapsync_state == SNAPSYNC_NEGOTIATING ||
        snapsync_state == SNAPSYNC_RECEIVING ||
        snapsync_state == SNAPSYNC_VERIFYING)
        return true;
    if (peer_id == 0)
        return false;
    return false;
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
        LOG_FAIL("net", "hash is NULL in accept_block_for_processing");
    if (snapshot_active)
        LOG_FAIL("net", "block rejected: snapshot sync is active");
    if (block_already_seen(hash))
        LOG_FAIL("net", "block rejected: already seen");
    block_mark_seen(hash);
    return true;
}
bool msgprocessor_test_should_ignore_snapshot_offer(
    enum snapshot_sync_state snapsync_state,
    uint32_t serving_peer_id,
    enum peer_state peer_state,
    uint32_t peer_id,
    enum sync_state sync_state) {
    return msg_should_ignore_snapshot_offer(snapsync_state, serving_peer_id,
                                            peer_state, peer_id, sync_state);
}
void msgprocessor_block_clear_seen(const struct uint256 *hash) {
    block_clear_seen(hash);
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

    /* Initialize Dandelion++ tx propagation */
    if (!g_dandelion_init) {
        dandelion_init(&g_dandelion);
        g_dandelion_init = true;
    }

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
void push_manifest(struct msg_processor *mp, struct p2p_node *node)
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
void push_block_manifest(struct msg_processor *mp,
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

/* Function(s) removed — moved to split files */

static bool process_ping(struct msg_processor *mp, struct p2p_node *node,
                          struct byte_stream *s)
{
    uint64_t nonce = 0;
    if (node->version >= BIP0031_VERSION) {
        if (!stream_read_u64_le(s, &nonce))
            LOG_FAIL("net", "failed to read ping nonce from %s",
                     node->addr_name);
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
        LOG_FAIL("net", "failed to read pong nonce from %s",
                 node->addr_name);

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
        LOG_FAIL("net", "failed to read addr count from %s",
                 node->addr_name);

    if (count > MAX_ADDR_TO_SEND) {
        printf("Peer %s: addr message too large (%llu)\n",
               node->addr_name, (unsigned long long)count);
        node->disconnect = true;
        LOG_FAIL("net", "addr count %llu exceeds max from %s",
                 (unsigned long long)count, node->addr_name);
    }

    struct net_addr source;
    net_addr_init(&source);
    memcpy(source.ip, node->addr.svc.addr.ip, 16);

    for (uint64_t i = 0; i < count; i++) {
        struct net_address addr;
        net_address_init(&addr);
        if (!net_address_deserialize(&addr, s, true))
            LOG_FAIL("net", "failed to deserialize addr[%llu] from %s",
                     (unsigned long long)i, node->addr_name);

        if (mp->net_mgr)
            addrman_add(&mp->net_mgr->addrman, &addr, &source, 0);
    }
    return true;
}

/* Function(s) removed — moved to split files */

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

/* Function(s) removed — moved to split files */

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
        LOG_FAIL("net", "failed to read feefilter rate from %s",
                 node->addr_name);
    (void)fee_rate;
    (void)node;
    return true;
}

static bool process_notfound(struct p2p_node *node, struct byte_stream *s)
{
    uint64_t count;
    if (!stream_read_compact_size(s, &count))
        LOG_FAIL("net", "failed to read notfound count from %s",
                 node->addr_name);

    struct download_manager *dm = get_download_mgr();
    for (uint64_t i = 0; i < count; i++) {
        struct inv_item inv;
        if (!inv_item_deserialize(&inv, s))
            LOG_FAIL("net", "failed to deserialize notfound inv[%llu] from %s",
                     (unsigned long long)i, node->addr_name);
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

/* ── Dispatch table handler wrappers ──────────────────────────────
 * Standard handlers have varying signatures; these thin wrappers
 * adapt them to the uniform (mp, node, s) dispatch signature. */

static bool handle_version(struct msg_processor *mp, struct p2p_node *node,
                           struct byte_stream *s)
{
    return process_version(mp, node, s);
}

static bool handle_verack(struct msg_processor *mp, struct p2p_node *node,
                          struct byte_stream *s)
{
    (void)s;
    return process_verack(mp, node);
}

static bool handle_ping(struct msg_processor *mp, struct p2p_node *node,
                        struct byte_stream *s)
{
    return process_ping(mp, node, s);
}

static bool handle_pong(struct msg_processor *mp, struct p2p_node *node,
                        struct byte_stream *s)
{
    (void)mp;
    return process_pong(node, s);
}

static bool handle_addr(struct msg_processor *mp, struct p2p_node *node,
                        struct byte_stream *s)
{
    return process_addr(mp, node, s);
}

static bool handle_inv(struct msg_processor *mp, struct p2p_node *node,
                       struct byte_stream *s)
{
    return process_inv(mp, node, s);
}

static bool handle_getdata(struct msg_processor *mp, struct p2p_node *node,
                           struct byte_stream *s)
{
    return process_getdata(mp, node, s);
}

static bool handle_getblocks(struct msg_processor *mp, struct p2p_node *node,
                             struct byte_stream *s)
{
    return process_getblocks(mp, node, s);
}

static bool handle_getheaders(struct msg_processor *mp, struct p2p_node *node,
                              struct byte_stream *s)
{
    return process_getheaders(mp, node, s);
}

static bool handle_block_msg(struct msg_processor *mp, struct p2p_node *node,
                             struct byte_stream *s)
{
    return process_block_msg(mp, node, s);
}

static bool handle_tx_msg(struct msg_processor *mp, struct p2p_node *node,
                          struct byte_stream *s)
{
    return process_tx_msg(mp, node, s);
}

static bool handle_headers(struct msg_processor *mp, struct p2p_node *node,
                           struct byte_stream *s)
{
    return process_headers(mp, node, s);
}

static bool handle_getaddr(struct msg_processor *mp, struct p2p_node *node,
                           struct byte_stream *s)
{
    (void)s;
    return process_getaddr(mp, node);
}

static bool handle_mempool(struct msg_processor *mp, struct p2p_node *node,
                           struct byte_stream *s)
{
    (void)s;
    return process_mempool(mp, node);
}

static bool handle_sendheaders(struct msg_processor *mp, struct p2p_node *node,
                               struct byte_stream *s)
{
    (void)mp; (void)s;
    return process_sendheaders(node);
}

static bool handle_reject(struct msg_processor *mp, struct p2p_node *node,
                          struct byte_stream *s)
{
    (void)mp;
    return process_reject(node, s);
}

static bool handle_feefilter(struct msg_processor *mp, struct p2p_node *node,
                             struct byte_stream *s)
{
    (void)mp;
    return process_feefilter(node, s);
}

static bool handle_notfound(struct msg_processor *mp, struct p2p_node *node,
                            struct byte_stream *s)
{
    (void)mp;
    return process_notfound(node, s);
}

/* ── BIP37 bloom filter handlers ─────────────────────────────────
 * BIP37 is a known privacy leak: a peer can probe which addresses a
 * node owns by watching false-positive rates across crafted filters.
 * Default OFF — enable only with ZCL_ENABLE_BIP37=1. When disabled,
 * filterload/filteradd/filterclear score the peer as misbehaving. */

static bool handle_filterload(struct msg_processor *mp, struct p2p_node *node,
                               struct byte_stream *s)
{
    (void)s;
    if (!bip37_enabled()) {
        peer_misbehaving(mp->net_mgr, node, 100,
                         "filterload rejected: BIP37 disabled");
        LOG_FAIL("bip37", "filterload from %s — BIP37 disabled, disconnecting",
                 node->addr_name);
    }
    /* Full BIP37 filter loading not implemented — reject even when enabled
     * until a use case justifies it. */
    return true;
}

static bool handle_filteradd(struct msg_processor *mp, struct p2p_node *node,
                              struct byte_stream *s)
{
    (void)s;
    if (!bip37_enabled()) {
        peer_misbehaving(mp->net_mgr, node, 100,
                         "filteradd rejected: BIP37 disabled");
        LOG_FAIL("bip37", "filteradd from %s — BIP37 disabled, disconnecting",
                 node->addr_name);
    }
    return true;
}

static bool handle_filterclear(struct msg_processor *mp, struct p2p_node *node,
                                struct byte_stream *s)
{
    (void)s;
    if (!bip37_enabled()) {
        peer_misbehaving(mp->net_mgr, node, 100,
                         "filterclear rejected: BIP37 disabled");
        LOG_FAIL("bip37", "filterclear from %s — BIP37 disabled, disconnecting",
                 node->addr_name);
    }
    return true;
}

/* ── ZCL Messaging handlers ───────────────────────────────────── */

#include "net/zmsg.h"

static bool handle_zmsg(struct msg_processor *mp, struct p2p_node *node,
                        struct byte_stream *s)
{
    struct zmsg_message msg;
    if (!zmsg_deserialize(&msg, s))
        return true;

    msg.direction = ZMSG_INBOUND;
    msg.channel = ZMSG_CHANNEL_P2P;

    /* Store locally */
    bool is_new = zmsg_store_add(&msg);

    /* Persist to SQLite */
    struct node_db *ndb = msg_node_db(mp);
    if (ndb)
        db_zmsg_save(ndb, &msg);

    /* Send acknowledgment */
    struct byte_stream os;
    stream_init(&os, 64);
    stream_write(&os, msg.msg_id, 32);
    p2p_node_begin_message(node, MSG_ZMSG_ACK,
                           mp->params->pchMessageStart);
    p2p_node_write_message_data(node, os.data, os.size);
    p2p_node_end_message(node);
    stream_free(&os);

    if (is_new) {
        printf("zmsg: received message from %s via peer %s\n",
               msg.sender, node->addr_name);
    }
    return true;
}

static bool handle_zmsgack(struct msg_processor *mp, struct p2p_node *node,
                           struct byte_stream *s)
{
    (void)mp;
    uint8_t ack_id[32];
    if (!stream_read(s, ack_id, 32))
        return true;

    printf("zmsg: delivery ack from peer %s\n", node->addr_name);
    return true;
}

/* ── ZCL Market: file sharing handlers ─────────────────────────── */

#include "net/file_market.h"
#include "crypto/sha3.h"

static bool handle_zfilelist(struct msg_processor *mp, struct p2p_node *node,
                             struct byte_stream *s)
{
    /* Deserialize one or more file offers from the message */
    uint64_t count = 0;
    if (!stream_read_compact_size(s, &count))
        return true;
    if (count > 50) count = 50; /* limit per message */

    for (uint64_t i = 0; i < count; i++) {
        struct file_offer offer;
        if (!file_offer_deserialize(&offer, s))
            break;

        /* Reject expired TTL */
        if (offer.ttl == 0) continue;

        /* Store locally */
        bool is_new = file_market_add_offer(&offer);

        /* Persist to SQLite */
        struct node_db *ndb = msg_node_db(mp);
        if (ndb)
            db_file_offer_save(ndb, &offer);

        /* Re-gossip to other peers if new and TTL > 1 */
        if (is_new && offer.ttl > 1 && mp->net_mgr) {
            struct file_offer fwd = offer;
            fwd.ttl--;

            struct byte_stream os;
            stream_init(&os, 512);
            stream_write_compact_size(&os, 1);
            file_offer_serialize(&fwd, &os);

            zcl_mutex_lock(&mp->net_mgr->cs_nodes);
            for (size_t pi = 0; pi < mp->net_mgr->num_nodes; pi++) {
                struct p2p_node *peer = mp->net_mgr->nodes[pi];
                if (peer->id != node->id &&
                    peer->state >= PEER_HANDSHAKE_COMPLETE &&
                    !peer->disconnect &&
                    peer_supports_fast_sync(peer->services)) {
                    p2p_node_begin_message(peer, MSG_FILE_LIST,
                                           mp->params->pchMessageStart);
                    p2p_node_write_message_data(peer, os.data, os.size);
                    p2p_node_end_message(peer);
                }
            }
            zcl_mutex_unlock(&mp->net_mgr->cs_nodes);
            stream_free(&os);
        }

        printf("market: %s offer '%s' (%.1f MB, %lld zat/MB) from peer %s\n",
               is_new ? "new" : "updated",
               offer.filename,
               offer.size_bytes / (1024.0 * 1024.0),
               (long long)offer.price_per_mb,
               node->addr_name);
    }
    return true;
}

static bool handle_zfilechal(struct msg_processor *mp, struct p2p_node *node,
                             struct byte_stream *s)
{
    struct file_challenge chal;
    if (!file_challenge_deserialize(&chal, s))
        return true;

    /* Check if we're offering this file */
    struct file_offer offer;
    if (!file_market_find_offer(chal.root_hash, &offer)) {
        printf("market: challenge for unknown file from peer %s\n",
               node->addr_name);
        return true;
    }

    if (chal.chunk_index >= offer.num_chunks) {
        printf("market: challenge for invalid chunk %u/%u from peer %s\n",
               chal.chunk_index, offer.num_chunks, node->addr_name);
        return true;
    }

    /* Compute SHA3-256 of the chunk data.
     * For now, read chunk from the file on disk and hash it. */
    /* TODO Phase 3: read actual file chunks and hash them.
     * For now, respond with a hash derived from root_hash + chunk_index
     * so the protocol can be tested end-to-end. */
    struct file_proof proof;
    memset(&proof, 0, sizeof(proof));
    memcpy(proof.root_hash, chal.root_hash, 32);
    proof.chunk_index = chal.chunk_index;

    /* Hash: SHA3(root_hash || chunk_index) as placeholder */
    uint8_t preimage[36];
    memcpy(preimage, chal.root_hash, 32);
    memcpy(preimage + 32, &chal.chunk_index, 4);
    struct sha3_256_ctx sha3;
    sha3_256_init(&sha3);
    sha3_256_write(&sha3, preimage, 36);
    sha3_256_finalize(&sha3, proof.chunk_hash);

    struct byte_stream os;
    stream_init(&os, 128);
    file_proof_serialize(&proof, &os);
    p2p_node_begin_message(node, MSG_FILE_PROOF,
                           mp->params->pchMessageStart);
    p2p_node_write_message_data(node, os.data, os.size);
    p2p_node_end_message(node);
    stream_free(&os);

    printf("market: responded to chunk challenge %u for '%s' from peer %s\n",
           chal.chunk_index, offer.filename, node->addr_name);
    return true;
}

static bool handle_zfileproof(struct msg_processor *mp, struct p2p_node *node,
                              struct byte_stream *s)
{
    (void)mp;
    struct file_proof proof;
    if (!file_proof_deserialize(&proof, s))
        return true;

    /* Verify the proof matches our expected hash for this chunk.
     * This is checked by the download session manager. */
    printf("market: received chunk proof %u from peer %s\n",
           proof.chunk_index, node->addr_name);

    /* TODO Phase 3: verify against download session, advance state */
    return true;
}

static bool handle_zfilepay(struct msg_processor *mp, struct p2p_node *node,
                            struct byte_stream *s)
{
    struct file_payment pay;
    if (!file_payment_deserialize(&pay, s))
        return true;

    /* Verify the payment is in our mempool */
    struct uint256 txid_hash;
    memcpy(txid_hash.data, pay.txid, 32);
    bool in_mempool = tx_mempool_exists(mp->mempool, &txid_hash);

    printf("market: payment from peer %s for %u chunks (txid in mempool: %s)\n",
           node->addr_name, pay.chunks_paid,
           in_mempool ? "yes" : "NO");

    if (!in_mempool) {
        printf("market: rejecting payment — txid not found in mempool\n");
        return true;
    }

    /* TODO Phase 3: unlock chunks for this peer's download,
     * track payment state, begin serving file data */
    printf("market: payment verified, unlocking chunks %u-%u\n",
           pay.chunk_start, pay.chunk_start + pay.chunks_paid - 1);
    return true;
}

/* ── Extracted inline handlers ─────────────────────────────────── */

static bool handle_zfileaddr(struct msg_processor *mp, struct p2p_node *node,
                             struct byte_stream *s)
{
    uint8_t faddr[2];
    if (stream_read_bytes(s, faddr, 2)) {
        uint16_t fport;
        memcpy(&fport, faddr, 2);
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
    return true;
}

static bool handle_game_msg(struct msg_processor *mp, struct p2p_node *node,
                            struct byte_stream *s)
{
    (void)mp;
    uint8_t game_type = 0, position = 0;
    struct ttt_state peer_state;
    memset(&peer_state, 0, sizeof(peer_state));
    enum game_action action = game_deserialize(
        s->data + s->read_pos, s->size - s->read_pos,
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
        if (s->size - s->read_pos >= 11) {
            int64_t send_ts = 0;
            memcpy(&send_ts, s->data + s->read_pos + 3, 8);
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
    return true;
}

/* ── BIP152 Compact Block Handlers ─────────────────────────────── */
/* process_sendcmpct, process_cmpctblock, process_getblocktxn,
 * process_blocktxn moved to msg_compact.c */

static bool handle_sendcmpct(struct msg_processor *mp, struct p2p_node *node,
                             struct byte_stream *s)
{
    (void)mp;
    return process_sendcmpct(node, s);
}

static bool handle_cmpctblock(struct msg_processor *mp, struct p2p_node *node,
                              struct byte_stream *s)
{
    return process_cmpctblock(mp, node, s);
}

static bool handle_getblocktxn(struct msg_processor *mp, struct p2p_node *node,
                               struct byte_stream *s)
{
    return process_getblocktxn(mp, node, s);
}

static bool handle_blocktxn(struct msg_processor *mp, struct p2p_node *node,
                            struct byte_stream *s)
{
    return process_blocktxn(mp, node, s);
}

/* Forward declaration — handles all snapshot/chunk/block/flyclient
 * messages. These remain as one function for now because they share
 * complex state (g_swarm, g_block_swarm) and control flow. */
static bool handle_zcl23_sync(struct msg_processor *mp,
                              struct p2p_node *node,
                              struct byte_stream *s,
                              const char *cmd);

/* ── P2P Message Dispatch Table ──────────────────────────────────
 * Maps command strings to handler functions. Replaces the 30-deep
 * strcmp chain. Table is NULL-terminated (empty command). */

static const struct msg_dispatch_entry g_msg_dispatch[] = {
    /* ── Bitcoin P2P ── */
    { "version",      handle_version,      false, false, "p2p" },
    { "verack",       handle_verack,       false, false, "p2p" },
    { "ping",         handle_ping,         true,  false, "p2p" },
    { "pong",         handle_pong,         true,  false, "p2p" },
    { "addr",         handle_addr,         true,  false, "p2p" },
    { "inv",          handle_inv,          true,  false, "sync" },
    { "getdata",      handle_getdata,      true,  false, "sync" },
    { "getblocks",    handle_getblocks,    true,  false, "sync" },
    { "getheaders",   handle_getheaders,   true,  false, "sync" },
    { "block",        handle_block_msg,    true,  false, "sync" },
    { "tx",           handle_tx_msg,       true,  false, "mempool" },
    { "headers",      handle_headers,      true,  false, "sync" },
    { "getaddr",      handle_getaddr,      true,  false, "p2p" },
    { "mempool",      handle_mempool,      true,  false, "mempool" },
    { "sendheaders",  handle_sendheaders,  true,  false, "p2p" },
    { "reject",       handle_reject,       true,  false, "p2p" },
    { "feefilter",    handle_feefilter,    true,  false, "mempool" },
    { "notfound",     handle_notfound,     true,  false, "sync" },
    /* ── BIP152 compact blocks ── */
    { "sendcmpct",   handle_sendcmpct,   true,  false, "compact" },
    { "cmpctblock",  handle_cmpctblock,  true,  false, "compact" },
    { "getblocktxn", handle_getblocktxn, true,  false, "compact" },
    { "blocktxn",    handle_blocktxn,    true,  false, "compact" },
    /* ── BIP37 bloom filters (gated by ZCL_ENABLE_BIP37) ── */
    { "filterload",   handle_filterload,   true,  false, "bloom" },
    { "filteradd",    handle_filteradd,    true,  false, "bloom" },
    { "filterclear",  handle_filterclear,  true,  false, "bloom" },
    /* ── ZCL23 File Service ── */
    { "zfileaddr",    handle_zfileaddr,    true,  true,  "filesvc" },
    /* ── ZCL Messaging ── */
    { "zmsg",         handle_zmsg,         true,  true,  "msg" },
    { "zmsgack",      handle_zmsgack,      true,  true,  "msg" },
    /* ── ZCL Market ── */
    { "zfilelist",    handle_zfilelist,    true,  true,  "market" },
    { "zfilechal",    handle_zfilechal,    true,  true,  "market" },
    { "zfileproof",   handle_zfileproof,   true,  true,  "market" },
    { "zfilepay",     handle_zfilepay,     true,  true,  "market" },
    /* ── ZCL23 Game ── */
    { "zgame",        handle_game_msg,     true,  true,  "game" },
    /* sentinel */
    { "",             NULL,                false, false, NULL }
};

/* Snapshot/chunk/block/flyclient messages are dispatched separately
 * via handle_zcl23_sync() because they share complex state.
 * Commands: zsnapshot, zsnapreq, zsnapdata, zsnapend,
 *           zfcchallenge, zfcproofs, zmanifest, zchunkreq, zchunkdata,
 *           zblkmanfst, zblkreq, zblkdata, zblkbitmap */

const struct msg_dispatch_entry *msg_get_dispatch_table(void)
{
    return g_msg_dispatch;
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

        /* ── Dispatch via table ──────────────────────────────── */
        bool dispatched = false;
        for (const struct msg_dispatch_entry *e = g_msg_dispatch;
             e->handler; e++) {
            if (strcmp(cmd, e->command) != 0)
                continue;

            if (e->requires_handshake && node->version == 0) {
                printf("Peer %s: received %s before version\n",
                       node->addr_name, cmd);
                node->disconnect = true;
                stream_free(&s);
                net_message_free(&msg);
                goto _msg_loop_exit;
            }

            ok = e->handler(mp, node, &s);
            dispatched = true;
            break;
        }

        /* ZCL23 sync messages handled by a combined handler */
        if (!dispatched && cmd[0] == 'z') {
            if (node->version == 0) {
                printf("Peer %s: received %s before version\n",
                       node->addr_name, cmd);
                node->disconnect = true;
                stream_free(&s);
                net_message_free(&msg);
                goto _msg_loop_exit;
            }
            ok = handle_zcl23_sync(mp, node, &s, cmd);
            dispatched = true;
        }

        /* Reject any pre-handshake message not in the table */
        if (!dispatched && node->version == 0) {
            printf("Peer %s: received %s before version\n",
                   node->addr_name, cmd);
            node->disconnect = true;
            stream_free(&s);
            net_message_free(&msg);
            goto _msg_loop_exit;
        }

        stream_free(&s);
        net_message_free(&msg);

        if (!ok) {
            printf("Peer %s: error processing %s\n", node->addr_name, cmd);
        }
    }
    _msg_loop_exit:
    return true;
}

/* ── ZCL23 Sync Message Handler ──────────────────────────────────
 * Handles all snapshot, chunk, block-piece, and FlyClient messages.
 * These share complex state (g_swarm, g_block_swarm) and are kept
 * in one function for clarity. Will be split into individual
 * handlers as the protocol matures. */
static bool handle_zcl23_sync(struct msg_processor *mp,
                              struct p2p_node *node,
                              struct byte_stream *s,
                              const char *cmd)
{
    if (strcmp(cmd, MSG_SNAPSHOT_OFFER) == 0) {
            /* ── Route: zsnapshot → snapsync_handle_offer ──────── */
            struct snapshot_offer_params params;
            if (snapsync_parse_offer_params(&params, s)) {
                struct snapsync_status snap_status = {0};
                params.peer_id = (uint32_t)node->id;
                params.our_height = active_chain_height(
                    &mp->main_state->chain_active);

                {
                    struct snapshot_sync_service *svc =
                        msg_snapshot_sync_ensure(mp);
                    if (svc)
                        snapsync_get_status_snapshot(svc, &snap_status);
                }

                /* Additional gate: once snapshot sync already owns the
                 * receiver lifecycle, duplicate offers should be ignored in
                 * the router instead of trying to re-enter negotiation. */
                if (msg_should_ignore_snapshot_offer(
                        snap_status.state,
                        snap_status.serving_peer_id,
                        node->state,
                        (uint32_t)node->id,
                        sync_get_state())) {
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
                        case SNAPSYNC_OFFER_REJECTED_BLACKLISTED:
                            printf("[snapsync] Rejected offer from %s "
                                   "(peer %u): blacklisted after stall\n",
                                   node->addr_name, (uint32_t)node->id);
                            break;
                        case SNAPSYNC_OFFER_REJECTED_NOT_AHEAD:
                        case SNAPSYNC_OFFER_REJECTED_BUSY:
                            break; /* expected, no log needed */
                        default:
                            break;
                        }
                    }
                }
            }

        } else if (strcmp(cmd, MSG_SNAPSHOT_REQ) == 0) {
            /* ── Route: zsnapreq → snapsync_validate_serve_request ─ */
            int32_t from_h = 0;
            if (!stream_read_i32_le(s, &from_h)) {
                peer_misbehaving(mp->net_mgr, node, 10, "truncated zsnapreq");
                return true; /* skip — caller frees msg */
            }

            /* Pass remaining bytes (PoW data) to controller */
            size_t pow_len = s->size - s->read_pos;
            const uint8_t *pow_data = pow_len > 0 ? s->data + s->read_pos : NULL;
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
                s->data + s->read_pos, s->size - s->read_pos) : -1;
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
                    if (activated_height >= 0) {
                        printf("[snapshot] Chain tip set to height %d\n",
                               activated_height);
                        /* Update in-memory coins view to match snapshot.
                         * snapsync_activate_verified_tip → csr_commit_tip
                         * already set coins_best_block on the singleton's
                         * coins_tip in production. This raw setter stays
                         * as a defensive fallback for the test-harness
                         * path (CSR_REJECTED_NOT_INITIALIZED — csr
                         * singleton not wired), where snapsync's helper
                         * only touches active_chain / pindex_best_header.
                         * Low-level: bypasses csr on purpose. */
                        if (mp->coins_tip) {
                            struct uint256 snap_hash;
                            memcpy(snap_hash.data,
                                   svc->offered_block_hash, 32);
                            coins_view_cache_set_best_block(
                                mp->coins_tip, &snap_hash);
                        }
                    }
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
            if (stream_read_bytes(s, challenge.seed, 32) &&
                stream_read_u64_le(s, &challenge.chain_length) &&
                stream_read_bytes(s, challenge.mmb_root, 32)) {

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
            if (!snapsync_parse_fc_response(&resp, s)) {
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

            if (stream_read_i32_le(s, &height) &&
                stream_read_bytes(s, block_hash, 32) &&
                stream_read_u64_le(s, &num_utxos) &&
                stream_read_u32_le(s, &num_chunks) &&
                stream_read_u32_le(s, &chunk_size) &&
                stream_read_bytes(s, merkle_root, 32)) {

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
            if (!stream_read_u32_le(s, &chunk_index)) {
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
                struct utxo_chunk *chunk = zcl_calloc(1, sizeof(struct utxo_chunk), "utxo_chunk");
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
            if (!stream_read_u32_le(s, &chunk_index) ||
                !stream_read_u32_le(s, &num_entries) ||
                num_entries > 1000) {
                printf("Peer %s: bad zchunkdata header\n", node->addr_name);
                peer_misbehaving(mp->net_mgr, node, 20, "bad zchunkdata");
            } else if (!g_swarm_active) {
                printf("Peer %s: zchunkdata but no swarm active\n",
                       node->addr_name);
            } else {
                struct utxo_chunk *chunk = zcl_calloc(1, sizeof(struct utxo_chunk), "utxo_chunk");
                if (chunk) {
                    chunk->chunk_index = chunk_index;
                    chunk->num_entries = num_entries;
                    bool parse_ok = true;

                    for (uint32_t i = 0; i < num_entries && parse_ok; i++) {
                        if (!stream_read_bytes(s, chunk->entries[i].txid, 32))
                            { parse_ok = false; break; }
                        int32_t vout = 0;
                        if (!stream_read_i32_le(s, &vout))
                            { parse_ok = false; break; }
                        chunk->entries[i].vout = (uint32_t)vout;
                        if (!stream_read_i64_le(s, &chunk->entries[i].value))
                            { parse_ok = false; break; }
                        if (!stream_read_i32_le(s, &chunk->entries[i].height))
                            { parse_ok = false; break; }
                        uint16_t slen = 0;
                        if (!stream_read_u16_le(s, &slen))
                            { parse_ok = false; break; }
                        if (slen > 128) {
                            /* Script too large for entry — reject chunk.
                             * Don't silently truncate, that corrupts UTXOs. */
                            parse_ok = false; break;
                        }
                        chunk->entries[i].script_len = slen;
                        if (slen > 0 &&
                            !stream_read_bytes(s, chunk->entries[i].script, slen))
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

            if (stream_read_i32_le(s, &start_h) &&
                stream_read_i32_le(s, &end_h) &&
                stream_read_u32_le(s, &num_pieces) &&
                stream_read_bytes(s, tip_hash, 32) &&
                stream_read_bytes(s, merkle_root, 32)) {

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
            if (!stream_read_u32_le(s, &piece_index)) {
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
            if (!stream_read_u32_le(s, &piece_index) ||
                !stream_read_u32_le(s, &block_count) ||
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
                uint8_t (*blk_hashes)[32] = zcl_calloc(block_count, 32, "blk_piece_hashes");
                bool parse_ok = true;
                if (blk_hashes) {
                    for (uint32_t i = 0; i < block_count && parse_ok; i++) {
                        if (!stream_read_bytes(s, blk_hashes[i], 32))
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
            if (!stream_read_u32_le(s, &bitmap_len) ||
                bitmap_len == 0 || bitmap_len > 65536) {
                printf("Peer %s: bad zblkbitmap len=%u\n",
                       node->addr_name, bitmap_len);
            } else {
                uint8_t *bitmap = zcl_calloc(bitmap_len, 1, "blk_bitmap");
                if (bitmap && stream_read_bytes(s, bitmap, bitmap_len)) {
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

        }
    return true;
}


/* push_getheaders_from, push_getheaders, exec_getheaders_action
 * moved to msg_headers.c */

bool msg_send_messages(void *ctx, struct p2p_node *node, bool send_trickle)
{
    struct msg_processor *mp = (struct msg_processor *)ctx;
    bool snapshot_active = snapsync_is_active();

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

    /* Snapshot stall detection — if no chunk for 60s, reset and accept new peer */
    if (snapsync_check_stall()) {
        printf("[sync] Snapshot stall detected — will accept new offer\n");
    }

    /* Initiate sync, and periodically re-request if behind. */
    {
        bool should_sync = syncsvc_begin_peer_sync(node);
        struct sync_getheaders_action periodic = {0};

        int our_height = msg_get_height(mp);
        bool in_ibd = syncsvc_is_initial_block_download(node, our_height);
        int64_t now_send = (int64_t)time(NULL);

        /* ── Track pindex_best_header advance for stall detection ── */
        {
            int best_h = mp->main_state->pindex_best_header
                       ? mp->main_state->pindex_best_header->nHeight : 0;
            if (best_h > g_header_stall_last_height) {
                g_header_stall_last_height = best_h;
                g_header_stall_last_advance = now_send;
            }
            if (g_header_stall_last_advance == 0)
                g_header_stall_last_advance = now_send;
        }

        bool header_stall = syncsvc_is_header_sync_stalled(
            sync_get_state(), g_header_stall_last_height,
            g_header_stall_last_advance, now_send);

        /* ── Zero-outbound recovery ──────────────────────────────
         * If we have ONLY inbound peers and are behind, the normal
         * sync path never fires (syncsvc_begin_peer_sync rejects
         * inbound peers) and sync state never reaches
         * SYNC_HEADERS_DOWNLOAD, so stall detection never triggers.
         * Fix: when all peers are inbound and we're behind, force
         * the sync state transition and treat it as a stall so
         * inbound peers can serve headers. */
        if (!header_stall && node->inbound &&
            node->starting_height > our_height + 144) {
            bool have_outbound = false;
            zcl_mutex_lock(&mp->net_mgr->cs_nodes);
            for (size_t pi = 0; pi < mp->net_mgr->num_nodes; pi++) {
                if (!mp->net_mgr->nodes[pi]->inbound &&
                    !mp->net_mgr->nodes[pi]->disconnect) {
                    have_outbound = true;
                    break;
                }
            }
            zcl_mutex_unlock(&mp->net_mgr->cs_nodes);

            if (!have_outbound) {
                enum sync_state ss = sync_get_state();
                if (ss == SYNC_IDLE || ss == SYNC_FINDING_PEERS) {
                    sync_set_state(SYNC_HEADERS_DOWNLOAD,
                                   "no outbound peers, using inbound");
                }
                header_stall = true;
            }
        }

        /* ── Per-peer stale header disconnect (IBD only) ───────── */
        if (syncsvc_should_disconnect_stale_header_peer(node, our_height,
                                                         now_send)) {
            printf("HEADER STALL: peer %s delivered 0 useful headers in "
                   "%llds (total_delivered=%llu), disconnecting\n",
                   node->addr_name,
                   (long long)(now_send - (node->last_useful_headers_time
                       ? node->last_useful_headers_time
                       : node->time_connected)),
                   (unsigned long long)node->total_headers_delivered);
            node->disconnect = true;
            return true;
        }

        /* ── Header sync stall: disconnect worst outbound peer ─── */
        if (header_stall && !node->inbound &&
            node->state >= PEER_SYNCING_HEADERS) {
            /* Log stall once per detection cycle */
            static int64_t last_stall_log = 0;
            if (now_send - last_stall_log >= 30) {
                last_stall_log = now_send;
                printf("HEADER STALL: best_header stuck at %d for %llds, "
                       "disconnecting worst peer\n",
                       g_header_stall_last_height,
                       (long long)(now_send - g_header_stall_last_advance));
            }
            /* Find if this peer is the worst outbound by headers delivered.
             * We disconnect the current peer only if it has the minimum
             * total_headers_delivered. This is an approximation since we
             * see one peer at a time in send_messages — the first worst
             * peer we encounter gets disconnected. */
            static uint64_t worst_delivered = UINT64_MAX;
            static int64_t worst_check_time = 0;
            if (now_send != worst_check_time) {
                worst_delivered = UINT64_MAX;
                worst_check_time = now_send;
            }
            if (node->total_headers_delivered <= worst_delivered) {
                worst_delivered = node->total_headers_delivered;
                printf("HEADER STALL: disconnecting %s "
                       "(total_headers_delivered=%llu)\n",
                       node->addr_name,
                       (unsigned long long)node->total_headers_delivered);
                node->disconnect = true;
                return true;
            }
        }

        /* Re-request headers aggressively during IBD (10s), slower at tip (60s).
         * This is critical: legacy zclassicd sends at most 2000 headers per
         * getheaders response — for a 3M block chain, we need ~1500 rounds. */

        /* Use fallback (inbound peers) during header stall */
        if (header_stall) {
            bool ok = syncsvc_should_request_headers_with_fallback(
                node, our_height, now_send, true);
            if (ok && !snapshot_active) {
                should_sync = true;
                syncsvc_note_headers_requested(node, now_send);
            }
        }
        syncsvc_plan_periodic_getheaders(&periodic, node, our_height, now_send);
        if (periodic.should_send && !snapshot_active) {
            should_sync = true;
            syncsvc_note_headers_requested(node, now_send);
        }
        if (should_sync && !snapshot_active) {
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

        /* Snapshot receive owns catch-up while active. Normal block assignment,
         * stall recovery, and recovery getheaders only add churn until the
         * verified snapshot handoff is complete. */
        if (!snapshot_active) {
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
                        /* Clear re-queued blocks from the "recently seen"
                         * dedup buffer. Without this, blocks that were
                         * received but failed validation are permanently
                         * stuck: stall recovery re-downloads them, but
                         * block_already_seen silently drops them. */
                        for (size_t ri = 0; ri < recovery.alt_count; ri++)
                            block_clear_seen(&recovery.alt_hashes[ri]);
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
        bool is_progress_peer = !node->inbound && node->id == 0;
        bool is_watchdog_peer = !node->inbound;

        if ((is_progress_peer || is_watchdog_peer) &&
            now_prog - last_progress_log >= 60) {
            if (is_progress_peer)
                last_progress_log = now_prog;
            struct download_manager *dm2 = get_download_mgr();
            int h = msg_get_height(mp);
            int header_tip = mp->main_state->pindex_best_header
                ? mp->main_state->pindex_best_header->nHeight : h;
            struct sync_progress_snapshot progress;
            syncsvc_collect_progress(&progress, dm2, sync_get_state(),
                                     h, header_tip, node->last_block_time,
                                     now_prog);
            if (is_progress_peer && progress.should_log_progress) {
                /* Compute blocks/sec and ETA */
                static int64_t ibd_speed_last_time = 0;
                static int     ibd_speed_last_height = 0;
                double blk_per_sec = 0;
                int remaining = progress.header_height - progress.chain_height;
                int eta_seconds = 0;
                if (ibd_speed_last_time > 0) {
                    int64_t dt = now_prog - ibd_speed_last_time;
                    int dh = progress.chain_height - ibd_speed_last_height;
                    if (dt > 0 && dh > 0)
                        blk_per_sec = (double)dh / (double)dt;
                    if (blk_per_sec > 0 && remaining > 0)
                        eta_seconds = (int)((double)remaining / blk_per_sec);
                }
                ibd_speed_last_time = now_prog;
                ibd_speed_last_height = progress.chain_height;

                printf("IBD: h=%d/%d  %.1f blk/s  ETA %dm%ds  "
                       "dl[flight=%llu queue=%llu timeout=%llu]  "
                       "%.2f GB @ %.1f MB/s\n",
                       progress.chain_height, progress.header_height,
                       blk_per_sec, eta_seconds / 60, eta_seconds % 60,
                       (unsigned long long)progress.in_flight,
                       (unsigned long long)progress.queued,
                       (unsigned long long)progress.timed_out,
                       progress.gib_received, progress.mbps_avg);
            }

            /* Tip-stale watchdog: if we're at the tip but haven't
             * received a new block in 10 minutes, re-request headers.
             * Runs on ALL outbound peers, not just id==0, so recovery
             * works even if the first peer disconnected. */
            {
                struct sync_getheaders_action stale = {0};
                syncsvc_plan_tip_stale_getheaders(&stale, &progress,
                                                  node, now_prog);
                if (stale.should_send) {
                    printf("Tip stale: no new block for %llds, "
                           "re-requesting headers from %s\n",
                           (long long)progress.tip_stale_seconds,
                           node->addr_name);
                    exec_getheaders_action(mp, node, &stale);
                }
            }

            /* Sync watchdog: detect and recover from sync stalls.
             * Runs once per 30s cycle on the progress peer only. */
            if (is_progress_peer) {
                struct connman *cm =
                    (struct connman *)mp->net_mgr; /* manager is first field */
                struct download_manager *dm_wd = get_download_mgr();
                sync_watchdog_check(cm, dm_wd, mp->main_state);
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
             * Stop when send buffer exceeds 8MB to avoid unbounded
             * backlog that stalls the receiver.  The receiver's stall
             * detector fires at 120s — we must not queue more than
             * the receiver can process in that window. */
            for (int batch = 0; batch < 200; batch++) {
                if (node->send_size > 8 * 1024 * 1024)
                    break;  /* backpressure: wait for drain */
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

    /* Dandelion++ embargo check: fluff any stemmed txs whose embargo expired.
     * Done once per trickle cycle (not per-peer) via a static timer guard. */
    if (send_trickle && g_dandelion_init && g_dandelion.stempool_count > 0) {
        struct uint256 expired[32];
        int nexp = dandelion_stempool_check_embargo(&g_dandelion, expired, 32);
        if (nexp > 0 && mp->net_mgr) {
            zcl_mutex_lock(&mp->net_mgr->cs_nodes);
            for (int ei = 0; ei < nexp; ei++) {
                struct inv_item einv;
                inv_item_init_typed(&einv, MSG_TX, &expired[ei]);
                for (size_t pi = 0; pi < mp->net_mgr->num_nodes; pi++) {
                    struct p2p_node *peer = mp->net_mgr->nodes[pi];
                    if (peer->state >= PEER_HANDSHAKE_COMPLETE &&
                        !peer->disconnect && peer->relay_txes)
                        p2p_node_push_inventory(peer, &einv);
                }
            }
            zcl_mutex_unlock(&mp->net_mgr->cs_nodes);
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
