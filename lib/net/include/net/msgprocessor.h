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
#include <stdbool.h>

struct msg_processor {
    struct main_state *main_state;
    struct tx_mempool *mempool;
    struct coins_view_cache *coins_tip;
    const struct chain_params *params;
    const char *datadir;
    struct net_manager *net_mgr;
    const struct app_runtime_context *runtime;
};

void msg_processor_init(struct msg_processor *mp,
                         struct main_state *ms,
                         struct tx_mempool *mempool,
                         struct coins_view_cache *coins_tip,
                         const struct chain_params *params,
                         const char *datadir,
                         struct net_manager *net_mgr,
                         const struct app_runtime_context *runtime);

bool msg_process_messages(void *ctx, struct p2p_node *node);
bool msg_send_messages(void *ctx, struct p2p_node *node, bool send_trickle);
int msg_get_height(void *ctx);

/* Update the cached snapshot offer (thread-safe). Called from boot.c. */
struct snapshot_offer;
void msg_processor_update_offer(const struct snapshot_offer *offer);
bool msg_processor_get_offer(struct snapshot_offer *offer);
void msg_processor_invalidate_offer(void);

/* Publish or invalidate cached fast-sync artifacts. These functions take
 * ownership of the heap-backed arrays inside the provided manifests. */
bool msg_processor_publish_manifest(struct sync_manifest *manifest);
void msg_processor_invalidate_manifest(void);
bool msg_processor_get_manifest_header(struct sync_manifest *out);

bool msg_processor_publish_block_manifest(struct block_piece_manifest *manifest,
                                         int32_t built_at_height);
void msg_processor_invalidate_block_manifest(void);
bool msg_processor_get_block_manifest_header(struct block_piece_manifest *out,
                                            int32_t *built_at_height);

/* Test helpers for block relay deduplication. */
#include "core/uint256.h"
bool msgprocessor_test_block_already_seen(const struct uint256 *hash);
void msgprocessor_test_block_mark_seen(const struct uint256 *hash);
void msgprocessor_test_reset_recent_blocks(void);
int msgprocessor_test_get_recent_block_count(void);

#endif
