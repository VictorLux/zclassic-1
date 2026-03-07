/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "validation/check_transaction.h"
#include "core/amount.h"
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

    /* vin may be empty if joinsplits or shielded spends exist */
    if (tx->num_vin == 0 && tx->num_joinsplit == 0 &&
        tx->num_shielded_spend == 0) {
        return validation_state_dos(state, 10, false, REJECT_INVALID,
                                    "bad-txns-vin-empty", false, NULL);
    }
    /* vout may be empty if joinsplits or shielded outputs exist */
    if (tx->num_vout == 0 && tx->num_joinsplit == 0 &&
        tx->num_shielded_output == 0) {
        return validation_state_dos(state, 10, false, REJECT_INVALID,
                                    "bad-txns-vout-empty", false, NULL);
    }

    /* Size limits (post-Sapling max) */
    size_t tx_size = transaction_serialize_size(tx);
    if (tx_size > MAX_TX_SIZE_AFTER_SAPLING) {
        return validation_state_dos(state, 100, false, REJECT_INVALID,
                                    "bad-txns-oversize", false, NULL);
    }

    /* Check for negative or overflow output values */
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

    /* Check for non-zero valueBalance when there are no Sapling inputs or outputs */
    if (tx->num_shielded_spend == 0 && tx->num_shielded_output == 0 &&
        tx->value_balance != 0) {
        return validation_state_dos(state, 100, false, REJECT_INVALID,
                                    "bad-txns-valuebalance-nonzero",
                                    false, NULL);
    }

    /* Check for overflow valueBalance */
    if (tx->value_balance > MAX_MONEY || tx->value_balance < -MAX_MONEY) {
        return validation_state_dos(state, 100, false, REJECT_INVALID,
                                    "bad-txns-valuebalance-toolarge",
                                    false, NULL);
    }

    /* Negative valueBalance takes from transparent pool like outputs */
    if (tx->value_balance <= 0) {
        value_out += -tx->value_balance;
        if (!MoneyRange(value_out)) {
            return validation_state_dos(state, 100, false, REJECT_INVALID,
                                        "bad-txns-txouttotal-toolarge",
                                        false, NULL);
        }
    }

    /* Ensure that joinsplit values are well-formed */
    for (size_t i = 0; i < tx->num_joinsplit; i++) {
        const struct js_description *js = &tx->v_joinsplit[i];
        if (js->vpub_old < 0) {
            return validation_state_dos(state, 100, false, REJECT_INVALID,
                                        "bad-txns-vpub_old-negative",
                                        false, NULL);
        }
        if (js->vpub_new < 0) {
            return validation_state_dos(state, 100, false, REJECT_INVALID,
                                        "bad-txns-vpub_new-negative",
                                        false, NULL);
        }
        if (js->vpub_old > MAX_MONEY) {
            return validation_state_dos(state, 100, false, REJECT_INVALID,
                                        "bad-txns-vpub_old-toolarge",
                                        false, NULL);
        }
        if (js->vpub_new > MAX_MONEY) {
            return validation_state_dos(state, 100, false, REJECT_INVALID,
                                        "bad-txns-vpub_new-toolarge",
                                        false, NULL);
        }
        if (js->vpub_new != 0 && js->vpub_old != 0) {
            return validation_state_dos(state, 100, false, REJECT_INVALID,
                                        "bad-txns-vpubs-both-nonzero",
                                        false, NULL);
        }
        value_out += js->vpub_old;
        if (!MoneyRange(value_out)) {
            return validation_state_dos(state, 100, false, REJECT_INVALID,
                                        "bad-txns-txouttotal-toolarge",
                                        false, NULL);
        }
    }

    /* Ensure input values do not exceed MAX_MONEY */
    {
        int64_t value_in = 0;
        for (size_t i = 0; i < tx->num_joinsplit; i++) {
            value_in += tx->v_joinsplit[i].vpub_new;
            if (!MoneyRange(tx->v_joinsplit[i].vpub_new) ||
                !MoneyRange(value_in)) {
                return validation_state_dos(state, 100, false, REJECT_INVALID,
                                            "bad-txns-txintotal-toolarge",
                                            false, NULL);
            }
        }
        /* Positive valueBalance adds to transparent pool like inputs */
        if (tx->value_balance >= 0) {
            value_in += tx->value_balance;
            if (!MoneyRange(value_in)) {
                return validation_state_dos(state, 100, false, REJECT_INVALID,
                                            "bad-txns-txintotal-toolarge",
                                            false, NULL);
            }
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

    /* Check for duplicate joinsplit nullifiers */
    for (size_t i = 0; i < tx->num_joinsplit; i++) {
        for (int ni = 0; ni < ZC_NUM_JS_INPUTS; ni++) {
            const struct uint256 *nf = &tx->v_joinsplit[i].nullifiers[ni];
            for (size_t j = 0; j < i; j++) {
                for (int nj = 0; nj < ZC_NUM_JS_INPUTS; nj++) {
                    if (uint256_cmp(nf, &tx->v_joinsplit[j].nullifiers[nj]) == 0) {
                        return validation_state_dos(state, 100, false,
                            REJECT_INVALID,
                            "bad-joinsplits-nullifiers-duplicate",
                            false, NULL);
                    }
                }
            }
            /* Check within same joinsplit */
            for (int nj = 0; nj < ni; nj++) {
                if (uint256_cmp(nf, &tx->v_joinsplit[i].nullifiers[nj]) == 0) {
                    return validation_state_dos(state, 100, false,
                        REJECT_INVALID,
                        "bad-joinsplits-nullifiers-duplicate",
                        false, NULL);
                }
            }
        }
    }

    /* Check for duplicate sapling nullifiers */
    for (size_t i = 0; i < tx->num_shielded_spend; i++) {
        for (size_t j = i + 1; j < tx->num_shielded_spend; j++) {
            if (uint256_cmp(&tx->v_shielded_spend[i].nullifier,
                            &tx->v_shielded_spend[j].nullifier) == 0) {
                return validation_state_dos(state, 100, false, REJECT_INVALID,
                    "bad-spend-description-nullifiers-duplicate",
                    false, NULL);
            }
        }
    }

    if (transaction_is_coinbase(tx)) {
        /* Coinbase cannot have joinsplits */
        if (tx->num_joinsplit > 0) {
            return validation_state_dos(state, 100, false, REJECT_INVALID,
                                        "bad-cb-has-joinsplits", false, NULL);
        }
        /* Coinbase cannot have shielded spends or outputs */
        if (tx->num_shielded_spend > 0) {
            return validation_state_dos(state, 100, false, REJECT_INVALID,
                                        "bad-cb-has-spend-description",
                                        false, NULL);
        }
        if (tx->num_shielded_output > 0) {
            return validation_state_dos(state, 100, false, REJECT_INVALID,
                                        "bad-cb-has-output-description",
                                        false, NULL);
        }
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
