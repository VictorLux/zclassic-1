/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * ContextualCheckTransaction — height-aware transaction validation.
 * 16 checks matching zclassicd main.cpp:935-1098 exactly.
 *
 * Verifies: network upgrade rules, expiry, JoinSplit signatures,
 * Sapling Groth16 proofs, Sprout proofs, binding signatures. */

#include "validation/contextual_check_tx.h"
#include "consensus/consensus.h"
#include "consensus/upgrades.h"
#include "validation/sighash.h"
#include "crypto/ed25519.h"
#include "sapling/sapling.h"
#include "sapling/sprout.h"
#include "sapling/bn254.h"
#include "core/serialize.h"
#include "sapling/sapling_prover.h"

/* Default: -1 (verify everything). Set by boot.c from -assumevalid flag. */
_Atomic int g_assume_valid_height = -1;

/* Convenience: REJECT_IF with variable DoS level (many checks use dosLevel) */
#define REJECT_IF_DOS(cond, state, dos, reason) \
    REJECT_IF(cond, state, dos, reason)

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

    /* ── Network upgrade version rules ──────────────────────── */
    REJECT_IF_DOS(isSprout && tx->overwintered,
                  state, dosLevel, "tx-overwinter-not-active");

    if (saplingActive) {
        REJECT_IF_DOS(tx->version >= SAPLING_MIN_TX_VERSION && !tx->overwintered,
                      state, dosLevel, "tx-overwintered-flag-not-set");

        REJECT_IF_DOS(tx->overwintered &&
                      tx->version_group_id != SAPLING_VERSION_GROUP_ID,
                      state, dosLevel, "bad-sapling-tx-version-group-id");

        REJECT_IF(tx->overwintered && tx->version < SAPLING_MIN_TX_VERSION,
                  state, 100, "bad-tx-sapling-version-too-low");

        REJECT_IF(tx->overwintered && tx->version > SAPLING_MAX_TX_VERSION,
                  state, 100, "bad-tx-sapling-version-too-high");
    } else if (overwinterActive) {
        REJECT_IF_DOS(tx->version >= OVERWINTER_MIN_TX_VERSION && !tx->overwintered,
                      state, dosLevel, "tx-overwinter-flag-not-set");

        REJECT_IF_DOS(tx->overwintered &&
                      tx->version_group_id != OVERWINTER_VERSION_GROUP_ID,
                      state, dosLevel, "bad-overwinter-tx-version-group-id");

        REJECT_IF(tx->overwintered && tx->version > OVERWINTER_MAX_TX_VERSION,
                  state, 100, "bad-tx-overwinter-version-too-high");
    }

    /* ── Overwinter rules (apply when active, regardless of Sapling) ── */
    if (overwinterActive) {
        REJECT_IF_DOS(!tx->overwintered,
                      state, dosLevel, "tx-overwinter-active");

        if (is_expired_tx(tx, nHeight)) {
            int expiredDosLevel = is_expired_tx(tx, nHeight - 1) ? dosLevel : 0;
            REJECT_IF_DOS(true, state, expiredDosLevel, "tx-overwinter-expired");
        }
    }

    /* ── Pre-Sapling size limit ─────────────────────────────── */
    if (!saplingActive) {
        size_t tx_size = transaction_serialize_size(tx);
        REJECT_IF(tx_size > MAX_TX_SIZE_BEFORE_SAPLING,
                  state, 100, "bad-txns-oversize");
    }

    /* ── Skip expensive shielded proofs for assumed-valid blocks ── */
    bool skip_proofs = (g_assume_valid_height >= 0 &&
                        nHeight <= g_assume_valid_height);

    /* Compute sighash for shielded verification */
    struct uint256 data_to_be_signed;
    uint256_set_null(&data_to_be_signed);

    if (!skip_proofs &&
        (tx->num_joinsplit > 0 || tx->num_shielded_spend > 0 ||
         tx->num_shielded_output > 0))
    {
        uint32_t branch_id = consensus_current_epoch_branch_id(nHeight, params);
        struct script empty_script;
        empty_script.size = 0;
        struct sighash_type ht;
        ht.raw = 1; /* SIGHASH_ALL */
        REJECT_UNLESS(signature_hash(&empty_script, tx, NOT_AN_INPUT, ht, 0,
                                     branch_id, NULL, &data_to_be_signed),
                      state, 100, "error-computing-signature-hash");
    }

    /* ── JoinSplit Ed25519 signature ────────────────────────── */
    if (!skip_proofs && tx->num_joinsplit > 0) {
        REJECT_UNLESS(ed25519_verify(tx->joinsplit_sig, data_to_be_signed.data,
                                     32, tx->joinsplit_pubkey.data),
                      state, 100, "bad-txns-joinsplit-signature");
    }

    /* ── Sapling Groth16 spend/output proofs + binding sig ──── */
    if (!skip_proofs &&
        (tx->num_shielded_spend > 0 || tx->num_shielded_output > 0)) {
        void *sctx = zclassic_sapling_verification_ctx_init();
        REJECT_UNLESS(sctx, state, 100, "sapling-verification-ctx-init-failed");

        for (size_t i = 0; i < tx->num_shielded_spend; i++) {
            const struct spend_description *sd = &tx->v_shielded_spend[i];
            if (!zclassic_sapling_check_spend(
                    sctx, sd->cv.data, sd->anchor.data,
                    sd->nullifier.data, sd->rk.data,
                    sd->zkproof, sd->spend_auth_sig,
                    data_to_be_signed.data)) {
                zclassic_sapling_verification_ctx_free(sctx);
                return validation_state_dos(state, 100, false, REJECT_INVALID,
                    "bad-txns-sapling-spend-description-invalid", false, NULL);
            }
        }

        for (size_t i = 0; i < tx->num_shielded_output; i++) {
            const struct output_description *od = &tx->v_shielded_output[i];
            if (!zclassic_sapling_check_output(
                    sctx, od->cv.data, od->cm.data,
                    od->ephemeral_key.data, od->zkproof)) {
                zclassic_sapling_verification_ctx_free(sctx);
                return validation_state_dos(state, 100, false, REJECT_INVALID,
                    "bad-txns-sapling-output-description-invalid", false, NULL);
            }
        }

        if (!zclassic_sapling_final_check(
                sctx, tx->value_balance,
                tx->binding_sig, data_to_be_signed.data)) {
            zclassic_sapling_verification_ctx_free(sctx);
            return validation_state_dos(state, 100, false, REJECT_INVALID,
                "bad-txns-sapling-binding-sig-invalid", false, NULL);
        }

        zclassic_sapling_verification_ctx_free(sctx);
    }

    /* ── Sprout JoinSplit zk-SNARK proofs (Groth16 + PHGR13) ── */
    for (size_t i = 0; !skip_proofs && i < tx->num_joinsplit; i++) {
        const struct js_description *js = &tx->v_joinsplit[i];

        uint8_t h_sig[32];
        sprout_h_sig(js->random_seed.data, js->nullifiers[0].data,
                     js->nullifiers[1].data, tx->joinsplit_pubkey.data,
                     h_sig);

        if (js->use_groth) {
            REJECT_UNLESS(sprout_verify_groth16(js->proof,
                    js->anchor.data, h_sig,
                    js->macs[0].data, js->macs[1].data,
                    js->nullifiers[0].data, js->nullifiers[1].data,
                    js->commitments[0].data, js->commitments[1].data,
                    (uint64_t)js->vpub_old, (uint64_t)js->vpub_new),
                          state, 100, "bad-txns-joinsplit-proof-invalid");
        } else {
            REJECT_UNLESS(sprout_verify_phgr13(js->proof,
                    js->anchor.data, h_sig,
                    js->macs[0].data, js->macs[1].data,
                    js->nullifiers[0].data, js->nullifiers[1].data,
                    js->commitments[0].data, js->commitments[1].data,
                    (uint64_t)js->vpub_old, (uint64_t)js->vpub_new),
                          state, 100, "bad-txns-joinsplit-phgr13-invalid");
        }
    }

    return true;
}
