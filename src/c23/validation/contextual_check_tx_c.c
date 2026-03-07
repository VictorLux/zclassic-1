/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "validation/contextual_check_tx_c.h"
#include "consensus/consensus.h"
#include "consensus/upgrades.h"
#include "core/serialize_c.h"

static size_t transaction_wire_size(const struct transaction *tx)
{
    struct byte_stream s;
    stream_init(&s, 1024);

    if (tx->overwintered) {
        stream_write_u32_le(&s, (uint32_t)tx->version | (1U << 31));
        stream_write_u32_le(&s, tx->version_group_id);
    } else {
        stream_write_u32_le(&s, (uint32_t)tx->version);
    }

    stream_write_compact_size(&s, tx->num_vin);
    for (size_t i = 0; i < tx->num_vin; i++)
        tx_in_serialize(&tx->vin[i], &s);

    stream_write_compact_size(&s, tx->num_vout);
    for (size_t i = 0; i < tx->num_vout; i++)
        tx_out_serialize(&tx->vout[i], &s);

    stream_write_u32_le(&s, tx->lock_time);

    if (tx->overwintered) {
        stream_write_u32_le(&s, tx->expiry_height);
        if (tx->version >= SAPLING_TX_VERSION)
            stream_write_u64_le(&s, (uint64_t)tx->value_balance);
    }

    /* joinsplit/shielded data: write empty counts for transparent-only */
    if (tx->version >= 2)
        stream_write_compact_size(&s, 0); /* vjoinsplit */
    if (tx->version >= SAPLING_TX_VERSION) {
        stream_write_compact_size(&s, 0); /* vShieldedSpend */
        stream_write_compact_size(&s, 0); /* vShieldedOutput */
    }

    size_t sz = s.size;
    stream_free(&s);
    return sz;
}

bool contextual_check_transaction(const struct transaction *tx,
                                   struct validation_state *state,
                                   const struct consensus_params *params,
                                   int nHeight,
                                   int dosLevel)
{
    bool overwinterActive = consensus_network_upgrade_active(
        params, nHeight, UPGRADE_OVERWINTER);
    bool saplingActive = consensus_network_upgrade_active(
        params, nHeight, UPGRADE_SAPLING);
    bool isSprout = !overwinterActive;

    if (isSprout && tx->overwintered) {
        return validation_state_dos(state, dosLevel, false, REJECT_INVALID,
                                    "tx-overwinter-not-active", false, NULL);
    }

    if (saplingActive) {
        if (tx->version >= SAPLING_MIN_TX_VERSION && !tx->overwintered) {
            return validation_state_dos(state, dosLevel, false, REJECT_INVALID,
                                        "tx-overwintered-flag-not-set",
                                        false, NULL);
        }
        if (tx->overwintered &&
            tx->version_group_id != SAPLING_VERSION_GROUP_ID) {
            return validation_state_dos(state, dosLevel, false, REJECT_INVALID,
                                        "bad-sapling-tx-version-group-id",
                                        false, NULL);
        }
        if (tx->overwintered && tx->version < SAPLING_MIN_TX_VERSION) {
            return validation_state_dos(state, 100, false, REJECT_INVALID,
                                        "bad-tx-sapling-version-too-low",
                                        false, NULL);
        }
        if (tx->overwintered && tx->version > SAPLING_MAX_TX_VERSION) {
            return validation_state_dos(state, 100, false, REJECT_INVALID,
                                        "bad-tx-sapling-version-too-high",
                                        false, NULL);
        }
    } else if (overwinterActive) {
        if (tx->version >= OVERWINTER_MIN_TX_VERSION && !tx->overwintered) {
            return validation_state_dos(state, dosLevel, false, REJECT_INVALID,
                                        "tx-overwinter-flag-not-set",
                                        false, NULL);
        }
        if (tx->overwintered &&
            tx->version_group_id != OVERWINTER_VERSION_GROUP_ID) {
            return validation_state_dos(state, dosLevel, false, REJECT_INVALID,
                                        "bad-overwinter-tx-version-group-id",
                                        false, NULL);
        }
        if (tx->overwintered && tx->version > OVERWINTER_MAX_TX_VERSION) {
            return validation_state_dos(state, 100, false, REJECT_INVALID,
                                        "bad-tx-overwinter-version-too-high",
                                        false, NULL);
        }
    }

    if (overwinterActive) {
        if (!tx->overwintered) {
            return validation_state_dos(state, dosLevel, false, REJECT_INVALID,
                                        "tx-overwinter-active", false, NULL);
        }
        if (is_expired_tx(tx, nHeight)) {
            int expiredDosLevel = is_expired_tx(tx, nHeight - 1) ? dosLevel : 0;
            return validation_state_dos(state, expiredDosLevel, false,
                                        REJECT_INVALID,
                                        "tx-overwinter-expired", false, NULL);
        }
    }

    if (!saplingActive) {
        if (transaction_wire_size(tx) > MAX_TX_SIZE_BEFORE_SAPLING) {
            return validation_state_dos(state, 100, false, REJECT_INVALID,
                                        "bad-txns-oversize", false, NULL);
        }
    }

    return true;
}
