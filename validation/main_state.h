/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_VALIDATION_MAIN_STATE_H
#define ZCL_VALIDATION_MAIN_STATE_H

#include "validation/main_constants.h"
#include "validation/chainstate.h"
#include "validation/txmempool.h"
#include "util/sync.h"
#include <stdbool.h>
#include <stdatomic.h>

struct main_state {
    zcl_mutex_t cs_main;

    struct block_map map_block_index;
    struct active_chain chain_active;
    struct block_index *pindex_best_header;

    int nScriptCheckThreads;

    _Atomic bool fImporting;
    _Atomic bool fReindex;
    bool fTxIndex;
    bool fHavePruned;
    bool fPruneMode;
    bool fIsBareMultisigStd;
    bool fCheckBlockIndex;
    bool fCheckpointsEnabled;
    bool fCoinbaseEnforcedProtectionEnabled;
    bool fAlerts;
    bool fExperimentalMode;
    bool fNoFastSync;

    size_t nCoinCacheUsage;
    uint64_t nPruneTarget;
    int64_t nMaxTipAge;

    uint64_t nLastBlockTx;
    uint64_t nLastBlockSize;

    int64_t nTimeBestReceived;
};

static inline void main_state_init(struct main_state *ms)
{
    zcl_mutex_init(&ms->cs_main);
    block_map_init(&ms->map_block_index);
    active_chain_init(&ms->chain_active);
    ms->pindex_best_header = NULL;
    ms->nScriptCheckThreads = 0;
    atomic_store(&ms->fImporting, false);
    atomic_store(&ms->fReindex, false);
    ms->fTxIndex = false;
    ms->fHavePruned = false;
    ms->fPruneMode = false;
    ms->fIsBareMultisigStd = true;
    ms->fCheckBlockIndex = false;
    ms->fCheckpointsEnabled = true;
    ms->fCoinbaseEnforcedProtectionEnabled = true;
    ms->fAlerts = DEFAULT_ALERTS;
    ms->fExperimentalMode = false;
    ms->fNoFastSync = false;
    ms->nCoinCacheUsage = 5000 * 300;
    ms->nPruneTarget = 0;
    ms->nMaxTipAge = DEFAULT_MAX_TIP_AGE;
    ms->nLastBlockTx = 0;
    ms->nLastBlockSize = 0;
    ms->nTimeBestReceived = 0;
}

static inline void main_state_free(struct main_state *ms)
{
    block_map_free(&ms->map_block_index);
    active_chain_free(&ms->chain_active);
    zcl_mutex_destroy(&ms->cs_main);
}

#endif
