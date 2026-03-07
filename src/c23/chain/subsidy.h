/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_SUBSIDY_H
#define ZCL_SUBSIDY_H

#include "core/amount.h"
#include "consensus/params.h"
#include <stdint.h>

int64_t get_block_subsidy(int nHeight, const struct consensus_params *params);

#endif
