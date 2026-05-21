/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * disconnect_tip — roll back the active chain by one block.
 *
 * Extracted from process_block_core.c (WS-6 phase 1, file-level split).
 * Pure code motion; function body is byte-identical to its prior site. */

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "validation/process_block.h"
#include "validation/main_logic.h"
#include "validation/connect_block.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "coins/undo.h"
#include "coins/coins.h"
#include "core/core_io.h"
#include "core/serialize.h"
#include "core/uint256.h"
#include "storage/disk_block_io.h"
#include "storage/block_index_db.h"
#include "models/database.h"
#include "controllers/sync_controller.h"
#include "event/event.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include "process_block_internal.h"

bool disconnect_tip(struct validation_state *state,
                    struct main_state *ms,
                    struct coins_view_cache *coins_tip,
                    const char *datadir)
{
    struct block_index *pindex_delete = active_chain_tip(&ms->chain_active);
    if (!pindex_delete)
        LOG_FAIL("validation", "disconnect_tip called with no active chain tip");
    /* Never disconnect genesis (pprev is NULL) */
    if (!pindex_delete->pprev)
        LOG_FAIL("validation", "disconnect_tip refused to disconnect genesis block");

    struct block block;
    block_init(&block);
    if (!read_block_from_disk_index(&block, pindex_delete, datadir)) {
        block_free(&block);
        return validation_state_error(state, "failed-to-read-block");
    }

    /* Read undo data */
    struct block_undo blockundo;
    block_undo_init(&blockundo);

    struct disk_block_pos undo_pos;
    undo_pos.nFile = pindex_delete->nFile;
    undo_pos.nPos = pindex_delete->nUndoPos;

    bool undo_loaded = false;
    if (undo_pos.nPos > 0) {
        disk_block_io_lock();
        FILE *f = open_undo_file(datadir, &undo_pos, true);
        if (f) {
            fseek(f, 0, SEEK_END);
            long file_len = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (file_len > 0 && file_len <= 32 * 1024 * 1024) {
                uint8_t *buf = zcl_malloc((size_t)file_len, "undo_file_buf");
                if (buf) {
                    size_t nread = fread(buf, 1, (size_t)file_len, f); // disk-io-lock: held
                    if (nread > 0) {
                        struct byte_stream s;
                        stream_init_from_data(&s, buf, nread);
                        block_undo_deserialize(&blockundo, &s);
                        undo_loaded = (blockundo.num_txundo > 0);
                    }
                    free(buf);
                }
            }
            disk_block_io_release_handle(f);
        }
        disk_block_io_unlock();
    }

    /* Self-healing: reconstruct undo data from tx index when undo file
     * is missing or corrupt. For each non-coinbase input, look up the
     * source transaction via the tx index and extract the spent output. */
    if (!undo_loaded && block.num_vtx > 1 && g_active_block_tree) {
        printf("[self-heal] Reconstructing undo data for h=%d "
               "(%zu txs) from tx index\n",
               pindex_delete->nHeight, block.num_vtx);
        blockundo.num_txundo = block.num_vtx - 1;
        blockundo.vtxundo = zcl_calloc(blockundo.num_txundo,
                                    sizeof(struct tx_undo), "undo_vtxundo");
        bool reconstruct_ok = (blockundo.vtxundo != NULL);
        for (size_t i = 1; i < block.num_vtx && reconstruct_ok; i++) {
            const struct transaction *tx = &block.vtx[i];
            struct tx_undo *tu = &blockundo.vtxundo[i - 1];
            tu->num_prevout = tx->num_vin;
            tu->vprevout = zcl_calloc(tx->num_vin, sizeof(struct tx_in_undo), "undo_vprevout");
            if (!tu->vprevout) { reconstruct_ok = false; break; }

            for (size_t j = 0; j < tx->num_vin; j++) {
                const struct uint256 *prev_txid = &tx->vin[j].prevout.hash;
                uint32_t prev_n = tx->vin[j].prevout.n;
                struct disk_tx_pos txpos;
                disk_tx_pos_init(&txpos);
                if (!block_tree_db_read_tx_index(g_active_block_tree,
                                                  prev_txid, &txpos) ||
                    txpos.block_pos.nFile < 0) {
                    reconstruct_ok = false; break;
                }
                struct block src_blk;
                block_init(&src_blk);
                if (!read_block_from_disk(&src_blk, &txpos.block_pos,
                                          datadir)) {
                    block_free(&src_blk);
                    reconstruct_ok = false; break;
                }
                bool found = false;
                for (size_t ti = 0; ti < src_blk.num_vtx; ti++) {
                    if (uint256_eq(&src_blk.vtx[ti].hash, prev_txid)) {
                        if (prev_n < src_blk.vtx[ti].num_vout) {
                            /* struct assignment copies the fixed-size
                             * script_pub_key array by value */
                            tu->vprevout[j].txout =
                                src_blk.vtx[ti].vout[prev_n];
                            /* Get source block height */
                            struct uint256 src_hash;
                            block_get_hash(&src_blk, &src_hash);
                            struct block_index *src_idx = block_map_find(
                                &ms->map_block_index, &src_hash);
                            tu->vprevout[j].height = src_idx ?
                                (unsigned int)src_idx->nHeight : 0;
                            tu->vprevout[j].coinbase =
                                transaction_is_coinbase(&src_blk.vtx[ti]);
                            found = true;
                        }
                        break;
                    }
                }
                block_free(&src_blk);
                if (!found) { reconstruct_ok = false; break; }
            }
        }
        if (reconstruct_ok) {
            printf("[self-heal] Undo data reconstructed for h=%d\n",
                   pindex_delete->nHeight);
            undo_loaded = true;
        } else {
            fprintf(stderr, // obs-ok:pre-existing-diagnostic
                    "[self-heal] Failed to reconstruct undo data "
                    "for h=%d\n", pindex_delete->nHeight);
            block_undo_free(&blockundo);
            block_undo_init(&blockundo);
        }
    }

    /* Apply disconnect */
    {
        struct coins_view_cache view;
        struct coins_view backing;
        coins_view_cache_as_view(&backing, coins_tip);
        coins_view_cache_init(&view, &backing);

        if (!disconnect_block(&block, state, pindex_delete, &view, &blockundo)) {
            block_free(&block);
            block_undo_free(&blockundo);
            coins_view_cache_free(&view);
            LOG_FAIL("validation", "disconnect_block failed at height %d",
                     pindex_delete->nHeight);
        }

        if (!coins_view_cache_flush(&view)) {
            fprintf(stderr, // obs-ok:pre-existing-diagnostic
                    "disconnect_tip: FATAL coins flush failed "
                    "h=%d\n", pindex_delete->nHeight);
            coins_view_cache_free(&view);
            block_free(&block);
            block_undo_free(&blockundo);
            return validation_state_error(state, "coins-flush-failed");
        }
        coins_view_cache_free(&view);
    }

    /* Sync disconnect to SQLite */
    {
        struct node_db *ndb = process_block_node_db_internal();
        if (ndb)
            node_db_sync_disconnect_block(ndb,
                                          &block, pindex_delete);
    }

    if (!update_tip(ms, pindex_delete->pprev)) {
        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                "disconnect_tip: update_tip rejected at h=%d — csr "
                "refused the rollback commit (see `csr: REJECTED` "
                "above). Coins view rolled back but in-memory chain "
                "tip did NOT rewind; caller must treat this as a "
                "system error (reorg recovery will observe the "
                "mismatch on next activate_best_chain pass).\n",
                pindex_delete->pprev ? pindex_delete->pprev->nHeight : -1);
        block_free(&block);
        block_undo_free(&blockundo);
        return validation_state_error(state, "csr-tip-rollback-rejected");
    }

    /* Invariant assertion — the coins view must no longer
     * report any tx from the disconnected block.
     *
     * This catches the 2026-04-19 BIP30 stall shape at the boundary
     * where it was silently corrupted: if disconnect_block
     * (`connect_block.c:639`) ever regresses to the bare-erase
     * pattern, or a new path re-introduces stale unspent coinbase
     * entries via some other route, this check surfaces the bug
     * immediately instead of letting it fester until the next
     * connect_block retry trips bad-txns-BIP30 and loops.
     *
     * Debug builds (no NDEBUG) abort so the test suite catches
     * regressions. Release builds log + emit an event so production
     * regressions show up in telemetry without crashing. */
    for (size_t i = 0; i < block.num_vtx; i++) {
        if (coins_view_cache_have_coins(coins_tip, &block.vtx[i].hash)) {
            char hex[65];
            uint256_get_hex(&block.vtx[i].hash, hex);
            fprintf(stderr, // obs-ok:pre-existing-diagnostic
                "disconnect_tip: INVARIANT violated h=%d tx[%zu] %s "
                "still reachable via coins_view_cache_have_coins after "
                "disconnect — see docs/archive/2026-04/2026-04-19-bip30-stall.md\n",
                pindex_delete->nHeight, i, hex);
            event_emitf(EV_UTXO_CHECKPOINT_FAIL, 0,
                "disconnect_tip_invariant h=%d txid=%s",
                pindex_delete->nHeight, hex);
#ifndef NDEBUG
            assert(!"disconnect_tip: coins view retained disconnected tx");
#endif
        }
    }

    block_free(&block);
    block_undo_free(&blockundo);
    return true;
}
