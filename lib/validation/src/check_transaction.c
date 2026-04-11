/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * CheckTransaction — context-free transaction validation.
 * 30 checks matching zclassicd main.cpp:1116-1364 exactly.
 *
 * Uses REJECT_IF / REJECT_UNLESS macros from validation.h
 * for Rails-style DRY validation (no boilerplate). */

#include "validation/check_transaction.h"
#include "core/amount.h"
#include "core/uint256.h"
#include "consensus/consensus.h"
#include "metrics/metrics.h"
#include "event/event.h"

static bool check_transaction_impl(const struct transaction *tx,
                                    struct validation_state *state);

bool check_transaction(const struct transaction *tx,
                       struct validation_state *state)
{
    bool ok = check_transaction_impl(tx, state);
    /* Emit on invalid (DoS-able) rejections. Skip MODE_ERROR (fatal,
     * internal failures unrelated to consensus) and successful runs.
     * Payload format (wave 8): "hash=<64hex> reason=<name> dos=<n>".
     * Hash lets consensus_reject_index key rejections by txid so
     * zcl_explain_reject can answer "why was this txid rejected?". */
    if (!ok && state && state->mode == MODE_INVALID &&
        state->reject_reason[0] != '\0') {
        char hex[65];
        uint256_get_hex(&tx->hash, hex);
        event_emitf(EV_CONSENSUS_REJECT_TX, 0,
                    "hash=%s reason=%s dos=%d",
                    hex, state->reject_reason, state->dos);
    }
    return ok;
}

static bool check_transaction_impl(const struct transaction *tx,
                                    struct validation_state *state)
{
    if (!transaction_is_coinbase(tx))
        metrics_increment_tx_validated();

    /* ── Version checks ─────────────────────────────────────── */
    REJECT_IF(!tx->overwintered && tx->version < SPROUT_MIN_TX_VERSION,
              state, 100, "bad-txns-version-too-low");

    if (tx->overwintered) {
        REJECT_IF(tx->version < OVERWINTER_MIN_TX_VERSION,
                  state, 100, "bad-tx-overwinter-version-too-low");

        REJECT_IF(tx->version_group_id != OVERWINTER_VERSION_GROUP_ID &&
                  tx->version_group_id != SAPLING_VERSION_GROUP_ID,
                  state, 100, "bad-tx-version-group-id");

        REJECT_IF(tx->expiry_height >= TX_EXPIRY_HEIGHT_THRESHOLD,
                  state, 100, "bad-tx-expiry-height-too-high");
    }

    /* ── Input/output existence ─────────────────────────────── */
    REJECT_IF(tx->num_vin == 0 && tx->num_joinsplit == 0 &&
              tx->num_shielded_spend == 0,
              state, 10, "bad-txns-vin-empty");

    REJECT_IF(tx->num_vout == 0 && tx->num_joinsplit == 0 &&
              tx->num_shielded_output == 0,
              state, 10, "bad-txns-vout-empty");

    /* ── Size limit ─────────────────────────────────────────── */
    size_t tx_size = transaction_serialize_size(tx);
    REJECT_IF(tx_size > MAX_TX_SIZE_AFTER_SAPLING,
              state, 100, "bad-txns-oversize");

    /* ── Output value validation ────────────────────────────── */
    int64_t value_out = 0;
    for (size_t i = 0; i < tx->num_vout; i++) {
        REJECT_IF(tx->vout[i].value < 0,
                  state, 100, "bad-txns-vout-negative");
        REJECT_IF(tx->vout[i].value > MAX_MONEY,
                  state, 100, "bad-txns-vout-toolarge");
        value_out += tx->vout[i].value;
        REJECT_UNLESS(MoneyRange(value_out),
                      state, 100, "bad-txns-txouttotal-toolarge");
    }

    /* ── Sapling valueBalance ───────────────────────────────── */
    REJECT_IF(tx->num_shielded_spend == 0 && tx->num_shielded_output == 0 &&
              tx->value_balance != 0,
              state, 100, "bad-txns-valuebalance-nonzero");

    REJECT_IF(tx->value_balance > MAX_MONEY || tx->value_balance < -MAX_MONEY,
              state, 100, "bad-txns-valuebalance-toolarge");

    /* Negative valueBalance takes from transparent pool */
    if (tx->value_balance <= 0) {
        value_out += -tx->value_balance;
        REJECT_UNLESS(MoneyRange(value_out),
                      state, 100, "bad-txns-txouttotal-toolarge");
    }

