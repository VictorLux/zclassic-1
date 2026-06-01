/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_NET_MSGPROCESSOR_H
#define ZCL_NET_MSGPROCESSOR_H

#include "net/net.h"
#include "validation/main_state.h"
#include "validation/txmempool.h"
#include "coins/coins_view.h"
#include "chain/chainparams.h"
#include "config/runtime.h"
#include "net/fast_sync.h"
#include "event/event.h"
#include <stdbool.h>

struct block;
struct transaction;
struct validation_state;
struct active_chain;
struct fc_challenge;
struct fc_response;

typedef bool (*msg_compact_block_submit_fn)(struct block *block,
                                            struct validation_state *out,
                                            void *ctx);
typedef bool (*msg_block_submit_fn)(struct block *block,
                                    struct validation_state *out,
                                    void *ctx);
typedef void (*msg_peer_save_fn)(const struct p2p_node *node, void *ctx);
typedef bool (*msg_snapshot_active_fn)(void *ctx);
typedef void (*msg_wallet_tx_accepted_fn)(const struct transaction *tx,
                                          void *ctx);
typedef void (*msg_block_connected_fn)(int height, void *ctx);
typedef bool (*msg_flyclient_proof_fn)(
    struct fc_response *resp,
    const struct fc_challenge *challenge,
    const struct active_chain *chain_active,
    void *ctx);

struct msg_processor {
    struct main_state *main_state;
    struct tx_mempool *mempool;
    struct coins_view_cache *coins_tip;
    const struct chain_params *params;
    const char *datadir;
    struct net_manager *net_mgr;
    const struct app_runtime_context *runtime;
    msg_block_submit_fn block_submit;
    void *block_submit_ctx;
    msg_compact_block_submit_fn compact_block_submit;
    void *compact_block_submit_ctx;
    msg_peer_save_fn peer_save;
    void *peer_save_ctx;
    msg_snapshot_active_fn snapshot_active;
    void *snapshot_active_ctx;
    msg_wallet_tx_accepted_fn wallet_tx_accepted;
    void *wallet_tx_accepted_ctx;
    msg_block_connected_fn block_connected;
    void *block_connected_ctx;
    msg_flyclient_proof_fn flyclient_proof;
    void *flyclient_proof_ctx;
};

/* ── P2P message dispatch table ──────────────────────────────────
 * Each entry maps a P2P command string to a handler function.
 * The dispatch table replaces the strcmp chain in msg_process_messages.
 * Handlers return true on success, false on protocol error. */

struct byte_stream;  /* forward decl */

typedef bool (*msg_handler_fn)(struct msg_processor *mp,
                               struct p2p_node *node,
                               struct byte_stream *s);

struct msg_dispatch_entry {
    char command[13];           /* P2P command (max 12 bytes + NUL) */
    msg_handler_fn handler;
    bool requires_handshake;   /* must have completed version/verack? */
    bool zcl23_only;           /* requires NODE_ZCL23 service bit? */
    const char *service_name;  /* for logging: "p2p", "sync", "game", etc. */
};

/* Get the dispatch table (NULL-terminated). For testing. */
const struct msg_dispatch_entry *msg_get_dispatch_table(void);

void msg_processor_init(struct msg_processor *mp,
                         struct main_state *ms,
                         struct tx_mempool *mempool,
                         struct coins_view_cache *coins_tip,
                         const struct chain_params *params,
                         const char *datadir,
                         struct net_manager *net_mgr,
                         const struct app_runtime_context *runtime);

void msg_processor_set_compact_block_submit(
    struct msg_processor *mp,
    msg_compact_block_submit_fn submit,
    void *ctx);
void msg_processor_set_block_submit(struct msg_processor *mp,
                                    msg_block_submit_fn submit,
                                    void *ctx);
void msg_processor_set_peer_save(struct msg_processor *mp,
                                 msg_peer_save_fn save,
                                 void *ctx);
void msg_processor_set_snapshot_active(struct msg_processor *mp,
                                       msg_snapshot_active_fn active,
                                       void *ctx);
void msg_processor_set_wallet_tx_accepted(
    struct msg_processor *mp,
    msg_wallet_tx_accepted_fn accepted,
    void *ctx);
void msg_processor_set_block_connected(
    struct msg_processor *mp,
    msg_block_connected_fn connected,
    void *ctx);
void msg_processor_set_flyclient_proof_builder(
    struct msg_processor *mp,
    msg_flyclient_proof_fn build,
    void *ctx);

bool msg_process_messages(void *ctx, struct p2p_node *node);
bool msg_send_messages(void *ctx, struct p2p_node *node, bool send_trickle);
int msg_get_height(void *ctx);

