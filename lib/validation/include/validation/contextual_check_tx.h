/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_CONTEXTUAL_CHECK_TX_H
#define ZCL_CONTEXTUAL_CHECK_TX_H

#include "consensus/params.h"
#include "consensus/validation.h"
#include "primitives/transaction.h"
#include <stdbool.h>

#define TX_EXPIRING_SOON_THRESHOLD 3

static inline bool is_expired_tx(const struct transaction *tx, int nHeight)
{
    if (tx->expiry_height == 0 || transaction_is_coinbase(tx))
        return false;
    return (uint32_t)nHeight >= tx->expiry_height;
}

static inline bool is_expiring_soon_tx(const struct transaction *tx,
                                       int nNextBlockHeight)
{
    return is_expired_tx(tx, nNextBlockHeight + TX_EXPIRING_SOON_THRESHOLD);
}

#define LOCKTIME_THRESHOLD_TX 500000000

static inline bool is_final_tx(const struct transaction *tx,
                                int nBlockHeight, int64_t nBlockTime)
{
    if (tx->lock_time == 0)
        return true;
    int64_t lt = (int64_t)tx->lock_time;
    if (lt < (lt < LOCKTIME_THRESHOLD_TX ? (int64_t)nBlockHeight : nBlockTime))
        return true;
    for (size_t i = 0; i < tx->num_vin; i++) {
        if (!tx_in_is_final(&tx->vin[i]))
            return false;
    }
    return true;
}

bool contextual_check_transaction(const struct transaction *tx,
                                   struct validation_state *state,
                                   const struct consensus_params *params,
                                   int nHeight,
                                   int dosLevel);

/* Skip Groth16 proof verification for blocks at or below this height.
 * Set via -assumevalid=<hash>. Default: latest checkpoint height.
 * Value of -1 disables (verify everything). */
extern int g_assume_valid_height;

#endif
