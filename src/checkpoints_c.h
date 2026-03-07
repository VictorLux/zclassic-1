/* Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_CHECKPOINTS_C_H
#define ZCL_CHECKPOINTS_C_H

#include "chain_c.h"
#include "uint256.h"
#include <stdbool.h>
#include <stdint.h>

struct checkpoint_entry {
    int height;
    struct uint256 hash;
};

struct checkpoint_data {
    const struct checkpoint_entry *entries;
    int nEntries;
    int64_t nTimeLastCheckpoint;
    int64_t nTransactionsLastCheckpoint;
    double fTransactionsPerDay;
};

int checkpoints_get_total_blocks_estimate(const struct checkpoint_data *data);

double checkpoints_guess_verification_progress(
    const struct checkpoint_data *data,
    const struct block_index *pindex, bool fSigchecks);

#endif
