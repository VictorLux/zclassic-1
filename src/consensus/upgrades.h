/* Copyright (c) 2018 The Zcash developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCASH_CONSENSUS_UPGRADES_H
#define ZCASH_CONSENSUS_UPGRADES_H

#include "consensus/params.h"
#include <stdbool.h>
#include <stdint.h>

enum upgrade_state {
    UPGRADE_DISABLED,
    UPGRADE_PENDING,
    UPGRADE_ACTIVE
};

struct nu_info {
    uint32_t nBranchId;
    const char *strName;
    const char *strInfo;
};

extern const struct nu_info NetworkUpgradeInfo[];
extern const uint32_t SPROUT_BRANCH_ID;

#define EQUIHASH_DEFAULT_PARAMS 0

struct equihash_info {
    unsigned int N;
    unsigned int K;
};

extern struct equihash_info EquihashUpgradeInfo[];

enum upgrade_state consensus_upgrade_state(int nHeight, const struct consensus_params *params, enum upgrade_index idx);
int consensus_current_epoch(int nHeight, const struct consensus_params *params);
uint32_t consensus_current_epoch_branch_id(int nHeight, const struct consensus_params *params);
bool consensus_is_branch_id(int branchId);
bool consensus_is_activation_height(int nHeight, const struct consensus_params *params, enum upgrade_index idx);
bool consensus_is_activation_height_any(int nHeight, const struct consensus_params *params);
int consensus_next_epoch(int nHeight, const struct consensus_params *params);
int consensus_next_activation_height(int nHeight, const struct consensus_params *params);

#endif
