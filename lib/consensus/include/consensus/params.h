/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef BITCOIN_CONSENSUS_PARAMS_H
#define BITCOIN_CONSENSUS_PARAMS_H

#include "core/uint256.h"
#include <stdbool.h>
#include <stdint.h>

enum upgrade_index {
    BASE_SPROUT = 0,
    UPGRADE_TESTDUMMY,
    UPGRADE_OVERWINTER,
    UPGRADE_SAPLING,
    UPGRADE_BUBBLES,
    UPGRADE_DIFFADJ,
    UPGRADE_BUTTERCUP,
    MAX_NETWORK_UPGRADES
};

#define NETWORK_UPGRADE_ALWAYS_ACTIVE 0
#define NETWORK_UPGRADE_NO_ACTIVATION (-1)

struct network_upgrade {
    int nProtocolVersion;
    int nActivationHeight;
};

#define PRE_BUTTERCUP_POW_TARGET_SPACING 150
#define POST_BUTTERCUP_POW_TARGET_SPACING 75
#define PRE_BUTTERCUP_HALVING_INTERVAL 840000
#define PRE_BUTTERCUP_REGTEST_HALVING_INTERVAL 150
#define BUTTERCUP_POW_TARGET_SPACING_RATIO (PRE_BUTTERCUP_POW_TARGET_SPACING / POST_BUTTERCUP_POW_TARGET_SPACING)
#define POST_BUTTERCUP_HALVING_INTERVAL (PRE_BUTTERCUP_HALVING_INTERVAL * BUTTERCUP_POW_TARGET_SPACING_RATIO)
#define POST_BUTTERCUP_REGTEST_HALVING_INTERVAL (PRE_BUTTERCUP_REGTEST_HALVING_INTERVAL * BUTTERCUP_POW_TARGET_SPACING_RATIO)

struct consensus_params {
    struct uint256 hashGenesisBlock;
    bool fCoinbaseMustBeProtected;
    int nSubsidySlowStartInterval;
    int nPreButtercupSubsidyHalvingInterval;
    int nPostButtercupSubsidyHalvingInterval;
    int nMajorityEnforceBlockUpgrade;
    int nMajorityRejectBlockOutdated;
    int nMajorityWindow;
    struct network_upgrade vUpgrades[MAX_NETWORK_UPGRADES];
    struct uint256 powLimit;
    int32_t nPowAllowMinDifficultyBlocksAfterHeight; /* -1 = disabled */
    bool nPowAllowMinDifficultyEnabled;
    bool scaleDifficultyAtUpgradeFork;
    int64_t nPowAveragingWindow;
    int64_t nPowMaxAdjustDown;
    int64_t nPowMaxAdjustUp;
    int64_t nPreButtercupPowTargetSpacing;
    int64_t nPostButtercupPowTargetSpacing;
    struct uint256 nMinimumChainWork;
};

static inline int consensus_subsidy_slow_start_shift(const struct consensus_params *p)
{
    return p->nSubsidySlowStartInterval / 2;
}

bool consensus_network_upgrade_active(const struct consensus_params *params, int nHeight, enum upgrade_index idx);
int consensus_halving(const struct consensus_params *params, int nHeight);
int64_t consensus_pow_target_spacing(const struct consensus_params *params, int nHeight);
int64_t consensus_averaging_window_timespan(const struct consensus_params *params, int nHeight);
int64_t consensus_min_actual_timespan(const struct consensus_params *params, int nHeight);
int64_t consensus_max_actual_timespan(const struct consensus_params *params, int nHeight);

static inline int consensus_last_founders_reward_height(const struct consensus_params *p)
{
    return p->nPreButtercupSubsidyHalvingInterval + consensus_subsidy_slow_start_shift(p) - 1;
}

#endif