/* Update the cached snapshot offer (thread-safe). Called from boot.c. */
struct snapshot_offer;
void msg_processor_update_offer(const struct snapshot_offer *offer);
bool msg_processor_get_offer(struct snapshot_offer *offer);
void msg_processor_invalidate_offer(void);
uint64_t msg_processor_offer_cache_version(void);

/* Publish or invalidate cached fast-sync artifacts.
 * Ownership of heap-backed arrays transfers on successful publish.
 * Caller must provide internally consistent manifests:
 * - sync manifest: num_chunks > 0, chunk_size > 0, non-NULL chunk_hashes
 * - block manifest: start_height <= end_height, num_pieces > 0,
 *   non-NULL piece_hashes */
bool msg_processor_publish_manifest(struct sync_manifest *manifest);
void msg_processor_invalidate_manifest(void);
bool msg_processor_get_manifest_header(struct sync_manifest *out);
uint64_t msg_processor_manifest_cache_version(void);

/* Deep-copy the cached manifest's chunk_hashes array so MSG_MANIFEST can
 * be serialized on the wire without holding g_manifest_mutex through the
 * socket write. On success *out_hashes is heap-allocated (caller frees)
 * and *out_count is the number of 32-byte hashes. Returns false if no
 * manifest is published or allocation fails. */
bool msg_processor_copy_manifest_hashes(uint8_t (**out_hashes)[32],
                                        uint32_t *out_count);

bool msg_processor_publish_block_manifest(struct block_piece_manifest *manifest,
                                         int32_t built_at_height);
void msg_processor_invalidate_block_manifest(void);
bool msg_processor_get_block_manifest_header(struct block_piece_manifest *out,
                                            int32_t *built_at_height);
uint64_t msg_processor_block_manifest_cache_version(void);

#include "core/uint256.h"

/* per-peer FlyClient challenge rate-limit tuning. See
 * msgprocessor.c for the full rationale — short version: each
 * zfcchallenge is expensive (50 MMB proofs), so we cap per-peer
 * consumption at BURST on first use and refill at RATE_PER_SEC. A
 * legitimate IBD peer needs one token per snapshot offer; a flood
 * burns through the burst, then drops silently. */
#define FC_CHALLENGE_RATE_PER_SEC 10u
#define FC_CHALLENGE_BURST        30u

/* test hooks: drive the FlyClient challenge rate limiter with an
 * explicit clock, read dropped-challenge telemetry, and reset the table
 * between cases. Not intended for production call-sites. */
bool msgprocessor_test_fc_rate_acquire(node_id_t peer_id, int64_t now_ms);
uint32_t msgprocessor_test_fc_rate_dropped(node_id_t peer_id);
bool msgprocessor_test_fc_rate_should_score(node_id_t peer_id);
void msgprocessor_test_fc_rate_reset(void);

/* test hooks: drive the g_swarm_active atomic CAS used by the
 * zmanifest handler. try_claim returns true exactly once until
 * release() is called; concurrent callers see at most one success. */
bool msgprocessor_test_swarm_try_claim(void);
void msgprocessor_test_swarm_release(void);
bool msgprocessor_test_swarm_is_active(void);

/* test hook: swap the allocator process_mempool uses for its
 * scratch hash buffer. Pass NULL to restore the default zcl_malloc
 * path; pass a function that returns NULL to simulate OOM. Only
 * influences process_mempool — no global malloc override. */
void msgprocessor_test_set_mempool_alloc_hook(void *(*hook)(size_t));

/* Test helpers for block relay deduplication. */
bool msgprocessor_test_block_already_seen(const struct uint256 *hash);
void msgprocessor_test_block_mark_seen(const struct uint256 *hash);
bool msgprocessor_test_accept_block_for_processing(const struct uint256 *hash,
                                                   bool snapshot_active);
bool msgprocessor_test_should_ignore_snapshot_offer(
    enum snapshot_sync_state snapsync_state,
    uint32_t serving_peer_id,
    enum peer_state peer_state,
    uint32_t peer_id,
    enum sync_state sync_state);
void msgprocessor_test_reset_recent_blocks(void);
int msgprocessor_test_get_recent_block_count(void);

/* ── Header sync diagnostic counters (msg_headers.c) ─────────── */

struct msg_headers_stats {
    uint64_t batches_received;
    uint64_t total_accepted;
    uint64_t total_rejected;
    uint64_t newly_added;
    uint64_t already_known;
};

void msg_headers_get_stats(struct msg_headers_stats *out);

#endif
