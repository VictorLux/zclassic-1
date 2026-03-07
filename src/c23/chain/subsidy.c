/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "chain/subsidy.h"
#include "consensus/upgrades.h"
#include <assert.h>

int64_t get_block_subsidy(int nHeight, const struct consensus_params *params)
{
    int64_t nSubsidy = (int64_t)(12.5 * COIN);

    if (nHeight < params->nSubsidySlowStartInterval / 2) {
        nSubsidy /= params->nSubsidySlowStartInterval;
        nSubsidy *= nHeight;
        return nSubsidy;
    } else if (nHeight < params->nSubsidySlowStartInterval) {
        nSubsidy /= params->nSubsidySlowStartInterval;
        nSubsidy *= (nHeight + 1);
        return nSubsidy;
    }

    assert(nHeight > consensus_subsidy_slow_start_shift(params));

    int halvings = consensus_halving(params, nHeight);

    if (halvings >= 64)
        return 0;

    if (consensus_network_upgrade_active(params, nHeight, UPGRADE_BUTTERCUP)) {
        return (nSubsidy / BUTTERCUP_POW_TARGET_SPACING_RATIO) >> halvings;
    }

    return nSubsidy >> halvings;
}
