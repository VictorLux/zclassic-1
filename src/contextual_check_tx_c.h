/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_CONTEXTUAL_CHECK_TX_C_H
#define ZCL_CONTEXTUAL_CHECK_TX_C_H

#include "consensus/params.h"
#include "consensus/validation_c.h"
#include "primitives/transaction_c.h"
#include <stdbool.h>

static inline bool is_expired_tx(const struct transaction *tx, int nHeight)
{
    if (tx->overwintered && tx->expiry_height != 0 &&
        nHeight >= (int)tx->expiry_height)
        return true;
    return false;
}

bool contextual_check_transaction(const struct transaction *tx,
                                   struct validation_state *state,
                                   const struct consensus_params *params,
                                   int nHeight,
                                   int dosLevel);

#endif
