/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

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

    if (!check_block(block, state, params, expensive_checks,
                     !just_check, expensive_checks))
        return false;

    struct uint256 block_hash;
    block_header_get_hash(&block->header, &block_hash);

    /* Special case: genesis block */
    if (uint256_cmp(&block_hash, &params->consensus.hashGenesisBlock) == 0) {
        if (!just_check)
            coins_view_cache_set_best_block(view, &block_hash);
        return true;
    }

    /* BIP30: do not allow blocks that overwrite existing unspent transactions.
     * After BIP34 activation (height in coinbase), duplicate txids are impossible.
     * ZClassic has BIP34 from genesis, so BIP30 violations cannot occur.
     * We still check for blocks below the BIP34 enforcement height as a safety
     * measure, but skip for heights where BIP34 guarantees uniqueness. */
    if (pindex->nHeight <= 91842) {
        for (size_t i = 0; i < block->num_vtx; i++) {
            if (coins_view_cache_have_coins(view, &block->vtx[i].hash)) {
                struct coins existing;
                coins_init(&existing);
                if (coins_view_cache_get_coins(view, &block->vtx[i].hash, &existing)) {
                    if (!coins_is_pruned(&existing)) {
                        coins_free(&existing);
                        return validation_state_dos(state, 100, false, REJECT_INVALID,
                            "bad-txns-BIP30", false, NULL);
                    }
                }
                coins_free(&existing);
            }
        }
    }

    uint32_t flags = SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY;

    struct block_undo blockundo;
    block_undo_init(&blockundo);
    if (block->num_vtx > 1) {
        if (!block_undo_alloc(&blockundo, block->num_vtx - 1)) {
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

        sig_ops += (unsigned int)get_legacy_sig_op_count(tx, flags);
        if (sig_ops > MAX_BLOCK_SIGOPS) {
            block_undo_free(&blockundo);
            return validation_state_dos(state, 100, false, REJECT_INVALID,
                "bad-blk-sigops", false, NULL);
        }

        if (!transaction_is_coinbase(tx)) {
            if (!coins_view_cache_have_inputs(view, tx)) {
                char txhex[65];
                uint256_get_hex(&tx->hash, txhex);
                printf("MISSING INPUTS for tx %s (vin=%zu):\n", txhex, tx->num_vin);
                for (size_t mi = 0; mi < tx->num_vin; mi++) {
                    char ph[65];
                    uint256_get_hex(&tx->vin[mi].prevout.hash, ph);
                    struct coins c;
                    coins_init(&c);
                    bool have = coins_view_cache_get_coins(view, &tx->vin[mi].prevout.hash, &c);
                    printf("  vin[%zu]: %s:%u have=%d num_vout=%zu\n",
                           mi, ph, tx->vin[mi].prevout.n, have, c.num_vout);
                    if (have && tx->vin[mi].prevout.n < c.num_vout) {
                        printf("    vout[%u] amount=%lld script_size=%zu\n",
                               tx->vin[mi].prevout.n,
                               (long long)c.vout[tx->vin[mi].prevout.n].value,
                               c.vout[tx->vin[mi].prevout.n].script_pub_key.size);
                    }
                    coins_free(&c);
                }
                block_undo_free(&blockundo);
                return validation_state_dos(state, 100, false, REJECT_INVALID,
                    "bad-txns-inputs-missingorspent", false, NULL);
            }

            if (!coins_view_cache_have_joinsplit_requirements(view, tx)) {
                block_undo_free(&blockundo);
                return validation_state_dos(state, 100, false, REJECT_INVALID,
                    "bad-txns-joinsplit-requirements-not-met", false, NULL);
            }
        }

        if (!transaction_is_coinbase(tx)) {
            int64_t value_in = coins_view_cache_get_value_in(view, tx);
            int64_t value_out = transaction_get_value_out(tx);
            fees += value_in - value_out;

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
                            REJECT_INVALID, "mandatory-script-verify-flag-failed",
                            false, NULL);
                    }
                }
            }
        }

        /* Update UTXO set */
        if (i > 0) {
            update_coins_with_undo(tx, view, &blockundo.vtxundo[i - 1],
                                   pindex->nHeight);
        } else {
            struct tx_undo dummy;
            tx_undo_init(&dummy);
            update_coins_with_undo(tx, view, &dummy, pindex->nHeight);
            tx_undo_free(&dummy);
        }
    }

    /* Verify coinbase reward */
    int64_t block_reward = fees +
        get_block_subsidy(pindex->nHeight, &params->consensus);
    if (transaction_get_value_out(&block->vtx[0]) > block_reward) {
        block_undo_free(&blockundo);
        return validation_state_dos(state, 100, false, REJECT_INVALID,
            "bad-cb-amount", false, NULL);
    }

    if (just_check) {
        block_undo_free(&blockundo);
        return true;
    }

    coins_view_cache_set_best_block(view, &block_hash);

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

    if (blockundo->num_txundo != block->num_vtx - 1)
        return false;

    for (size_t i = block->num_vtx; i-- > 0; ) {
        const struct transaction *tx = &block->vtx[i];

        if (i > 0) {
            const struct tx_undo *txundo = &blockundo->vtxundo[i - 1];
            if (txundo->num_prevout != tx->num_vin)
                return false;

            for (size_t j = tx->num_vin; j-- > 0; ) {
                const struct tx_in_undo *undo = &txundo->vprevout[j];
                struct coins_cache_entry *entry =
                    coins_view_cache_modify(view, &tx->vin[j].prevout.hash);
                if (!entry)
                    return false;

                if (tx->vin[j].prevout.n >= entry->coins.num_vout) {
                    size_t new_size = tx->vin[j].prevout.n + 1;
                    struct tx_out *new_vout =
                        realloc(entry->coins.vout,
                                new_size * sizeof(struct tx_out));
                    if (!new_vout)
                        return false;
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

    if (pindex->pprev && pindex->pprev->phashBlock)
        coins_view_cache_set_best_block(view, pindex->pprev->phashBlock);

    return true;
}
