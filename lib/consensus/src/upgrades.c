/* Copyright (c) 2018 The Zcash developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "consensus/upgrades.h"
#include "util/log_macros.h"
#include <assert.h>

const struct nu_info NetworkUpgradeInfo[MAX_NETWORK_UPGRADES] = {
    { 0,          "Sprout",      "The Zclassic network at launch" },
    { 0x74736554, "Test dummy",  "Test dummy info" },
    { 0x5ba81b19, "Overwinter",  "See https://z.cash/upgrade/overwinter.html for details." },
    { 0x76b809bb, "Sapling",     "See https://z.cash/upgrade/sapling.html for details." },
    { 0x821a451c, "Bubbles",     "See ZClassic for details." },
    { 0x930b540d, "Bubbly",      "See ZClassic for details." },
    { 0x930b540d, "Buttercup",   "See ZClassic for details." },
};

const uint32_t SPROUT_BRANCH_ID = 0;

struct equihash_info EquihashUpgradeInfo[MAX_NETWORK_UPGRADES] = {
    { EQUIHASH_DEFAULT_PARAMS, EQUIHASH_DEFAULT_PARAMS },
    { EQUIHASH_DEFAULT_PARAMS, EQUIHASH_DEFAULT_PARAMS },
    { EQUIHASH_DEFAULT_PARAMS, EQUIHASH_DEFAULT_PARAMS },
    { EQUIHASH_DEFAULT_PARAMS, EQUIHASH_DEFAULT_PARAMS },
    { 192, 7 },
    { 192, 7 },
    { 192, 7 },
};

enum upgrade_state consensus_upgrade_state(int nHeight, const struct consensus_params *params,
                                            enum upgrade_index idx)
{
    assert(nHeight >= 0);
    assert(idx >= BASE_SPROUT && idx < MAX_NETWORK_UPGRADES);
    int activation = params->vUpgrades[idx].nActivationHeight;

    if (activation == NETWORK_UPGRADE_NO_ACTIVATION)
        return UPGRADE_DISABLED;
    else if (nHeight >= activation)
        return UPGRADE_ACTIVE;
    else
        return UPGRADE_PENDING;
}

int consensus_current_epoch(int nHeight, const struct consensus_params *params)
{
    for (int i = MAX_NETWORK_UPGRADES - 1; i >= BASE_SPROUT; i--) {
        if (consensus_network_upgrade_active(params, nHeight, (enum upgrade_index)i))
            return i;
    }
    return BASE_SPROUT;
}

uint32_t consensus_current_epoch_branch_id(int nHeight, const struct consensus_params *params)
{
    return NetworkUpgradeInfo[consensus_current_epoch(nHeight, params)].nBranchId;
}

bool consensus_is_branch_id(int branchId)
{
    for (int i = BASE_SPROUT; i < MAX_NETWORK_UPGRADES; i++) {
        if ((uint32_t)branchId == NetworkUpgradeInfo[i].nBranchId)
            return true;
    }
    LOG_FAIL("consensus", "unrecognized branch id 0x%08x", branchId);
}

bool consensus_is_activation_height(int nHeight, const struct consensus_params *params,
                                     enum upgrade_index idx)
{
    assert(idx >= BASE_SPROUT && idx < MAX_NETWORK_UPGRADES);
    if (idx == BASE_SPROUT)
        LOG_FAIL("consensus", "BASE_SPROUT has no activation height");
    return nHeight >= 0 && nHeight == params->vUpgrades[idx].nActivationHeight;
}

bool consensus_is_activation_height_any(int nHeight, const struct consensus_params *params)
{
    if (nHeight < 0)
        LOG_FAIL("consensus", "is_activation_height_any: negative height %d", nHeight);
    for (int i = BASE_SPROUT + 1; i < MAX_NETWORK_UPGRADES; i++) {
        if (nHeight == params->vUpgrades[i].nActivationHeight)
            return true;
    }
    LOG_FAIL("consensus", "height %d is not an activation height for any upgrade", nHeight);
}

int consensus_next_epoch(int nHeight, const struct consensus_params *params)
{
    if (nHeight < 0)
        LOG_ERR("consensus", "next_epoch: negative height %d", nHeight);
    for (int i = BASE_SPROUT + 1; i < MAX_NETWORK_UPGRADES; i++) {
        if (consensus_upgrade_state(nHeight, params, (enum upgrade_index)i) == UPGRADE_PENDING)
            return i;
    }
    LOG_ERR("consensus", "next_epoch: no pending upgrade at height %d", nHeight);
}

int consensus_next_activation_height(int nHeight, const struct consensus_params *params)
{
    int idx = consensus_next_epoch(nHeight, params);
    if (idx >= 0)
        return params->vUpgrades[idx].nActivationHeight;
    LOG_ERR("consensus", "next_activation_height: no next epoch at height %d", nHeight);
}
