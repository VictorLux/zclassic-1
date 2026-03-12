/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_EQUIHASH_H
#define ZCL_EQUIHASH_H

#include "chain/chainparams.h"
#include "primitives/block.h"
#include <stdbool.h>

bool check_equihash_solution(const struct block_header *header,
                             const struct chain_params *params);

#endif
