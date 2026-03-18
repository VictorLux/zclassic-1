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
#include <stdbool.h>

struct msg_processor {
    struct main_state *main_state;
    struct tx_mempool *mempool;
    struct coins_view_cache *coins_tip;
    const struct chain_params *params;
    const char *datadir;
    struct net_manager *net_mgr;
};

void msg_processor_init(struct msg_processor *mp,
                         struct main_state *ms,
                         struct tx_mempool *mempool,
                         struct coins_view_cache *coins_tip,
                         const struct chain_params *params,
                         const char *datadir,
                         struct net_manager *net_mgr);

bool msg_process_messages(void *ctx, struct p2p_node *node);
bool msg_send_messages(void *ctx, struct p2p_node *node, bool send_trickle);
int msg_get_height(void *ctx);

/* Test helpers for block relay deduplication. */
#include "core/uint256.h"
bool msgprocessor_test_block_already_seen(const struct uint256 *hash);
void msgprocessor_test_block_mark_seen(const struct uint256 *hash);
void msgprocessor_test_reset_recent_blocks(void);
int msgprocessor_test_get_recent_block_count(void);

#endif
