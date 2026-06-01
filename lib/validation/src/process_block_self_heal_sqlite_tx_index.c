/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * SQLite tx-index recovery source for missing-UTXO self-heal.
 *
 * This source trusts the runtime-owned TxIndex model lookup only as a hint:
 * the source block must exist in block_map, be consensus-backed on disk, and
 * contain the requested txid before any UTXO is injected. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "chain/chain.h"
#include "config/runtime.h"
#include "storage/disk_block_io.h"

#include "process_block_internal.h"

bool chain_restore_block_is_consensus_backed_on_disk(
    const struct block_index *tip,
    const char *datadir);

bool process_block_recover_missing_utxo_from_sqlite_tx_index(
    struct main_state *ms,
    struct coins_view_cache *coins_tip,
    const struct uint256 *txid,
    uint32_t missing_vout,
    const char *datadir,
    int retry_no)
{
    struct node_db *ndb = process_block_node_db_internal();
    if (!ms || !coins_tip || !txid || !datadir || !ndb)
        return false;

    struct app_runtime_tx_index_hit dbtx;
    if (!app_runtime_node_db_tx_index_find(ndb, txid->data, &dbtx))
        return false;

    struct uint256 block_hash;
    memcpy(block_hash.data, dbtx.block_hash, sizeof(block_hash.data));
    struct block_index *src_idx =
        block_map_find(&ms->map_block_index, &block_hash);
    if (!src_idx) {
        char txhex[65];
        uint256_get_hex(txid, txhex);
        fprintf(stderr, "[self-heal] SQLite tx index hit for %s but "
                "source block is absent from block_map (height=%d)\n",
                txhex, dbtx.block_height);
        return false;
    }

    if (src_idx->nHeight != dbtx.block_height) {
        char txhex[65];
        uint256_get_hex(txid, txhex);
        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                "[self-heal] SQLite tx index height mismatch for %s: "
                "db=%d index=%d; using hash-verified block index\n",
                txhex, dbtx.block_height, src_idx->nHeight);
    }

    if (!chain_restore_block_is_consensus_backed_on_disk(src_idx, datadir)) {
        char txhex[65];
        uint256_get_hex(txid, txhex);
        fprintf(stderr, "[self-heal] SQLite tx index hit for %s points to "
                "non-verified disk block h=%d file=%d pos=%u\n",
                txhex, src_idx->nHeight, src_idx->nFile, src_idx->nDataPos);
        return false;
    }

    struct block src_block;
    block_init(&src_block);
    if (!read_block_from_disk_index(&src_block, src_idx, datadir)) {
        block_free(&src_block);
        return false;
    }

    bool recovered = false;
    if (dbtx.tx_index >= 0 && (size_t)dbtx.tx_index < src_block.num_vtx &&
        uint256_eq(&src_block.vtx[dbtx.tx_index].hash, txid)) {
        recovered = process_block_inject_missing_utxo(
            coins_tip, txid, missing_vout, &src_block.vtx[dbtx.tx_index],
            src_idx->nHeight, "SQLite tx index", retry_no);
    } else {
        for (size_t ti = 0; ti < src_block.num_vtx; ti++) {
            if (!uint256_eq(&src_block.vtx[ti].hash, txid))
                continue;
            recovered = process_block_inject_missing_utxo(
                coins_tip, txid, missing_vout, &src_block.vtx[ti],
                src_idx->nHeight, "SQLite tx index scan", retry_no);
            break;
        }
    }

    if (recovered && dbtx.used_reversed) {
        char txhex[65];
        uint256_get_hex(txid, txhex);
        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                "[self-heal] SQLite tx index recovered %s using %s lookup "
                "after local block/tx hash verification\n",
                txhex, "reversed");
    }

    block_free(&src_block);
    return recovered;
}