    /* ── JoinSplit value validation ─────────────────────────── */
    for (size_t i = 0; i < tx->num_joinsplit; i++) {
        const struct js_description *js = &tx->v_joinsplit[i];

        REJECT_IF(js->vpub_old < 0, state, 100, "bad-txns-vpub_old-negative");
        REJECT_IF(js->vpub_new < 0, state, 100, "bad-txns-vpub_new-negative");
        REJECT_IF(js->vpub_old > MAX_MONEY,
                  state, 100, "bad-txns-vpub_old-toolarge");
        REJECT_IF(js->vpub_new > MAX_MONEY,
                  state, 100, "bad-txns-vpub_new-toolarge");
        REJECT_IF(js->vpub_new != 0 && js->vpub_old != 0,
                  state, 100, "bad-txns-vpubs-both-nonzero");

        value_out += js->vpub_old;
        REJECT_UNLESS(MoneyRange(value_out),
                      state, 100, "bad-txns-txouttotal-toolarge");
    }

    /* ── Input value overflow check ─────────────────────────── */
    {
        int64_t value_in = 0;
        for (size_t i = 0; i < tx->num_joinsplit; i++) {
            value_in += tx->v_joinsplit[i].vpub_new;
            REJECT_IF(!MoneyRange(tx->v_joinsplit[i].vpub_new) ||
                      !MoneyRange(value_in),
                      state, 100, "bad-txns-txintotal-toolarge");
        }
        if (tx->value_balance >= 0) {
            value_in += tx->value_balance;
            REJECT_UNLESS(MoneyRange(value_in),
                          state, 100, "bad-txns-txintotal-toolarge");
        }
    }

    /* ── Duplicate transparent inputs ───────────────────────── */
    for (size_t i = 0; i < tx->num_vin; i++) {
        for (size_t j = i + 1; j < tx->num_vin; j++) {
            REJECT_IF(outpoint_cmp(&tx->vin[i].prevout,
                                   &tx->vin[j].prevout) == 0,
                      state, 100, "bad-txns-inputs-duplicate");
        }
    }

    /* ── Duplicate JoinSplit nullifiers ──────────────────────── */
    for (size_t i = 0; i < tx->num_joinsplit; i++) {
        for (int ni = 0; ni < ZC_NUM_JS_INPUTS; ni++) {
            const struct uint256 *nf = &tx->v_joinsplit[i].nullifiers[ni];
            for (size_t j = 0; j < i; j++) {
                for (int nj = 0; nj < ZC_NUM_JS_INPUTS; nj++) {
                    REJECT_IF(uint256_cmp(nf,
                              &tx->v_joinsplit[j].nullifiers[nj]) == 0,
                              state, 100,
                              "bad-joinsplits-nullifiers-duplicate");
                }
            }
            for (int nj = 0; nj < ni; nj++) {
                REJECT_IF(uint256_cmp(nf,
                          &tx->v_joinsplit[i].nullifiers[nj]) == 0,
                          state, 100, "bad-joinsplits-nullifiers-duplicate");
            }
        }
    }

    /* ── Duplicate Sapling nullifiers ───────────────────────── */
    for (size_t i = 0; i < tx->num_shielded_spend; i++) {
        for (size_t j = i + 1; j < tx->num_shielded_spend; j++) {
            REJECT_IF(uint256_cmp(&tx->v_shielded_spend[i].nullifier,
                                  &tx->v_shielded_spend[j].nullifier) == 0,
                      state, 100,
                      "bad-spend-description-nullifiers-duplicate");
        }
    }

    /* ── Coinbase restrictions ──────────────────────────────── */
    if (transaction_is_coinbase(tx)) {
        REJECT_IF(tx->num_joinsplit > 0,
                  state, 100, "bad-cb-has-joinsplits");
        REJECT_IF(tx->num_shielded_spend > 0,
                  state, 100, "bad-cb-has-spend-description");
        REJECT_IF(tx->num_shielded_output > 0,
                  state, 100, "bad-cb-has-output-description");
        REJECT_IF(tx->vin[0].script_sig.size < 2 ||
                  tx->vin[0].script_sig.size > 100,
                  state, 100, "bad-cb-length");
    } else {
        /* Non-coinbase: no null prevouts */
        for (size_t i = 0; i < tx->num_vin; i++) {
            REJECT_IF(outpoint_is_null(&tx->vin[i].prevout),
                      state, 10, "bad-txns-prevout-null");
        }
    }

    return true;
}
