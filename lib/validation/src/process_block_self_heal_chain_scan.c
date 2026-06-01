/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Bounded active-chain scan recovery source for missing-UTXO self-heal.
 *
 * When the SQLite tx index does not contain the missing transaction, this
 * source walks the verified active chain backwards within the configured
 * bound, injects a found transaction's outputs, and opportunistically
 * backfills the LevelDB tx index for the next lookup. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>

#include "chain/chain.h"
#include "event/event.h"
#include "storage/block_index_db.h"
#include "storage/disk_block_io.h"
#include "storage/txdb.h"

#include "process_block_internal.h"

bool process_block_recover_missing_utxo_from_chain_scan(
    struct main_state *ms,
    struct coins_view_cache *coins_tip,
    const struct uint256 *txid,
    uint32_t vout,
    const char *datadir,
    int retry_no)
{
    char hex[65];
    uint256_get_hex(txid, hex);

    /* The tx index can be empty for this tx because LDB fast-sync imports
     * UTXOs but doesn't populate block_tree_db's tx-offset entries. Before
     * surrendering the block as BLOCK_FAILED_VALID, walk the active chain
     * backward a bounded number of blocks and search each for the missing
     * txid. If found, inject its outputs into the coins cache and backfill
     * the tx_index entry so the next spend of the same tx is O(log N).
     *
     * 2026-05-10 stalls: live imports have needed UTXOs 150k-200k blocks
     * behind tip after partial chainstate recovery. Default to a deep bounded
     * scan and keep ZCL_SELF_HEAL_SCAN_DEPTH as an operator override for
     * deeper exceptional repairs. Lower values are ignored because they make
     * the live recovery path fail open into a restart loop. */
    int tip_h = active_chain_height(&ms->chain_active);
    int depth_limit = process_block_self_heal_scan_depth_limit();
    int scan_stop = (tip_h - depth_limit < 0) ? 0 : tip_h - depth_limit;

    bool recovered = false;
    bool scan_hit = false;
    int scan_blocks_checked = 0;
    int scan_hit_height = -1;
    for (int h = tip_h; h >= scan_stop && !scan_hit; h--) {
        struct block_index *bi = active_chain_at(&ms->chain_active, h);
        if (!bi || !(bi->nStatus & BLOCK_HAVE_DATA))
            continue;
        scan_blocks_checked++;
        struct block scan_b;
        block_init(&scan_b);
        if (!read_block_from_disk_index(&scan_b, bi, datadir)) {
            block_free(&scan_b);
            continue;
        }
        for (size_t ti = 0; ti < scan_b.num_vtx; ti++) {
            if (!uint256_eq(&scan_b.vtx[ti].hash, txid))
                continue;
            if (process_block_inject_missing_utxo(
                    coins_tip, txid, vout,
                    &scan_b.vtx[ti], h,
                    "verified chain scan", retry_no)) {
                scan_hit = true;
                scan_hit_height = h;
                struct disk_tx_pos tx_new;
                disk_tx_pos_init(&tx_new);
                tx_new.block_pos.nFile = bi->nFile;
                tx_new.block_pos.nPos = bi->nDataPos;
                (void)block_tree_db_write_tx_index(
                    g_active_block_tree, txid, &tx_new, 1);
            }
            break;
        }
        block_free(&scan_b);
    }

    if (scan_hit) {
        atomic_fetch_add_explicit(
            &g_self_heal_scan_hits, 1,
            memory_order_relaxed);
        atomic_fetch_add_explicit(
            &g_self_heal_scan_blocks_checked_total,
            (uint64_t)scan_blocks_checked,
            memory_order_relaxed);
        printf("[self-heal] RECOVERED UTXO %s via "
               "chain scan (hit_h=%d, depth=%d, "
               "blocks_checked=%d) — retry %d\n",
               hex, scan_hit_height,
               tip_h - scan_hit_height,
               scan_blocks_checked, retry_no);
        fflush(stdout);
        event_emitf(EV_SELF_HEAL_SCAN_HIT, 0,
            "tx=%s h=%d depth=%d",
            hex, scan_hit_height,
            tip_h - scan_hit_height);
        recovered = true;
    } else {
        atomic_fetch_add_explicit(
            &g_self_heal_scan_exhausted, 1,
            memory_order_relaxed);
        atomic_fetch_add_explicit(
            &g_self_heal_scan_blocks_checked_total,
            (uint64_t)scan_blocks_checked,
            memory_order_relaxed);
        fprintf(stderr, // obs-ok:pre-existing-diagnostic
            "[self-heal] scan exhausted "
            "(tx=%s, tip_h=%d, depth_limit=%d, "
            "blocks_checked=%d) — no match\n",
            hex, tip_h, depth_limit,
            scan_blocks_checked);
        event_emitf(EV_SELF_HEAL_SCAN_EXHAUSTED, 0,
            "tx=%s tip_h=%d depth=%d",
            hex, tip_h, depth_limit);
    }

    return recovered;
}
