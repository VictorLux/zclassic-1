/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * update_coins — applies a transaction's effects to the UTXO set.
 * Spends inputs (removes UTXOs), creates outputs (adds UTXOs).
 * Maintains undo data for chain reorgs.
 *
 * PEDANTIC: every error path logs loudly and returns false.
 * Silent failures here cause UTXO set corruption. */

#include "validation/update_coins.h"
#include "coins/utxo_commitment.h"
#include "domain/consensus/coins_math.h"
#include "storage/utxo_projection.h"
#include "util/log_macros.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util/safe_alloc.h"

/* UTXO projection-emission counters. These mirror the
 * utxo_projection internals but are owned by the emitter side so an
 * operator can spot a divergence (emit-side count vs consume-side count)
 * without grepping two subsystems. Reset on every process restart. */
#include <stdatomic.h>
static _Atomic uint64_t g_utxo_event_emit_total      = 0;
static _Atomic uint64_t g_utxo_event_emit_fail_total = 0;

uint64_t update_coins_event_emit_total(void)
{
    return atomic_load_explicit(&g_utxo_event_emit_total,
                                memory_order_relaxed);
}
uint64_t update_coins_event_emit_fail_total(void)
{
    return atomic_load_explicit(&g_utxo_event_emit_fail_total,
                                memory_order_relaxed);
}

void update_coins_emit_utxo_add_projection(const uint8_t txid[32], uint32_t vout,
                                  int64_t value, uint32_t height,
                                  bool is_coinbase,
                                  const uint8_t *script_bytes,
                                  uint32_t script_len)
{
    /* B3: legacy authors only while LEGACY. Once utxo_apply_stage is
     * the authority (STAGE), the stage emits and we yield so there is
     * exactly one writer to the projection. */
    if (utxo_projection_get_author() != UTXO_AUTHOR_LEGACY)
        return;
    /* Projection emission: failures NEVER gate the legacy SQLite write.
     * obs-ok marker downgrades the warning to "operator can grep
     * later" rather than "abort consensus path". */
    if (utxo_projection_emit_add(txid, vout, value, height, is_coinbase,
                                  script_bytes, script_len)) {
        atomic_fetch_add_explicit(&g_utxo_event_emit_total, 1,
                                  memory_order_relaxed);
    } else if (utxo_projection_event_log() != NULL) {
        /* Only counted as a failure when projection emission is actually
         * enabled (event log non-NULL). A NULL log is the legitimate
         * "event log not wired yet" case, not an error. */
        atomic_fetch_add_explicit(&g_utxo_event_emit_fail_total, 1,
                                  memory_order_relaxed);
        fprintf(stderr,  // obs-ok:utxo-event-emit-failure
                "[update_coins] projection emit_add failed h=%u\n", height);
    }
}

void update_coins_emit_utxo_spend_projection(const uint8_t txid[32], uint32_t vout)
{
    /* B3: see update_coins_emit_utxo_add_projection — yield once the stage
     * is the authority so the projection has a single writer. */
    if (utxo_projection_get_author() != UTXO_AUTHOR_LEGACY)
        return;
    if (utxo_projection_emit_spend(txid, vout)) {
        atomic_fetch_add_explicit(&g_utxo_event_emit_total, 1,
                                  memory_order_relaxed);
    } else if (utxo_projection_event_log() != NULL) {
        atomic_fetch_add_explicit(&g_utxo_event_emit_fail_total, 1,
                                  memory_order_relaxed);
        fprintf(stderr,  // obs-ok:utxo-event-emit-failure
                "[update_coins] projection emit_spend failed\n");
    }
}

