/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_MINER_H
#define ZCL_MINER_H

#include "chain/chain.h"
#include "chain/chainparams.h"
#include "coins/coins_view.h"
#include "consensus/validation.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "validation/main_state.h"
#include "validation/txmempool.h"
#include <stdbool.h>
#include <stdint.h>

struct block_template {
    struct block block;
    int64_t *tx_fees;
    unsigned int *tx_sig_ops;
    size_t num_entries;
};

void block_template_init(struct block_template *bt);
void block_template_free(struct block_template *bt);

struct block_template *create_new_block(const struct script *coinbase_script,
                                         struct main_state *ms,
                                         struct coins_view_cache *coins_tip,
                                         struct tx_mempool *mempool,
                                         const struct chain_params *params);

void increment_extra_nonce(struct block *pblock,
                           struct block_index *pindex_prev,
                           unsigned int *extra_nonce);

#endif
