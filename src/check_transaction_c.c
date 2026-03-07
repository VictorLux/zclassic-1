/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "check_transaction_c.h"
#include "amount.h"
#include "consensus/consensus.h"

bool check_transaction(const struct transaction *tx,
                       struct validation_state *state)
{
    if (!tx->overwintered && tx->version < SPROUT_MIN_TX_VERSION) {
        return validation_state_dos(state, 100, false, REJECT_INVALID,
                                    "bad-txns-version-too-low", false, NULL);
    }

    if (tx->overwintered) {
        if (tx->version < OVERWINTER_MIN_TX_VERSION) {
            return validation_state_dos(state, 100, false, REJECT_INVALID,
                                        "bad-tx-overwinter-version-too-low",
                                        false, NULL);
        }
        if (tx->version_group_id != OVERWINTER_VERSION_GROUP_ID &&
            tx->version_group_id != SAPLING_VERSION_GROUP_ID) {
            return validation_state_dos(state, 100, false, REJECT_INVALID,
                                        "bad-tx-version-group-id",
                                        false, NULL);
        }
        if (tx->expiry_height >= TX_EXPIRY_HEIGHT_THRESHOLD) {
            return validation_state_dos(state, 100, false, REJECT_INVALID,
                                        "bad-tx-expiry-height-too-high",
                                        false, NULL);
        }
    }

    if (tx->num_vin == 0) {
        return validation_state_dos(state, 10, false, REJECT_INVALID,
                                    "bad-txns-vin-empty", false, NULL);
    }
    if (tx->num_vout == 0) {
        return validation_state_dos(state, 10, false, REJECT_INVALID,
                                    "bad-txns-vout-empty", false, NULL);
    }

    int64_t value_out = 0;
    for (size_t i = 0; i < tx->num_vout; i++) {
        if (tx->vout[i].value < 0) {
            return validation_state_dos(state, 100, false, REJECT_INVALID,
                                        "bad-txns-vout-negative", false, NULL);
        }
        if (tx->vout[i].value > MAX_MONEY) {
            return validation_state_dos(state, 100, false, REJECT_INVALID,
                                        "bad-txns-vout-toolarge", false, NULL);
        }
        value_out += tx->vout[i].value;
        if (!MoneyRange(value_out)) {
            return validation_state_dos(state, 100, false, REJECT_INVALID,
                                        "bad-txns-txouttotal-toolarge",
                                        false, NULL);
        }
    }

    /* Check for duplicate inputs */
    for (size_t i = 0; i < tx->num_vin; i++) {
        for (size_t j = i + 1; j < tx->num_vin; j++) {
            if (outpoint_cmp(&tx->vin[i].prevout, &tx->vin[j].prevout) == 0) {
                return validation_state_dos(state, 100, false, REJECT_INVALID,
                                            "bad-txns-inputs-duplicate",
                                            false, NULL);
            }
        }
    }

    if (transaction_is_coinbase(tx)) {
        if (tx->vin[0].script_sig.size < 2 ||
            tx->vin[0].script_sig.size > 100) {
            return validation_state_dos(state, 100, false, REJECT_INVALID,
                                        "bad-cb-length", false, NULL);
        }
    } else {
        for (size_t i = 0; i < tx->num_vin; i++) {
            if (outpoint_is_null(&tx->vin[i].prevout)) {
                return validation_state_dos(state, 10, false, REJECT_INVALID,
                                            "bad-txns-prevout-null",
                                            false, NULL);
            }
        }
    }

    return true;
}
