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
#include "util/log_macros.h"
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
#include "util/workpool.h"
#include "util/util.h"

/* ── Parallel script verification ────────────────────────── */

/* Work item for one input's script verification */
struct script_check {
    const struct script *script_sig;
    const struct script *script_pub_key;
    uint32_t flags;
    uint32_t branch_id;
    struct tx_sig_checker tsc;
    struct precomputed_tx_data *txdata; /* shared per-tx, read-only */
};

static bool script_check_fn(void *item)
{
    struct script_check *sc = (struct script_check *)item;
    struct sig_checker checker = tx_make_sig_checker(&sc->tsc);
    ScriptError serror = SCRIPT_ERR_OK;
    return verify_script(sc->script_sig, sc->script_pub_key,
                         sc->flags, &checker, sc->branch_id, &serror);
}

/* Lazy-initialized global workpool for script verification.
 * Workers persist across blocks to avoid thread create/destroy overhead. */
static struct workpool g_script_pool;
static bool g_script_pool_ready = false;

static struct workpool *get_script_pool(void)
{
    if (!g_script_pool_ready) {
        int ncores = GetNumCores();
        if (ncores < 2) ncores = 2;
        if (ncores > 16) ncores = 16;
        /* Queue capacity: large enough for biggest blocks (~5000 inputs) */
        if (workpool_init(&g_script_pool, ncores, 8192, script_check_fn))
            g_script_pool_ready = true;
        else
            return NULL;
    }
    return &g_script_pool;
}

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
#include "util/safe_alloc.h"

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
        LOG_FAIL("connect", "check_block failed at height %d", pindex->nHeight);

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
     * Skip below assume-valid: when re-connecting blocks on top of an
     * imported UTXO snapshot, the coinbase outputs already exist in the
     * UTXO set. These blocks were validated by zclassicd — BIP30 is
     * redundant and would false-positive on the imported coinbases. */
    bool skip_bip30 = (g_assume_valid_height >= 0 &&
                       pindex->nHeight <= g_assume_valid_height);
    for (size_t i = 0; !skip_bip30 && i < block->num_vtx; i++) {
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

    /* ── Parallel script verification: collection arrays ─── *
     * Phase 1 (in the loop below): gather all script checks.
     * Phase 2 (after the loop): dispatch to workpool.
     * txdatas[] holds precomputed tx data that outlives the loop. */
    struct script_check *checks = NULL;
    void **check_ptrs = NULL;
    size_t num_checks = 0;
    size_t checks_cap = 0;
    struct precomputed_tx_data *txdatas = NULL;
    size_t num_txdatas = 0;
    size_t txdatas_cap = 0;

    for (size_t i = 0; i < block->num_vtx; i++) {
        const struct transaction *tx = &block->vtx[i];

        /* ── Sigops check ─────────────────────────────── */
        sig_ops += (unsigned int)get_legacy_sig_op_count(tx, flags);
        REJECT_IF_CLEANUP(sig_ops > MAX_BLOCK_SIGOPS,
                          state, 100, "bad-blk-sigops",
                          (free(checks), free(check_ptrs), free(txdatas),
                           block_undo_free(&blockundo)));

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
                        free(checks); free(check_ptrs); free(txdatas);
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
                free(checks); free(check_ptrs); free(txdatas);
                block_undo_free(&blockundo);
                return validation_state_dos(state, 100, false, REJECT_INVALID,
                    "bad-txns-inputs-missingorspent", false, NULL);
            }

            /* ── JoinSplit anchor requirements ─────────── */
            if (!coins_view_cache_have_joinsplit_requirements(view, tx)) {
                free(checks); free(check_ptrs); free(txdatas);
                block_undo_free(&blockundo);
                return validation_state_dos(state, 100, false, REJECT_INVALID,
                    "bad-txns-joinsplit-requirements-not-met", false, NULL);
            }

            /* ── P2SH sigops (P1.6) ────────────────────── *
             * Add sigops done by pay-to-script-hash inputs; prevents a
             * rogue miner from hiding an expensive-to-validate block
             * inside a small scriptSig.  Mirrors zclassicd
             * src/main.cpp::GetP2SHSigOpCount + the ConnectBlock check
             * at main.cpp:2634-2637.  Must run AFTER have_inputs so the
             * prevouts are guaranteed to be in the view cache. */
            sig_ops += (unsigned int)get_p2sh_sig_op_count(tx, view, flags);
            REJECT_IF_CLEANUP(sig_ops > MAX_BLOCK_SIGOPS,
                              state, 100, "bad-blk-sigops",
                              (free(checks), free(check_ptrs), free(txdatas),
                               block_undo_free(&blockundo)));
        }

        /* ── Fee calculation with per-input MoneyRange ─── */
        if (!transaction_is_coinbase(tx)) {
            int64_t value_in = coins_view_cache_get_value_in(view, tx);
            int64_t value_out = transaction_get_value_out(tx);

            /* get_value_in returns -1 on missing inputs or out-of-range values.
             * This catches corrupted coins before they propagate. */
            REJECT_IF_CLEANUP(value_in < 0,
                              state, 100, "bad-txns-inputvalues-outofrange",
                              (free(checks), free(check_ptrs), free(txdatas),
                               block_undo_free(&blockundo)));

            /* Per-input value range check (zclassicd CheckTxInputs:2075) */
            REJECT_IF_CLEANUP(!MoneyRange(value_in),
                              state, 100, "bad-txns-inputvalues-outofrange",
                              (free(checks), free(check_ptrs), free(txdatas),
                               block_undo_free(&blockundo)));

            /* Inputs must cover outputs (no money creation) */
            REJECT_IF_CLEANUP(value_in < value_out,
                              state, 100, "bad-txns-in-belowout",
                              (free(checks), free(check_ptrs), free(txdatas),
                               block_undo_free(&blockundo)));

            int64_t tx_fee = value_in - value_out;

            /* Fee sanity: non-negative and no overflow */
            REJECT_IF_CLEANUP(tx_fee < 0,
                              state, 100, "bad-txns-fee-negative",
                              (free(checks), free(check_ptrs), free(txdatas),
                               block_undo_free(&blockundo)));
            REJECT_IF_CLEANUP(!MoneyRange(fees + tx_fee),
                              state, 100, "bad-txns-fee-outofrange",
                              (free(checks), free(check_ptrs), free(txdatas),
                               block_undo_free(&blockundo)));
            fees += tx_fee;

            event_emitf(EV_TX_INPUTS_CHECKED, 0,
                        "h=%d tx=%zu value_in=%lld fee=%lld",
                        pindex->nHeight, i,
                        (long long)value_in, (long long)tx_fee);

            /* ── Script verification (Phase 1: collect) ─── */
            if (expensive_checks) {
                if (num_txdatas >= txdatas_cap) {
                    txdatas_cap = txdatas_cap ? txdatas_cap * 2 : 64;
                    txdatas = zcl_realloc(txdatas,
                                      txdatas_cap * sizeof(*txdatas), "connect_txdatas");
                }
                precompute_tx_data(tx, &txdatas[num_txdatas]);
                num_txdatas++;

                for (size_t j = 0; j < tx->num_vin; j++) {
                    const struct tx_out *prev_out =
                        coins_view_cache_get_output_for(view, &tx->vin[j]);
                    if (!prev_out) {
                        free(checks);
                        free(check_ptrs);
                        free(txdatas);
                        block_undo_free(&blockundo);
                        return validation_state_dos(state, 100, false,
                            REJECT_INVALID, "bad-txns-inputs-missingorspent",
                            false, NULL);
                    }

                    /* Per-input value in valid range */
                    if (!MoneyRange(prev_out->value)) {
                        free(checks);
                        free(check_ptrs);
                        free(txdatas);
                        block_undo_free(&blockundo);
                        return validation_state_dos(state, 100, false,
                            REJECT_INVALID,
                            "bad-txns-inputvalues-outofrange",
                            false, NULL);
                    }

                    if (num_checks >= checks_cap) {
                        checks_cap = checks_cap ? checks_cap * 2 : 256;
                        checks = zcl_realloc(checks,
                                         checks_cap * sizeof(*checks), "connect_checks");
                        check_ptrs = zcl_realloc(check_ptrs,
                                             checks_cap * sizeof(*check_ptrs), "connect_check_ptrs");
                    }

                    struct script_check *sc = &checks[num_checks];
                    sc->script_sig = &tx->vin[j].script_sig;
                    sc->script_pub_key = &prev_out->script_pub_key;
                    sc->flags = flags;
                    sc->branch_id = branch_id;
                    sc->txdata = &txdatas[num_txdatas - 1];
                    tx_sig_checker_init(&sc->tsc, tx, (unsigned int)j,
                                        prev_out->value, branch_id,
                                        sc->txdata);
                    check_ptrs[num_checks] = sc;
                    num_checks++;
                }
            }
        }

        /* ── Update UTXO set ──────────────────────────── */
        if (i > 0) {
            if (!update_coins_with_undo(tx, view, &blockundo.vtxundo[i - 1],
                                        pindex->nHeight)) {
                free(checks); free(check_ptrs); free(txdatas);
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
                free(checks); free(check_ptrs); free(txdatas);
                block_undo_free(&blockundo);
                return validation_state_dos(state, 100, false, REJECT_INVALID,
                    "bad-txns-utxo-update-failed", true, NULL);
            }
        }
    }

    /* ── Script verification (Phase 2: parallel dispatch) ─── *
     * All inputs have been collected into checks[]. Now verify them
     * in parallel via the workpool. UTXO updates above are already
     * complete, so the coins view is not accessed by workers. */
    if (num_checks > 0) {
        struct workpool *pool = get_script_pool();
        bool scripts_ok;

        if (pool && num_checks >= 4) {
            /* Parallel path: dispatch to worker threads */
            scripts_ok = workpool_run(pool, check_ptrs, num_checks);
        } else {
            /* Sequential fallback: < 4 inputs or pool init failed */
            scripts_ok = true;
            for (size_t c = 0; c < num_checks && scripts_ok; c++)
                scripts_ok = script_check_fn(check_ptrs[c]);
        }

        free(checks);
        free(check_ptrs);
        free(txdatas);

        if (!scripts_ok) {
            block_undo_free(&blockundo);
            return validation_state_dos(state, 100, false,
                REJECT_INVALID, "mandatory-script-verify-flag-failed",
                false, NULL);
        }

        event_emitf(EV_SCRIPT_VERIFIED, 0,
                    "h=%d inputs=%zu parallel=%s",
                    pindex->nHeight, num_checks,
                    (pool && num_checks >= 4) ? "yes" : "no");
    } else {
        free(checks);
        free(check_ptrs);
        free(txdatas);
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
                    /* Clamp prevout.n against MAX_BLOCK_SIZE before
                     * extending the vout array. A valid funding tx
                     * cannot encode more than MAX_BLOCK_SIZE / ~30
                     * outputs (each tx_out is at least 8-byte value +
                     * minimal scriptPubKey), so anything beyond that
                     * ceiling is either corrupted block data or an
                     * attacker-controlled value crafted to force a
                     * ~128 GB realloc (prevout.n = UINT32_MAX =>
                     * 2**32 * sizeof(tx_out) ≈ 128 GB). */
                    if (tx->vin[j].prevout.n >= MAX_BLOCK_SIZE) {
                        LOG_FAIL("disconnect",
                                 "prevout.n out of range h=%d tx=%zu vin=%zu n=%u",
                                 pindex->nHeight, i, j, tx->vin[j].prevout.n);
                    }
                    size_t new_size = tx->vin[j].prevout.n + 1;
                    struct tx_out *new_vout =
                        zcl_realloc(entry->coins.vout,
                                new_size * sizeof(struct tx_out), "disconnect_vout");
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

        /* Remove this tx's outputs from the coins view.
         *
         * The scratch view wrapping coins_tip is populated lazily by
         * coins_view_cache_modify / get_coins, and cvc_batch_write
         * (lib/coins/src/coins_view.c:255) only propagates DIRTY
         * entries to the backing store — the same rule holds for the
         * SQLite flush at coins_view_sqlite.c:664. A bare
         * `coins_map_erase` on the scratch is therefore a no-op with
         * respect to the backing: the tx's outputs survive in
         * coins_tip, and (after the next flush) in SQLite's utxos
         * table, even though the block creating them has been
         * disconnected. Any subsequent reconnect of the same block
         * trips BIP30 on the have_coins fall-through.
         *
         * Fix: materialize the backing entry into the scratch as a
         * DIRTY+pruned tombstone so cvc_batch_write propagates a
         * pruned entry to the parent (which in turn drives the
         * DELETE at coins_view_sqlite.c:667 on the next coins flush).
         * Matches Bitcoin Core's CCoinsViewCache semantics (erase
         * semantics propagate through LevelDB's CDBBatch::Erase),
         * while staying compatible with our DIRTY-driven SQLite
         * flush path. See docs/postmortems/2026-04-19-bip30-stall.md.
         *
         * `coins_view_cache_modify` fetches the backing entry into
         * the scratch on miss; freeing the coins + re-init'ing makes
         * coins_is_pruned return true, so cvc_batch_write takes the
         * pruned branch and the DELETE signal reaches the backing. */
        struct coins_cache_entry *ghost =
            coins_view_cache_modify(view, &tx->hash);
        if (ghost) {
            coins_free(&ghost->coins);
            coins_init(&ghost->coins);
            ghost->flags |= COINS_CACHE_DIRTY;
        }
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
