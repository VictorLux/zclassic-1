/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * ConnectBlock — full block validation against the UTXO set.
 * 14 checks matching zclassicd main.cpp:2489-2702 exactly.
 *
 * NEW in this refactor:
 *   - REJECT_IF / REJECT_UNLESS macros (Rails-style DRY)
 *   - ZIP-209 turnstile checks (Sprout/Sapling pool can't go negative)
 *   - Per-input MoneyRange validation
 *   - Validation events via event_emitf()
 *   - Input value < output check (bad-txns-in-belowout) */

#include <stdio.h>
#include "validation/connect_block.h"
#include "validation/check_block.h"
#include "validation/check_transaction.h"
#include "validation/contextual_check_tx.h"
#include "validation/update_coins.h"
#include "validation/sigops.h"
#include "validation/sighash.h"
#include "validation/tx_verifier.h"
#include "chain/subsidy.h"
#include "chain/checkpoints.h"
#include "consensus/upgrades.h"
#include "validation/main_constants.h"
#include "sapling/incremental_merkle_tree.h"
#include "event/event.h"

/* Global Sapling tree pointer — set by process_block's connect_tip
 * before calling connect_block. NULL during just_check mode. */
static struct incremental_merkle_tree *g_sapling_tree = NULL;

void connect_block_set_sapling_tree(struct incremental_merkle_tree *tree)
{
    g_sapling_tree = tree;
}

#ifndef COINBASE_MATURITY
#define COINBASE_MATURITY 100
#endif
#include "script/interpreter.h"
#include <string.h>

static bool checkpoint_covers(const struct checkpoint_data *cpdata,
                              int height)
{
    if (cpdata->nEntries <= 0)
        return false;
    return cpdata->entries[cpdata->nEntries - 1].height >= height;
}

bool connect_block(const struct block *block,
                   struct validation_state *state,
                   struct block_index *pindex,
                   struct coins_view_cache *view,
                   const struct chain_params *params,
                   bool just_check)
{
    bool expensive_checks = true;
    if (checkpoint_covers(&params->checkpointData, pindex->nHeight))
        expensive_checks = false;
    if (g_assume_valid_height >= 0 && pindex->nHeight <= g_assume_valid_height)
        expensive_checks = false;

    /* Re-validate block. Merkle root is always checked (cheap SHA256d
     * over txids — catches data corruption even below assume-valid).
     * PoW and size limits gated by expensive_checks. */
    if (!check_block(block, state, params, expensive_checks,
                     !just_check, true))
        return false;

    /* Use pre-computed hash from block_index */
    struct uint256 block_hash;
    if (pindex->phashBlock)
        block_hash = *pindex->phashBlock;
    else
        block_header_get_hash(&block->header, &block_hash);

    /* Genesis block: just set best block, no validation needed */
    if (uint256_cmp(&block_hash, &params->consensus.hashGenesisBlock) == 0) {
        if (!just_check) {
            /* Low-level: bypasses csr — `view` here is a stack-local
             * scratchpad (see process_block.c:817) that wraps coins_tip
             * as backing. The real tip commit runs in
             * process_block_commit_tip() → csr_commit_tip() after
             * coins_view_cache_flush() propagates this view to the
             * global coins_tip. There is no block_map / active_chain
             * mutation here; this is a single-field write inside an
             * in-flight block apply. */
            coins_view_cache_set_best_block(view, &block_hash);
        }
        return true;
    }

    event_emitf(EV_BLOCK_CONNECT_START, 0,
                "height=%d ntx=%zu", pindex->nHeight, block->num_vtx);

    /* ── View/prevblock invariant (zclassicd main.cpp:2513) ── */
    {
        struct uint256 view_best;
        coins_view_cache_get_best_block(view, &view_best);
        if (!uint256_is_null(&view_best) &&
            uint256_cmp(&block->header.hashPrevBlock, &view_best) != 0) {
            char vhex[65], phex[65];
            uint256_get_hex(&view_best, vhex);
            uint256_get_hex(&block->header.hashPrevBlock, phex);
            fprintf(stderr, "connect_block: FATAL view/prevblock mismatch "
                    "h=%d view=%s prev=%s\n", pindex->nHeight, vhex, phex);
            REJECT_FATAL(state, "connect_block-view-mismatch");
        }
    }

    /* ── ZIP-209: Turnstile enforcement ─────────────────────── *
     * Shielded value pools (Sprout and Sapling) must never go negative.
     * This matches zclassicd ConnectBlock lines 2537-2551.
     *
     * CRITICAL: nChainSproutValue/nChainSaplingValue use boost::optional
     * semantics in zclassicd. We mirror this with has_chain_*_value flags.
     * Only enforce the turnstile when the cumulative value is KNOWN —
     * i.e., when every ancestor block's per-block value was computed.
     * If the parent's chain value is unknown (imported from LevelDB where
     * these weren't tracked), skip the check. */
    if (pindex->pprev) {
        /* Compute per-block shielded value from transactions */
        int64_t sprout_value = 0;
        int64_t sapling_value = 0;
        for (size_t i = 0; i < block->num_vtx; i++) {
            const struct transaction *tx = &block->vtx[i];
            for (size_t j = 0; j < tx->num_joinsplit; j++) {
                sprout_value += tx->v_joinsplit[j].vpub_old;
                sprout_value -= tx->v_joinsplit[j].vpub_new;
            }
            sapling_value += tx->value_balance;
        }
        pindex->nSproutValue = sprout_value;
        pindex->has_sprout_value = true;
        pindex->nSaplingValue = sapling_value;

        /* Propagate cumulative chain values only when parent is known */
        if (pindex->pprev->has_chain_sprout_value) {
            pindex->nChainSproutValue =
                pindex->pprev->nChainSproutValue + sprout_value;
            pindex->has_chain_sprout_value = true;

            /* ZIP-209: Sprout pool can't go negative */
            REJECT_IF(pindex->nChainSproutValue < 0,
                      state, 100,
                      "bad-txns-sprout-turnstile-violation");
        }

        if (pindex->pprev->has_chain_sapling_value) {
            pindex->nChainSaplingValue =
                pindex->pprev->nChainSaplingValue + sapling_value;
            pindex->has_chain_sapling_value = true;

            /* ZIP-209: Sapling pool can't go negative */
            REJECT_IF(pindex->nChainSaplingValue < 0,
                      state, 100,
                      "bad-txns-sapling-turnstile-violation");
        }
    }

    /* ── BIP30: no overwriting unspent transactions ─────────── *
     * ZClassic enforces unconditionally — no height exceptions. */
    for (size_t i = 0; i < block->num_vtx; i++) {
        if (coins_view_cache_have_coins(view, &block->vtx[i].hash)) {
            struct coins existing;
            coins_init(&existing);
            if (coins_view_cache_get_coins(view, &block->vtx[i].hash,
                                           &existing)) {
                if (!coins_is_pruned(&existing)) {
                    coins_free(&existing);
                    return validation_state_dos(state, 100, false,
                        REJECT_INVALID, "bad-txns-BIP30", false, NULL);
                }
            }
            coins_free(&existing);
        }
    }

    uint32_t flags = SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY;

    struct block_undo blockundo;
    block_undo_init(&blockundo);
    if (block->num_vtx > 1) {
        if (!block_undo_alloc(&blockundo, block->num_vtx - 1)) {
            fprintf(stderr, "connect_block: block_undo_alloc failed h=%d "
                    "ntx=%zu\n", pindex->nHeight, block->num_vtx);
            block_undo_free(&blockundo);
            return false;
        }
    }

    uint32_t branch_id = consensus_current_epoch_branch_id(
        pindex->nHeight, &params->consensus);

    int64_t fees = 0;
    unsigned int sig_ops = 0;

    for (size_t i = 0; i < block->num_vtx; i++) {
        const struct transaction *tx = &block->vtx[i];

        /* ── Sigops check ─────────────────────────────── */
        sig_ops += (unsigned int)get_legacy_sig_op_count(tx, flags);
        REJECT_IF_CLEANUP(sig_ops > MAX_BLOCK_SIGOPS,
                          state, 100, "bad-blk-sigops",
                          block_undo_free(&blockundo));

        if (!transaction_is_coinbase(tx)) {
            /* ── Coinbase maturity (100 blocks) ───────── */
            for (size_t j = 0; j < tx->num_vin; j++) {
                struct coins prev_coins;
                coins_init(&prev_coins);
                if (coins_view_cache_get_coins(view,
                        &tx->vin[j].prevout.hash, &prev_coins)) {
                    if (prev_coins.is_coinbase &&
                        pindex->nHeight - prev_coins.height < COINBASE_MATURITY) {
                        coins_free(&prev_coins);
                        block_undo_free(&blockundo);
                        return validation_state_dos(state, 100, false,
                            REJECT_INVALID,
                            "bad-txns-premature-spend-of-coinbase",
                            false, NULL);
                    }
                }
                coins_free(&prev_coins);
            }

            /* ── All inputs must exist and be unspent ── */
            if (!coins_view_cache_have_inputs(view, tx)) {
                /* Find the specific missing input for self-healing recovery */
                for (size_t vi = 0; vi < tx->num_vin; vi++) {
                    const struct tx_out *out =
                        coins_view_cache_get_output_for(view, &tx->vin[vi]);
                    if (!out) {
                        char prevhex[65];
                        uint256_get_hex(&tx->vin[vi].prevout.hash, prevhex);
                        printf("missing-input: h=%d tx[%zu] vin[%zu]=%s:%u "
                               "cache=%zu\n", pindex->nHeight, i, vi,
                               prevhex, tx->vin[vi].prevout.n,
                               view->cache_coins.size);
                        state->missing_txid = tx->vin[vi].prevout.hash;
                        state->missing_vout = tx->vin[vi].prevout.n;
                        state->has_missing_utxo = true;
                        break;
                    }
                }
                block_undo_free(&blockundo);
                return validation_state_dos(state, 100, false, REJECT_INVALID,
                    "bad-txns-inputs-missingorspent", false, NULL);
            }

            /* ── JoinSplit anchor requirements ─────────── */
            if (!coins_view_cache_have_joinsplit_requirements(view, tx)) {
                block_undo_free(&blockundo);
                return validation_state_dos(state, 100, false, REJECT_INVALID,
                    "bad-txns-joinsplit-requirements-not-met", false, NULL);
            }
        }

        /* ── Fee calculation with per-input MoneyRange ─── */
        if (!transaction_is_coinbase(tx)) {
            int64_t value_in = coins_view_cache_get_value_in(view, tx);
            int64_t value_out = transaction_get_value_out(tx);

            /* get_value_in returns -1 on missing inputs or out-of-range values.
             * This catches corrupted coins before they propagate. */
            REJECT_IF_CLEANUP(value_in < 0,
                              state, 100, "bad-txns-inputvalues-outofrange",
                              block_undo_free(&blockundo));

            /* Per-input value range check (zclassicd CheckTxInputs:2075) */
            REJECT_IF_CLEANUP(!MoneyRange(value_in),
                              state, 100, "bad-txns-inputvalues-outofrange",
                              block_undo_free(&blockundo));

            /* Inputs must cover outputs (no money creation) */
            REJECT_IF_CLEANUP(value_in < value_out,
                              state, 100, "bad-txns-in-belowout",
                              block_undo_free(&blockundo));

            int64_t tx_fee = value_in - value_out;

            /* Fee sanity: non-negative and no overflow */
            REJECT_IF_CLEANUP(tx_fee < 0,
                              state, 100, "bad-txns-fee-negative",
                              block_undo_free(&blockundo));
            REJECT_IF_CLEANUP(!MoneyRange(fees + tx_fee),
                              state, 100, "bad-txns-fee-outofrange",
                              block_undo_free(&blockundo));
            fees += tx_fee;

            event_emitf(EV_TX_INPUTS_CHECKED, 0,
                        "h=%d tx=%zu value_in=%lld fee=%lld",
                        pindex->nHeight, i,
                        (long long)value_in, (long long)tx_fee);

            /* ── Script verification ─────────────────── */
            if (expensive_checks) {
                struct precomputed_tx_data txdata;
                precompute_tx_data(tx, &txdata);

                for (size_t j = 0; j < tx->num_vin; j++) {
                    const struct tx_out *prev_out =
                        coins_view_cache_get_output_for(view, &tx->vin[j]);
                    if (!prev_out) {
                        block_undo_free(&blockundo);
                        return validation_state_dos(state, 100, false,
                            REJECT_INVALID, "bad-txns-inputs-missingorspent",
                            false, NULL);
                    }

                    /* Per-input value in valid range */
                    REJECT_IF_CLEANUP(!MoneyRange(prev_out->value),
                                      state, 100,
                                      "bad-txns-inputvalues-outofrange",
                                      block_undo_free(&blockundo));

                    struct tx_sig_checker tsc;
                    tx_sig_checker_init(&tsc, tx, (unsigned int)j,
                                        prev_out->value, branch_id, &txdata);
                    struct sig_checker checker = tx_make_sig_checker(&tsc);

                    ScriptError serror = SCRIPT_ERR_OK;
                    if (!verify_script(&tx->vin[j].script_sig,
                                       &prev_out->script_pub_key,
                                       flags, &checker,
                                       branch_id, &serror)) {
                        block_undo_free(&blockundo);
                        return validation_state_dos(state, 100, false,
                            REJECT_INVALID,
                            "mandatory-script-verify-flag-failed",
                            false, NULL);
                    }
                }
                event_emitf(EV_SCRIPT_VERIFIED, 0,
                            "h=%d tx=%zu inputs=%zu",
                            pindex->nHeight, i, tx->num_vin);
            }
        }

        /* ── Update UTXO set ──────────────────────────── */
        if (i > 0) {
            if (!update_coins_with_undo(tx, view, &blockundo.vtxundo[i - 1],
                                        pindex->nHeight)) {
                block_undo_free(&blockundo);
                return validation_state_dos(state, 100, false, REJECT_INVALID,
                    "bad-txns-utxo-update-failed", true, NULL);
            }
        } else {
            struct tx_undo dummy;
            tx_undo_init(&dummy);
            bool ok = update_coins_with_undo(tx, view, &dummy,
                                              pindex->nHeight);
            tx_undo_free(&dummy);
            if (!ok) {
                block_undo_free(&blockundo);
                return validation_state_dos(state, 100, false, REJECT_INVALID,
                    "bad-txns-utxo-update-failed", true, NULL);
            }
        }
    }

    /* ── Sapling commitment tree root verification ─────────── *
     * The Sapling tree is maintained by sync_controller which handles
     * both tree updates AND wallet witness advancement. It verifies
     * hashFinalSaplingRoot matches the computed tree root after each
     * block. See sync_controller.c:291-325.
     *
     * connect_block validates the tree root was set correctly in the
     * block header by checking it's not all-zeros after Sapling activation
     * (a basic sanity check — full verification is in sync_controller). */
    if (!just_check) {
        bool sapling_active = consensus_network_upgrade_active(
            &params->consensus, pindex->nHeight, UPGRADE_SAPLING);
        if (sapling_active) {
            static const uint8_t zeros[32] = {0};
            if (memcmp(block->header.hashFinalSaplingRoot.data, zeros, 32) == 0) {
                fprintf(stderr, "connect_block: hashFinalSaplingRoot is "
                        "all-zeros at Sapling height %d\n", pindex->nHeight);
                block_undo_free(&blockundo);
                return validation_state_dos(state, 100, false, REJECT_INVALID,
                    "bad-sapling-root-zeroed", false, NULL);
            }
        }
    }

    /* ── Coinbase reward validation ───────────────────────── */
    int64_t subsidy = get_block_subsidy(pindex->nHeight, &params->consensus);
    REJECT_IF_CLEANUP(fees > INT64_MAX - subsidy,
                      state, 100, "bad-cb-reward-overflow",
                      block_undo_free(&blockundo));

    int64_t block_reward = fees + subsidy;
    REJECT_IF_CLEANUP(transaction_get_value_out(&block->vtx[0]) > block_reward,
                      state, 100, "bad-cb-amount",
                      block_undo_free(&blockundo));

    if (just_check) {
        block_undo_free(&blockundo);
        return true;
    }

    /* Low-level: bypasses csr — `view` is a stack-local scratchpad
     * (process_block.c:817) wrapping coins_tip as backing. The real
     * tip commit happens in update_tip() → process_block_commit_tip()
     * → csr_commit_tip() after coins_view_cache_flush() promotes this
     * view's pending writes to the global coins_tip. */
    coins_view_cache_set_best_block(view, &block_hash);

    event_emitf(EV_BLOCK_CONNECT_DONE, 0,
                "height=%d fees=%lld sigops=%u",
                pindex->nHeight, (long long)fees, sig_ops);

    block_undo_free(&blockundo);
    return true;
}

bool disconnect_block(const struct block *block,
                      struct validation_state *state,
                      struct block_index *pindex,
                      struct coins_view_cache *view,
                      const struct block_undo *blockundo)
{
    (void)state;

    if (blockundo->num_txundo != block->num_vtx - 1) {
        fprintf(stderr, "disconnect_block: undo size mismatch "
                "(undo=%zu vtx=%zu)\n",
                blockundo->num_txundo, block->num_vtx);
        return false;
    }

    for (size_t i = block->num_vtx; i-- > 0; ) {
        const struct transaction *tx = &block->vtx[i];

        if (i > 0) {
            const struct tx_undo *txundo = &blockundo->vtxundo[i - 1];
            if (txundo->num_prevout != tx->num_vin) {
                fprintf(stderr, "disconnect_block: txundo prevout mismatch "
                        "tx=%zu (prevout=%zu vin=%zu)\n",
                        i, txundo->num_prevout, tx->num_vin);
                return false;
            }

            for (size_t j = tx->num_vin; j-- > 0; ) {
                const struct tx_in_undo *undo = &txundo->vprevout[j];
                struct coins_cache_entry *entry =
                    coins_view_cache_modify(view, &tx->vin[j].prevout.hash);
                if (!entry) {
                    fprintf(stderr, "disconnect_block: coins_modify failed "
                            "tx=%zu vin=%zu\n", i, j);
                    return false;
                }

                if (tx->vin[j].prevout.n >= entry->coins.num_vout) {
                    size_t new_size = tx->vin[j].prevout.n + 1;
                    struct tx_out *new_vout =
                        realloc(entry->coins.vout,
                                new_size * sizeof(struct tx_out));
                    if (!new_vout) {
                        fprintf(stderr, "disconnect_block: realloc failed "
                                "tx=%zu vin=%zu new_size=%zu\n", i, j, new_size);
                        return false;
                    }
                    for (size_t k = entry->coins.num_vout; k < new_size; k++)
                        tx_out_set_null(&new_vout[k]);
                    entry->coins.vout = new_vout;
                    entry->coins.num_vout = new_size;
                }

                if (undo->height > 0) {
                    entry->coins.is_coinbase = undo->coinbase;
                    entry->coins.height = (int)undo->height;
                    entry->coins.version = undo->version;
                }
                entry->coins.vout[tx->vin[j].prevout.n] = undo->txout;
                entry->flags |= COINS_CACHE_DIRTY;
            }
        }

        coins_map_erase(&view->cache_coins, &tx->hash);
    }

    if (pindex->pprev && pindex->pprev->phashBlock) {
        /* Low-level: bypasses csr — `view` is the stack-local scratchpad
         * built by disconnect_tip (process_block.c:1426). The global tip
         * commit happens in disconnect_tip → update_tip(pprev) →
         * process_block_commit_tip() → csr_commit_tip() after
         * coins_view_cache_flush() propagates this view to coins_tip. */
        coins_view_cache_set_best_block(view, pindex->pprev->phashBlock);
    }

    return true;
}