bool update_coins_with_undo(const struct transaction *tx,
                            struct coins_view_cache *inputs,
                            struct tx_undo *txundo,
                            int nHeight)
{
    if (!transaction_is_coinbase(tx)) {
        if (!tx_undo_alloc(txundo, tx->num_vin))
            LOG_FAIL("update_coins", "tx_undo_alloc failed num_vin=%zu h=%d",
                     tx->num_vin, nHeight);
        for (size_t i = 0; i < tx->num_vin; i++) {
            struct coins_cache_entry *entry =
                coins_view_cache_modify(inputs, &tx->vin[i].prevout.hash);
            if (!entry) {
                char hex[65];
                uint256_get_hex(&tx->vin[i].prevout.hash, hex);
                LOG_FAIL("update_coins", "coins_modify failed input[%zu]=%s h=%d",
                         i, hex, nHeight);
            }
            unsigned int nPos = tx->vin[i].prevout.n;

            /* Grow vout array if needed */
            if (nPos >= entry->coins.num_vout) {
                size_t new_size = nPos + 1;
                struct tx_out *nv = zcl_realloc(entry->coins.vout,
                    new_size * sizeof(struct tx_out), "coins_vout_grow");
                if (!nv)
                    LOG_FAIL("update_coins", "realloc failed new_size=%zu h=%d",
                             new_size, nHeight);
                for (size_t k = entry->coins.num_vout; k < new_size; k++)
                    tx_out_set_null(&nv[k]);
                entry->coins.vout = nv;
                entry->coins.num_vout = new_size;
            }
            if (tx_out_is_null(&entry->coins.vout[nPos])) {
                char hex[65];
                uint256_get_hex(&tx->vin[i].prevout.hash, hex);
                LOG_FAIL("update_coins",
                         "spending NULL output %s:%u at h=%d (double-spend or missing UTXO)",
                         hex, nPos, nHeight);
            }

            /* Validate output value before spending */
            if (!MoneyRange(entry->coins.vout[nPos].value))
                LOG_FAIL("update_coins", "corrupt output value %lld at h=%d",
                         (long long)entry->coins.vout[nPos].value, nHeight);

            /* Remove spent UTXO from commitment */
            utxo_commitment_remove(&inputs->commitment,
                                    tx->vin[i].prevout.hash.data, nPos,
                                    entry->coins.vout[nPos].value,
                                    entry->coins.height);

            /* Projection emission: also append EV_UTXO_SPEND to
             * the event_log so utxo_projection can derive the same
             * spend. Additive — does not gate the legacy path. */
            update_coins_emit_utxo_spend_projection(tx->vin[i].prevout.hash.data,
                                                nPos);

            /* Pure domain mutation: snapshot the txout into the undo
             * record, null the vout, and (if the coin is now fully
             * pruned) populate the parent metadata so a reorg can
             * rebuild it. This is the slice of update_coins that does
             * not touch the cache. Range / liveness preconditions
             * were already established above (we grew the array and
             * logged NULL-out / corrupt-value with LOG_FAIL); the
             * domain call cannot fail in this code path. */
            struct zcl_result _cu = coins_math_capture_undo(
                    &entry->coins, nPos, &txundo->vprevout[i]);
            if (!_cu.ok)
                LOG_FAIL("update_coins",
                         "coins_math_capture_undo failed code=%d msg=%s h=%d",
                         _cu.code, _cu.message, nHeight);
        }
    }

    /* Create new outputs */
    struct coins_cache_entry *new_entry =
        coins_view_cache_modify_new(inputs, &tx->hash);
    if (!new_entry)
        LOG_FAIL("update_coins", "modify_new failed at h=%d", nHeight);
    coins_from_transaction(&new_entry->coins, tx, nHeight);

    /* Validate new output values before adding to commitment */
    for (size_t vi = 0; vi < new_entry->coins.num_vout; vi++) {
        if (!tx_out_is_null(&new_entry->coins.vout[vi])) {
            if (!MoneyRange(new_entry->coins.vout[vi].value))
                LOG_FAIL("update_coins",
                         "new output[%zu] value %lld out of range at h=%d",
                         vi, (long long)new_entry->coins.vout[vi].value, nHeight);
            utxo_commitment_add(&inputs->commitment,
                                 tx->hash.data, (uint32_t)vi,
                                 new_entry->coins.vout[vi].value,
                                 nHeight);

            /* Projection emission: also append EV_UTXO_ADD to
             * the event_log so utxo_projection can derive the same
             * UTXO. Additive — does not gate the legacy path. */
            update_coins_emit_utxo_add_projection(tx->hash.data, (uint32_t)vi,
                                  new_entry->coins.vout[vi].value,
                                  (uint32_t)nHeight,
                                  new_entry->coins.is_coinbase,
                                  new_entry->coins.vout[vi].script_pub_key.data,
                                  (uint32_t)new_entry->coins.vout[vi].script_pub_key.size);
        }
    }

    return true;
}

void update_coins(const struct transaction *tx,
                  struct coins_view_cache *inputs,
                  int nHeight)
{
    struct tx_undo txundo;
    tx_undo_init(&txundo);
    update_coins_with_undo(tx, inputs, &txundo, nHeight);
    tx_undo_free(&txundo);
}
