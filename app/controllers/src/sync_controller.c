/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "controllers/sync_controller.h"
#include "models/wallet_key.h"
#include "models/wallet_tx.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "chain/chain.h"
#include "wallet/wallet.h"
#include "wallet/keystore.h"
#include "wallet/sapling_keys.h"
#include "keys/key.h"
#include "core/hash.h"
#include "core/serialize.h"
#include "script/standard.h"
#include "storage/disk_block_io.h"
#include "storage/dbwrapper.h"
#include "storage/coins_db.h"
#include "coins/undo.h"
#include "validation/chainstate.h"
#include "validation/txmempool.h"
#include "sapling/incremental_merkle_tree.h"
#include "sapling/sapling.h"
#include "sapling/note_encryption.h"
#include "support/cleanse.h"
#include "event/event.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdatomic.h>
#include <pthread.h>

_Atomic bool g_sapling_rescan_active = false;

bool node_db_sync_init(struct node_db *ndb, const char *datadir)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/node.db", datadir);
    if (!node_db_open(ndb, path)) {
        fprintf(stderr, "node_db_sync: failed to open %s\n", path);
        return false;
    }
    printf("SQLite database opened: %s (schema v%d)\n",
           path, node_db_schema_version(ndb));
    return true;
}

/* Extract the 20-byte address hash from a scriptPubKey.
 * Returns script type and fills addr_hash if applicable. */
static enum script_type classify_script(const uint8_t *script,
                                        size_t script_len,
                                        uint8_t addr_hash[20],
                                        bool *has_addr)
{
    *has_addr = false;

    /* P2PKH: OP_DUP OP_HASH160 <20> <hash> OP_EQUALVERIFY OP_CHECKSIG */
    if (script_len == 25 &&
        script[0] == 0x76 && script[1] == 0xa9 &&
        script[2] == 0x14 &&
        script[23] == 0x88 && script[24] == 0xac) {
        memcpy(addr_hash, script + 3, 20);
        *has_addr = true;
        return SCRIPT_P2PKH;
    }

    /* P2SH: OP_HASH160 <20> <hash> OP_EQUAL */
    if (script_len == 23 &&
        script[0] == 0xa9 && script[1] == 0x14 &&
        script[22] == 0x87) {
        memcpy(addr_hash, script + 2, 20);
        *has_addr = true;
        return SCRIPT_P2SH;
    }

    /* OP_RETURN */
    if (script_len > 0 && script[0] == 0x6a)
        return SCRIPT_OP_RETURN;

    return SCRIPT_OTHER;
}

/* Serialize a transaction to raw bytes. Caller must free. */
static uint8_t *serialize_tx(const struct transaction *tx,
                             size_t *out_len)
{
    struct byte_stream s;
    stream_init(&s, 512);
    transaction_serialize(tx, &s);
    *out_len = s.size;
    return s.data;
}

/* Advance Sapling commitment tree and all wallet witnesses for one block.
 * Creates initial witnesses for wallet notes whose cm appears in this block.
 * Saves updated tree and witnesses to SQLite. */
static void advance_wallet_witnesses(struct node_db *ndb,
                                     const struct block *blk,
                                     struct incremental_merkle_tree *tree,
                                     int height)
{
    /* Load unspent notes that may need initial witnesses */
    struct db_sapling_note wnotes[256];
    int nw = db_sapling_note_list_unspent(ndb, wnotes, 256);

    /* Track which notes already have witnesses (for initial witness creation) */
    bool has_witness[256];
    for (int i = 0; i < nw; i++) {
        uint8_t *wblob = NULL;
        size_t wlen = 0;
        int wh = 0;
        has_witness[i] = db_sapling_note_load_witness(ndb,
            wnotes[i].txid, wnotes[i].output_index,
            &wblob, &wlen, &wh) && wblob;
        if (wblob) free(wblob);
    }

    /* Deserialize existing witnesses for advancement */
    struct incremental_witness *witnesses = NULL;
    int num_with_witness = 0;
    int *witness_idx = NULL; /* maps witness array → wnotes index */
    if (nw > 0) {
        witnesses = calloc((size_t)nw, sizeof(struct incremental_witness));
        witness_idx = calloc((size_t)nw, sizeof(int));
        for (int i = 0; i < nw; i++) {
            if (!has_witness[i]) continue;
            uint8_t *wblob = NULL;
            size_t wlen = 0;
            int wh = 0;
            if (!db_sapling_note_load_witness(ndb,
                    wnotes[i].txid, wnotes[i].output_index,
                    &wblob, &wlen, &wh) || !wblob)
                continue;
            struct byte_stream ws;
            stream_init_from_data(&ws, wblob, wlen);
            if (incremental_witness_deserialize(&witnesses[num_with_witness],
                    &ws, SAPLING_INCREMENTAL_MERKLE_TREE_DEPTH,
                    tree->combine, tree->uncommitted)) {
                witness_idx[num_with_witness] = i;
                num_with_witness++;
            }
            free(wblob);
        }
    }

    /* Append each Sapling output commitment */
    bool has_sapling_outputs = false;
    for (size_t i = 0; i < blk->num_vtx; i++) {
        const struct transaction *tx = &blk->vtx[i];
        for (size_t j = 0; j < tx->num_shielded_output; j++) {
            const struct uint256 *cm = &tx->v_shielded_output[j].cm;

            /* Advance all existing witnesses */
            for (int wi = 0; wi < num_with_witness; wi++)
                incremental_witness_append(&witnesses[wi], cm);

            /* Append to tree */
            incremental_tree_append(tree, cm);
            has_sapling_outputs = true;

            /* Check if cm matches a wallet note without a witness */
            for (int wi = 0; wi < nw; wi++) {
                if (has_witness[wi]) continue;
                if (memcmp(wnotes[wi].cm, cm->data, 32) != 0) continue;
                /* Create initial witness from current tree state */
                struct incremental_witness iw;
                incremental_witness_init(&iw, tree);
                struct byte_stream iwout;
                stream_init(&iwout, 2048);
                incremental_witness_serialize(&iw, &iwout);
                db_sapling_note_save_witness(ndb,
                    wnotes[wi].txid, wnotes[wi].output_index,
                    iwout.data, iwout.size, height);
                stream_free(&iwout);
                has_witness[wi] = true;
                /* Add to witness advancement array for subsequent cms */
                if (witnesses) {
                    witnesses[num_with_witness] = iw;
                    witness_idx[num_with_witness] = wi;
                    num_with_witness++;
                }
                break;
            }
        }
    }

    /* Save all advanced witnesses */
    if (has_sapling_outputs) {
        for (int wi = 0; wi < num_with_witness; wi++) {
            struct byte_stream wout;
            stream_init(&wout, 2048);
            incremental_witness_serialize(&witnesses[wi], &wout);
            int idx = witness_idx[wi];
            db_sapling_note_save_witness(ndb,
                wnotes[idx].txid, wnotes[idx].output_index,
                wout.data, wout.size, height);
            stream_free(&wout);
        }
    }

    /* Save tree to node_state */
    {
        struct byte_stream ts;
        stream_init(&ts, 4096);
        incremental_tree_serialize(tree, &ts);
        node_db_state_set(ndb, "sapling_tree", ts.data, ts.size);
        stream_free(&ts);
    }

    free(witnesses);
    free(witness_idx);
}

bool node_db_sync_connect_block(struct node_db *ndb,
                                const struct block *blk,
                                const struct block_index *pindex)
{
    if (!ndb->open) return false;

    /* Batch mode: start transaction if not already in one */
    if (!ndb->sync_in_batch) {
        node_db_begin(ndb);
        ndb->sync_in_batch = true;
        ndb->sync_pending_blocks = 0;
    }

    /* 1. Index the block header */
    struct db_block db_blk;
    memset(&db_blk, 0, sizeof(db_blk));
    memcpy(db_blk.hash, pindex->phashBlock->data, 32);
    db_blk.height = pindex->nHeight;
    /* Get prev_hash from block header (always available) rather than
     * pprev pointer (may be NULL if block_index has gaps) */
    memcpy(db_blk.prev_hash, blk->header.hashPrevBlock.data, 32);
    db_blk.version = blk->header.nVersion;
    memcpy(db_blk.merkle_root,
           blk->header.hashMerkleRoot.data, 32);
    db_blk.time = blk->header.nTime;
    db_blk.bits = blk->header.nBits;
    memcpy(db_blk.nonce, blk->header.nNonce.data, 32);
    db_blk.solution = (uint8_t *)blk->header.nSolution;
    db_blk.solution_len = blk->header.nSolutionSize;
    memcpy(db_blk.chain_work, pindex->nChainWork.pn, 32);
    db_blk.status = pindex->nStatus;
    db_blk.file_num = pindex->nFile;
    db_blk.data_pos = (int)pindex->nDataPos;
    db_blk.num_tx = (int)blk->num_vtx;

    if (!db_block_save(ndb, &db_blk)) {
        /* Don't rollback — continue with tx/UTXO indexing.
         * Block header save can fail due to SQLite lock contention
         * with the catchup thread. UTXOs are more important. */
    }

    /* 2. Index each transaction and update UTXOs */
    for (size_t i = 0; i < blk->num_vtx; i++) {
        const struct transaction *tx = &blk->vtx[i];

        /* Index the transaction */
        struct db_tx_index db_tx;
        memset(&db_tx, 0, sizeof(db_tx));
        memcpy(db_tx.txid, tx->hash.data, 32);
        memcpy(db_tx.block_hash,
               pindex->phashBlock->data, 32);
        db_tx.block_height = pindex->nHeight;
        db_tx.tx_index = (int)i;
        db_tx.file_num = pindex->nFile;
        db_tx.file_pos = (int)pindex->nDataPos;
        db_tx.is_coinbase = (i == 0);

        if (!db_tx_save(ndb, &db_tx)) {
            node_db_rollback(ndb);
            return false;
        }

        /* UTXOs are managed by coins_view_sqlite (the canonical UTXO store).
         * Do NOT write UTXOs here — sync_controller handles blocks, txs,
         * sapling nullifiers, and wallet scanning only. */

        /* Track Sapling nullifiers (spends) */
        for (size_t j = 0; j < tx->num_shielded_spend; j++) {
            sqlite3_stmt *ns = ndb->stmt_nullifier_insert;
            sqlite3_reset(ns);
            sqlite3_bind_blob(ns, 1,
                tx->v_shielded_spend[j].nullifier.data,
                32, SQLITE_STATIC);
            sqlite3_step(ns);
        }
    }

    /* 3. Update Sapling commitment tree + maintain wallet witnesses */
    {
        struct incremental_merkle_tree tree;
        sapling_tree_init(&tree);
        uint8_t tree_buf[8192];
        size_t tree_len = 0;
        if (node_db_state_get(ndb, "sapling_tree", tree_buf, sizeof(tree_buf), &tree_len)
            && tree_len > 0) {
            struct byte_stream ts;
            stream_init_from_data(&ts, tree_buf, tree_len);
            sapling_tree_init(&tree);
            incremental_tree_deserialize(&tree, &ts);
        }

        advance_wallet_witnesses(ndb, blk, &tree, pindex->nHeight);

        /* Verify tree root matches block header.
         * During catchup from genesis (tree rebuilding), we may not have
         * the correct tree state yet. Only enforce after Sapling activation
         * and when we have a non-empty tree that should match. */
        struct uint256 tree_root;
        incremental_tree_root(&tree, &tree_root);
        if (memcmp(tree_root.data,
                   blk->header.hashFinalSaplingRoot.data, 32) != 0) {
            /* Check if this is an expected mismatch during IBD/catchup:
             * the tree is being rebuilt and hasn't caught up yet. */
            bool is_ibd = (sync_get_state() <= SYNC_BLOCKS_DOWNLOAD);

            /* If we have the correct tree data stored for this block's
             * parent, load it and retry. Otherwise during IBD, continue
             * and let the tree converge. At tip, log but don't reject —
             * the tree may need one cycle to resync after restart. */
            static int mismatch_count = 0;
            mismatch_count++;
            if (!is_ibd && mismatch_count > 50) {
                fprintf(stderr, "CRITICAL: Sapling tree root MISMATCH "
                    "at height %d (tree_size=%zu, %d mismatches) "
                    "— rejecting block\n",
                    pindex->nHeight, incremental_tree_size(&tree),
                    mismatch_count);
                fflush(stderr);
                return false;
            }
            if (!is_ibd && mismatch_count <= 50) {
                fprintf(stderr, "WARNING: Sapling tree root mismatch "
                    "at height %d — rebuilding (%d/50)\n",
                    pindex->nHeight, mismatch_count);
            }
            /* During IBD: log but continue — the tree will converge
             * once we process all blocks sequentially */
        }

        /* Store tree state per-block for disconnect */
        struct byte_stream ts;
        stream_init(&ts, 4096);
        incremental_tree_serialize(&tree, &ts);
        sqlite3_stmt *upd = NULL;
        sqlite3_prepare_v2(ndb->db,
            "UPDATE blocks SET sapling_tree_data=? WHERE hash=?",
            -1, &upd, NULL);
        sqlite3_bind_blob(upd, 1, ts.data, (int)ts.size, SQLITE_STATIC);
        sqlite3_bind_blob(upd, 2, pindex->phashBlock->data, 32, SQLITE_STATIC);
        sqlite3_step(upd);
        sqlite3_finalize(upd);
        stream_free(&ts);
    }

    /* 4. Update chain tip in state table */
    node_db_sync_set_tip(ndb,
        pindex->phashBlock->data, pindex->nHeight);

    /* Batch mode: commit only when batch_size reached */
    ndb->sync_pending_blocks++;
    int batch = ndb->sync_batch_size > 0 ? ndb->sync_batch_size : 1;
    if (ndb->sync_pending_blocks >= batch) {
        node_db_commit(ndb);
        ndb->sync_in_batch = false;
        ndb->sync_pending_blocks = 0;
    }
    return true;
}

bool node_db_sync_disconnect_block(struct node_db *ndb,
                                   const struct block *blk,
                                   const struct block_index *pindex)
{
    if (!ndb->open) return false;

    /* Flush any pending batch before disconnecting — disconnect needs
     * accurate SQLite state for UTXO restoration */
    node_db_sync_flush(ndb);

    node_db_begin(ndb);

    /* Remove transactions in reverse order */
    for (size_t i = blk->num_vtx; i > 0; i--) {
        const struct transaction *tx = &blk->vtx[i - 1];

        /* Remove wallet UTXOs created by this tx.
         * Note: consensus UTXOs are handled by coins_view_sqlite,
         * not sync_controller. */
        for (size_t j = 0; j < tx->num_vout; j++) {
            db_wallet_utxo_delete(ndb, tx->hash.data, (uint32_t)j);
        }

        /* Unmark any wallet_utxos that this tx spent.
         * When disconnecting, inputs that were marked spent become
         * unspent again (the spending tx is being reverted). */
        for (size_t j = 0; j < tx->num_vin; j++) {
            sqlite3_stmt *us = NULL;
            sqlite3_prepare_v2(ndb->db,
                "UPDATE wallet_utxos SET spent_txid=NULL, spent_vin=NULL"
                " WHERE spent_txid=? AND spent_vin=?",
                -1, &us, NULL);
            if (us) {
                sqlite3_bind_blob(us, 1, tx->hash.data, 32, SQLITE_STATIC);
                sqlite3_bind_int(us, 2, (int)j);
                sqlite3_step(us);
                sqlite3_finalize(us);
            }
        }

        /* Remove tx index entry */
        db_tx_delete(ndb, tx->hash.data);

        /* Remove Sapling nullifiers */
        for (size_t j = 0; j < tx->num_shielded_spend; j++) {
            sqlite3_stmt *s = NULL;
            sqlite3_prepare_v2(ndb->db,
                "DELETE FROM sapling_nullifiers"
                " WHERE nullifier=?",
                -1, &s, NULL);
            sqlite3_bind_blob(s, 1,
                tx->v_shielded_spend[j].nullifier.data,
                32, SQLITE_STATIC);
            sqlite3_step(s);
            sqlite3_finalize(s);
        }

        /* Note: restoring spent UTXOs (inputs consumed by this
         * block's txs) requires undo data. The coins_view_cache
         * path handles that; SQLite UTXOs are repopulated on
         * reconnect via connect_block. */
    }

    /* Restore Sapling tree from previous block */
    if (pindex->pprev) {
        sqlite3_stmt *tq = NULL;
        sqlite3_prepare_v2(ndb->db,
            "SELECT sapling_tree_data FROM blocks WHERE hash=?",
            -1, &tq, NULL);
        sqlite3_bind_blob(tq, 1, pindex->pprev->phashBlock->data, 32, SQLITE_STATIC);
        if (sqlite3_step(tq) == SQLITE_ROW) {
            int tlen = sqlite3_column_bytes(tq, 0);
            const void *tdata = sqlite3_column_blob(tq, 0);
            if (tdata && tlen > 0)
                node_db_state_set(ndb, "sapling_tree", tdata, (size_t)tlen);
        }
        sqlite3_finalize(tq);
    } else {
        /* No previous block — reset to empty tree */
        struct incremental_merkle_tree empty;
        sapling_tree_init(&empty);
        struct byte_stream es;
        stream_init(&es, 256);
        incremental_tree_serialize(&empty, &es);
        node_db_state_set(ndb, "sapling_tree", es.data, es.size);
        stream_free(&es);
    }

    /* Remove block */
    db_block_delete(ndb, pindex->phashBlock->data);

    /* Update tip to previous block */
    if (pindex->pprev) {
        node_db_sync_set_tip(ndb,
            pindex->pprev->phashBlock->data,
            pindex->pprev->nHeight);
    }

    node_db_commit(ndb);
    return true;
}

bool node_db_sync_wallet_tx(struct node_db *ndb,
                            const struct transaction *tx,
                            const struct wallet *w,
                            int block_height)
{
    if (!ndb->open) return false;

    bool is_ours = false;
    bool from_me = false;
    int64_t debit = 0;

    /* Track outputs that belong to us */
    for (size_t i = 0; i < tx->num_vout; i++) {
        const struct tx_out *out = &tx->vout[i];
        uint8_t addr_hash[20];
        bool has_addr = false;
        enum script_type stype = classify_script(
            out->script_pub_key.data,
            out->script_pub_key.size,
            addr_hash, &has_addr);

        if (!has_addr) continue;

        /* Check ownership: P2PKH checks keys, P2SH checks scripts */
        bool owned = false;
        struct key_id kid;
        memcpy(kid.id.data, addr_hash, 20);
        if (stype == SCRIPT_P2PKH)
            owned = keystore_have_key(&w->keystore, &kid);
        else if (stype == SCRIPT_P2SH) {
            struct uint160 sh;
            memcpy(sh.data, addr_hash, 20);
            owned = keystore_have_cscript(&w->keystore, &sh);
        }
        if (!owned) continue;

        is_ours = true;

        struct db_wallet_utxo wu;
        memset(&wu, 0, sizeof(wu));
        memcpy(wu.txid, tx->hash.data, 32);
        wu.vout = (uint32_t)i;
        wu.value = out->value;
        memcpy(wu.address_hash, addr_hash, 20);
        wu.script = (uint8_t *)out->script_pub_key.data;
        wu.script_len = out->script_pub_key.size;
        wu.height = block_height;
        wu.is_coinbase = (tx->num_vin == 1 &&
            tx->vin[0].prevout.n == 0xFFFFFFFF);

        db_wallet_utxo_save(ndb, &wu);
    }

    /* Mark inputs that spend our UTXOs.
     * Only mark spent if the UTXO actually exists in wallet_utxos —
     * otherwise we'd silently set is_ours on non-wallet UTXOs. */
    for (size_t i = 0; i < tx->num_vin; i++) {
        struct db_wallet_utxo spent;
        if (db_wallet_utxo_find(ndb,
                tx->vin[i].prevout.hash.data,
                tx->vin[i].prevout.n,
                &spent)) {
            debit += spent.value;
            from_me = true;
            free(spent.script);

            if (db_wallet_utxo_mark_spent(ndb,
                    tx->vin[i].prevout.hash.data,
                    tx->vin[i].prevout.n,
                    tx->hash.data, (int)i))
                is_ours = true;
        }
    }

    /* Only save the full tx if it involves our wallet */
    if (is_ours) {
        size_t raw_len = 0;
        uint8_t *raw = serialize_tx(tx, &raw_len);
        if (raw) {
            struct db_wallet_tx wtx;
            memset(&wtx, 0, sizeof(wtx));
            memcpy(wtx.txid, tx->hash.data, 32);
            wtx.raw_tx = raw;
            wtx.raw_tx_len = raw_len;
            wtx.has_block = (block_height > 0);
            wtx.block_height = block_height;
            if (wtx.has_block) {
                struct db_block blk;
                if (db_block_find_by_height(ndb, block_height, &blk)) {
                    memcpy(wtx.block_hash, blk.hash, 32);
                    wtx.time_received = (int64_t)blk.time;
                } else {
                    wtx.time_received = (int64_t)time(NULL);
                }
            } else {
                wtx.time_received = (int64_t)time(NULL);
            }
            wtx.from_me = from_me;
            if (from_me) {
                int64_t value_out = transaction_get_value_out(tx);
                wtx.fee = debit > value_out ? (debit - value_out) : 0;
            }
            db_wallet_tx_save(ndb, &wtx);
            free(raw);
        }
    }

    return is_ours;
}

bool node_db_sync_mempool_add(struct node_db *ndb,
                              const struct transaction *tx,
                              int64_t fee, int height)
{
    if (!ndb->open) return false;

    size_t raw_len = 0;
    uint8_t *raw = serialize_tx(tx, &raw_len);
    if (!raw) return false;

    struct db_mempool_entry e;
    memset(&e, 0, sizeof(e));
    memcpy(e.txid, tx->hash.data, 32);
    e.raw_tx = raw;
    e.raw_tx_len = raw_len;
    e.fee = fee;
    e.size = (int)raw_len;
    e.time_added = (int64_t)time(NULL);
    e.height_added = height;
    e.spends_coinbase = false;

    bool ok = db_mempool_save(ndb, &e);

    /* Track outpoint spends for conflict detection */
    for (size_t i = 0; i < tx->num_vin; i++) {
        db_mempool_add_spend(ndb, tx->hash.data,
            tx->vin[i].prevout.hash.data,
            tx->vin[i].prevout.n);
    }

    free(raw);
    return ok;
}

bool node_db_sync_mempool_remove(struct node_db *ndb,
                                 const uint8_t txid[32])
{
    return db_mempool_delete(ndb, txid);
}

bool node_db_sync_sapling_note(struct node_db *ndb,
                               const uint8_t txid[32],
                               uint32_t output_index,
                               int64_t value,
                               const uint8_t rcm[32],
                               const uint8_t memo[512],
                               size_t memo_len,
                               const uint8_t ivk[32],
                               const uint8_t diversifier[11],
                               const uint8_t pk_d[32],
                               const uint8_t cm[32],
                               const uint8_t nullifier[32],
                               int block_height)
{
    struct db_sapling_note n;
    memset(&n, 0, sizeof(n));
    memcpy(n.txid, txid, 32);
    n.output_index = output_index;
    n.value = value;
    memcpy(n.rcm, rcm, 32);
    if (memo && memo_len > 0) {
        size_t ml = memo_len < 512 ? memo_len : 512;
        memcpy(n.memo, memo, ml);
        n.memo_len = ml;
    }
    memcpy(n.ivk, ivk, 32);
    memcpy(n.diversifier, diversifier, 11);
    memcpy(n.pk_d, pk_d, 32);
    memcpy(n.cm, cm, 32);
    memcpy(n.nullifier, nullifier, 32);
    n.block_height = block_height;
    return db_sapling_note_save(ndb, &n);
}

bool node_db_sync_sapling_spend(struct node_db *ndb,
                                const uint8_t nullifier[32],
                                const uint8_t spending_txid[32])
{
    /* Add to global nullifier set */
    sqlite3_stmt *s = ndb->stmt_nullifier_insert;
    sqlite3_reset(s);
    sqlite3_bind_blob(s, 1, nullifier, 32, SQLITE_STATIC);
    sqlite3_step(s);

    /* Mark wallet note as spent */
    return db_sapling_note_mark_spent(ndb, nullifier,
                                      spending_txid);
}

bool node_db_sync_peer(struct node_db *ndb,
                       const uint8_t ip[16], uint16_t port,
                       uint64_t services, int64_t last_seen)
{
    struct db_peer p;
    memset(&p, 0, sizeof(p));
    memcpy(p.ip, ip, 16);
    p.port = port;
    p.services = services;
    p.last_seen = last_seen;
    return db_peer_save(ndb, &p);
}

int node_db_sync_get_tip_height(struct node_db *ndb)
{
    int64_t h = -1;
    node_db_state_get_int(ndb, "tip_height", &h);
    return (int)h;
}

bool node_db_sync_get_tip_hash(struct node_db *ndb, uint8_t hash_out[32])
{
    size_t len = 0;
    if (!node_db_state_get(ndb, "tip_hash", hash_out, 32, &len))
        return false;
    return len == 32;
}

bool node_db_sync_set_tip(struct node_db *ndb,
                          const uint8_t hash[32], int height)
{
    node_db_state_set(ndb, "tip_hash", hash, 32);
    return node_db_state_set_int(ndb, "tip_height",
                                 (int64_t)height);
}

/* Lean index: block header + txid index only.
 * No UTXO tracking, no nullifiers, no solution blob.
 * ~5x fewer SQLite ops than sync_block_inner. */
static bool sync_block_lean(struct node_db *ndb,
                            const struct block *blk,
                            const struct block_index *pindex)
{
    struct db_block db_blk;
    memset(&db_blk, 0, sizeof(db_blk));
    memcpy(db_blk.hash, pindex->phashBlock->data, 32);
    db_blk.height = pindex->nHeight;
    /* Get prev_hash from block header (always available) rather than
     * pprev pointer (may be NULL if block_index has gaps) */
    memcpy(db_blk.prev_hash, blk->header.hashPrevBlock.data, 32);
    db_blk.version = blk->header.nVersion;
    memcpy(db_blk.merkle_root,
           blk->header.hashMerkleRoot.data, 32);
    db_blk.time = blk->header.nTime;
    db_blk.bits = blk->header.nBits;
    memcpy(db_blk.nonce, blk->header.nNonce.data, 32);
    db_blk.solution = (uint8_t *)blk->header.nSolution;
    db_blk.solution_len = blk->header.nSolutionSize;
    memcpy(db_blk.chain_work, pindex->nChainWork.pn, 32);
    db_blk.status = pindex->nStatus;
    db_blk.file_num = pindex->nFile;
    db_blk.data_pos = (int)pindex->nDataPos;
    db_blk.num_tx = (int)blk->num_vtx;

    if (!db_block_save(ndb, &db_blk))
        return false;

    for (size_t i = 0; i < blk->num_vtx; i++) {
        const struct transaction *tx = &blk->vtx[i];

        struct db_tx_index db_tx;
        memset(&db_tx, 0, sizeof(db_tx));
        memcpy(db_tx.txid, tx->hash.data, 32);
        memcpy(db_tx.block_hash, pindex->phashBlock->data, 32);
        db_tx.block_height = pindex->nHeight;
        db_tx.tx_index = (int)i;
        db_tx.file_num = pindex->nFile;
        db_tx.file_pos = (int)pindex->nDataPos;
        db_tx.is_coinbase = (i == 0);
        db_tx_save(ndb, &db_tx);
    }

    return true;
}

/* Drop non-essential indexes for fast bulk loading. */
static void drop_bulk_indexes(struct node_db *ndb)
{
    const char *drops[] = {
        "DROP INDEX IF EXISTS idx_blocks_prev",
        "DROP INDEX IF EXISTS idx_blocks_chainwork",
        "DROP INDEX IF EXISTS idx_tx_block",
        "DROP INDEX IF EXISTS idx_tx_height",
        "DROP INDEX IF EXISTS idx_utxo_address",
        "DROP INDEX IF EXISTS idx_utxo_value",
        "DROP INDEX IF EXISTS idx_utxo_height",
        "DROP INDEX IF EXISTS idx_wtx_height",
        "DROP INDEX IF EXISTS idx_wtx_time",
        "DROP INDEX IF EXISTS idx_wutxo_unspent",
        "DROP INDEX IF EXISTS idx_wutxo_spent",
        "DROP INDEX IF EXISTS idx_snote_unspent",
        "DROP INDEX IF EXISTS idx_snote_nullifier",
        NULL
    };
    for (int i = 0; drops[i]; i++)
        sqlite3_exec(ndb->db, drops[i], NULL, NULL, NULL);
}

/* Rebuild indexes after bulk loading. */
static void rebuild_indexes(struct node_db *ndb)
{
    const char *creates[] = {
        "CREATE INDEX IF NOT EXISTS idx_blocks_prev"
        " ON blocks(prev_hash)",
        "CREATE INDEX IF NOT EXISTS idx_blocks_chainwork"
        " ON blocks(chain_work DESC)",
        "CREATE INDEX IF NOT EXISTS idx_tx_block"
        " ON transactions(block_hash)",
        "CREATE INDEX IF NOT EXISTS idx_tx_height"
        " ON transactions(block_height)",
        "CREATE INDEX IF NOT EXISTS idx_utxo_address"
        " ON utxos(address_hash) WHERE address_hash IS NOT NULL",
        "CREATE INDEX IF NOT EXISTS idx_utxo_value"
        " ON utxos(value DESC)",
        "CREATE INDEX IF NOT EXISTS idx_utxo_height"
        " ON utxos(height)",
        "CREATE INDEX IF NOT EXISTS idx_wtx_height"
        " ON wallet_transactions(block_height)",
        "CREATE INDEX IF NOT EXISTS idx_wtx_time"
        " ON wallet_transactions(time_received DESC)",
        "CREATE INDEX IF NOT EXISTS idx_wutxo_unspent"
        " ON wallet_utxos(address_hash) WHERE spent_txid IS NULL",
        "CREATE INDEX IF NOT EXISTS idx_wutxo_spent"
        " ON wallet_utxos(spent_txid) WHERE spent_txid IS NOT NULL",
        "CREATE INDEX IF NOT EXISTS idx_snote_unspent"
        " ON wallet_sapling_notes(ivk) WHERE spent_txid IS NULL",
        "CREATE INDEX IF NOT EXISTS idx_snote_nullifier"
        " ON wallet_sapling_notes(nullifier)",
        NULL
    };
    printf("SQLite: rebuilding indexes...\n");
    fflush(stdout);
    for (int i = 0; creates[i]; i++)
        sqlite3_exec(ndb->db, creates[i], NULL, NULL, NULL);
    printf("SQLite: indexes rebuilt\n");
    fflush(stdout);
}

/* Helper: mmap a block file, returning mapped data or NULL. */
static uint8_t *mmap_block_file(const char *datadir, int file_num,
                                size_t *out_size)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/blocks/blk%05d.dat",
             datadir, file_num);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat fst;
    if (fstat(fd, &fst) != 0) { close(fd); return NULL; }
    uint8_t *data = mmap(NULL, (size_t)fst.st_size,
                         PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (data == MAP_FAILED) return NULL;
    *out_size = (size_t)fst.st_size;
    posix_madvise(data, *out_size, POSIX_MADV_SEQUENTIAL);
    posix_madvise(data, *out_size, POSIX_MADV_WILLNEED);
    return data;
}

/* Try-decrypt Sapling outputs in a transaction and save to SQLite.
 * Returns number of notes found. */
static int catchup_try_sapling_decrypt(struct node_db *ndb,
                                        const struct transaction *tx,
                                        const struct wallet *w,
                                        int height)
{
    if (tx->num_shielded_output == 0 || w->sapling_keys.num_keys == 0)
        return 0;

    int found = 0;
    struct uint256 txid;
    {
        struct transaction *mtx = (struct transaction *)tx;
        transaction_compute_hash(mtx);
        txid = mtx->hash;
    }

    for (size_t oi = 0; oi < tx->num_shielded_output; oi++) {
        const struct output_description *od = &tx->v_shielded_output[oi];

        for (size_t ki = 0; ki < w->sapling_keys.num_keys; ki++) {
            const struct sapling_key_entry *ke = &w->sapling_keys.keys[ki];
            if (!ke->used) continue;

            uint8_t dhsecret[32];
            if (!sapling_ka_agree(od->ephemeral_key.data, ke->ivk, dhsecret))
                continue;

            uint8_t dec_key[32];
            if (!sapling_kdf(dec_key, dhsecret, od->ephemeral_key.data)) {
                memory_cleanse(dhsecret, 32);
                continue;
            }
            memory_cleanse(dhsecret, 32);

            uint8_t plaintext[564];
            if (!sapling_note_decrypt(dec_key, od->enc_ciphertext, 580,
                                       plaintext)) {
                memory_cleanse(dec_key, 32);
                continue;
            }
            memory_cleanse(dec_key, 32);

            if (plaintext[0] != 0x01) continue;

            uint8_t d[11];
            memcpy(d, plaintext + 1, 11);
            uint64_t value = 0;
            for (int b = 0; b < 8; b++)
                value |= ((uint64_t)plaintext[12 + b]) << (8 * b);
            uint8_t rcm[32];
            memcpy(rcm, plaintext + 20, 32);

            uint8_t pk_d[32];
            if (!sapling_ivk_to_pkd(ke->ivk, d, pk_d)) continue;

            uint8_t cm[32];
            if (!sapling_compute_cm(d, pk_d, value, rcm, cm)) continue;
            if (memcmp(cm, od->cm.data, 32) != 0) continue;

            uint8_t ak[32], nk[32];
            sapling_ask_to_ak(ke->xsk.expsk.ask, ak);
            sapling_nsk_to_nk(ke->xsk.expsk.nsk, nk);

            uint8_t nf[32];
            sapling_compute_nf(d, pk_d, value, rcm, ak, nk, 0, nf);

            node_db_sync_sapling_note(ndb, txid.data, (uint32_t)oi,
                (int64_t)value, rcm, plaintext + 52, 512,
                ke->ivk, d, pk_d, cm, nf, height);
            found++;
            memory_cleanse(plaintext, sizeof(plaintext));
            break;
        }
    }
    return found;
}

int node_db_sync_catchup(struct node_db *ndb,
                         const struct active_chain *chain,
                         const struct wallet *w,
                         const char *datadir)
{
    if (!ndb->open) return -1;

    int db_tip = node_db_sync_get_tip_height(ndb);
    int chain_tip = active_chain_height(chain);

    if (db_tip >= chain_tip)
        return 0;

    int start = db_tip + 1;
    if (start < 0) start = 0;
    int total = chain_tip - start + 1;

    int wallet_keys = 0;
    if (w) {
        for (size_t i = 0; i < w->keystore.num_keys; i++)
            if (w->keystore.keys[i].used) wallet_keys++;
    }
    printf("SQLite catchup: %d blocks (%d..%d), lean index + wallet scan"
           " (%d keys)\n", total, start, chain_tip, wallet_keys);
    fflush(stdout);

    /* Turbo mode: disable fsync, drop indexes, enlarge cache. */
    bool bulk_mode = (total > 50000);
    if (bulk_mode) {
        sqlite3_exec(ndb->db, "PRAGMA synchronous=OFF", NULL, NULL, NULL);
        sqlite3_exec(ndb->db, "PRAGMA cache_size=-524288", NULL, NULL, NULL);
        sqlite3_exec(ndb->db, "PRAGMA wal_autocheckpoint=0",
                     NULL, NULL, NULL);
        sqlite3_busy_timeout(ndb->db, 10000);
        drop_bulk_indexes(ndb);
    }

    /* Verify connection works before starting */
    if (!node_db_begin(ndb)) {
        fprintf(stderr, "catchup: BEGIN failed — aborting\n");
        return -1;
    }
    node_db_commit(ndb);

    int indexed = 0;
    int wallet_hits = 0;
    int batch_size = 100000;
    int64_t t_start = (int64_t)time(NULL);

    /* mmap cache */
    int cached_file = -1;
    uint8_t *cached_data = NULL;
    size_t cached_size = 0;

    /* Initialize Sapling commitment tree for catchup */
    struct incremental_merkle_tree sapling_tree;
    sapling_tree_init(&sapling_tree);
    {
        uint8_t tree_buf[8192];
        size_t tree_len = 0;
        if (node_db_state_get(ndb, "sapling_tree", tree_buf, sizeof(tree_buf), &tree_len)
            && tree_len > 0) {
            struct byte_stream ts;
            stream_init_from_data(&ts, tree_buf, tree_len);
            sapling_tree_init(&sapling_tree);
            incremental_tree_deserialize(&sapling_tree, &ts);
        }
    }

    node_db_begin(ndb);

    for (int h = start; h <= chain_tip; h++) {
        const struct block_index *pindex = active_chain_at(chain, h);
        if (!pindex) continue;
        if (!(pindex->nStatus & BLOCK_HAVE_DATA)) continue;

        /* mmap new file if needed */
        if (pindex->nFile != cached_file) {
            if (cached_data) munmap(cached_data, cached_size);
            cached_data = mmap_block_file(datadir, pindex->nFile,
                                          &cached_size);
            cached_file = cached_data ? pindex->nFile : -1;
            if (!cached_data) continue;
        }

        if (pindex->nDataPos >= cached_size) continue;

        struct block blk;
        block_init(&blk);

        size_t remaining = cached_size - pindex->nDataPos;
        struct byte_stream s;
        stream_init_from_data(&s, cached_data + pindex->nDataPos,
                              remaining);
        if (!block_deserialize(&blk, &s)) {
            block_free(&blk);
            continue;
        }

        /* Lean index: block header + txid index */
        if (!sync_block_lean(ndb, &blk, pindex) && indexed == 0) {
            fprintf(stderr, "catchup: first lean insert failed! "
                    "sqlite error: %s\n",
                    sqlite3_errmsg(ndb->db));
        }

        /* Wallet scan */
        if (w) {
            for (size_t i = 0; i < blk.num_vtx; i++) {
                if (node_db_sync_wallet_tx(ndb, &blk.vtx[i], w, h))
                    wallet_hits++;
                catchup_try_sapling_decrypt(ndb, &blk.vtx[i], w, h);
                for (size_t si = 0; si < blk.vtx[i].num_shielded_spend; si++) {
                    struct transaction *mtx = (struct transaction *)&blk.vtx[i];
                    transaction_compute_hash(mtx);
                    node_db_sync_sapling_spend(ndb,
                        blk.vtx[i].v_shielded_spend[si].nullifier.data,
                        mtx->hash.data);
                }
            }
        }

        /* Advance Sapling tree + wallet witnesses */
        advance_wallet_witnesses(ndb, &blk, &sapling_tree, h);

        block_free(&blk);
        indexed++;

        if (indexed % batch_size == 0) {
            node_db_sync_set_tip(ndb,
                pindex->phashBlock->data, h);
            node_db_commit(ndb);
            int64_t elapsed = (int64_t)time(NULL) - t_start;
            int rate = elapsed > 0 ? indexed / (int)elapsed : 0;
            printf("SQLite: %d/%d blocks (height %d, %d blk/s, %d wallet txs)\n",
                   indexed, total, h, rate, wallet_hits);
            fflush(stdout);
            node_db_begin(ndb);
        }
    }

    if (cached_data) munmap(cached_data, cached_size);

    /* Final commit */
    {
        const struct block_index *tip = active_chain_at(chain, chain_tip);
        if (tip)
            node_db_sync_set_tip(ndb,
                tip->phashBlock->data, chain_tip);
    }
    node_db_commit(ndb);

    /* Restore safe pragmas and rebuild indexes */
    if (bulk_mode) {
        rebuild_indexes(ndb);
        sqlite3_exec(ndb->db, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);
        sqlite3_exec(ndb->db, "PRAGMA cache_size=-65536", NULL, NULL, NULL);
        sqlite3_exec(ndb->db, "PRAGMA wal_autocheckpoint=1000",
                     NULL, NULL, NULL);
        sqlite3_wal_checkpoint_v2(ndb->db, NULL,
            SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL);
    }

    int64_t elapsed = (int64_t)time(NULL) - t_start;
    printf("SQLite catchup complete: %d blocks in %llds (%d blk/s)\n",
           indexed, (long long)elapsed,
           elapsed > 0 ? indexed / (int)elapsed : indexed);
    fflush(stdout);
    return indexed;
}

struct catchup_args {
    struct node_db *ndb;
    const struct active_chain *chain;
    const struct wallet *w;
    const char *datadir;
};

/* sync_wallet_inmemory removed: it passed height=0 for all wallet
 * transactions, causing INSERT OR REPLACE to overwrite correct heights
 * and clear spent_txid on already-spent UTXOs. The catchup block scan
 * already processes wallet transactions with correct heights. */

void *node_db_sync_catchup_thread(void *arg)
{
    struct catchup_args *a = (struct catchup_args *)arg;

    /* Open a dedicated SQLite connection for the catchup thread.
     * Sharing the main ndb connection with the RPC thread causes
     * silent write failures due to WAL locking contention. */
    char path[1024];
    snprintf(path, sizeof(path), "%s/node.db", a->datadir);

    struct node_db catchup_db;
    if (!node_db_open(&catchup_db, path)) {
        fprintf(stderr, "catchup: failed to open dedicated connection\n");
        return NULL;
    }

    node_db_sync_catchup(&catchup_db, a->chain, a->w, a->datadir);

    node_db_close(&catchup_db);
    return NULL;
}

int node_db_sync_wallet_keys(struct node_db *ndb,
                             const struct wallet *w)
{
    if (!ndb->open || !w) return 0;

    /* Skip if counts already match */
    int existing_tkeys = db_wallet_key_count(ndb);
    int existing_zkeys = db_sapling_key_count(ndb);
    int wallet_tkeys = 0;
    for (size_t i = 0; i < w->keystore.num_keys; i++)
        if (w->keystore.keys[i].used) wallet_tkeys++;
    int wallet_zkeys = 0;
    for (size_t i = 0; i < w->sapling_keys.num_keys; i++)
        if (w->sapling_keys.keys[i].used) wallet_zkeys++;

    if (existing_tkeys >= wallet_tkeys &&
        existing_zkeys >= wallet_zkeys)
        return 0;

    int count = 0;
    node_db_begin(ndb);

    /* Sync transparent keys */
    for (size_t i = 0; i < w->keystore.num_keys; i++) {
        const struct key_entry *ke = &w->keystore.keys[i];
        if (!ke->used) continue;
        if (!ke->key.fValid) continue;

        if (db_wallet_key_exists(ndb, ke->keyid.id.data))
            continue;

        struct pubkey pk;
        if (!privkey_get_pubkey(&ke->key, &pk))
            continue;

        struct db_wallet_key dbk;
        memset(&dbk, 0, sizeof(dbk));
        memcpy(dbk.pubkey_hash, ke->keyid.id.data, 20);
        memcpy(dbk.pubkey, pk.vch, pk.size);
        dbk.pubkey_len = pk.size;
        memcpy(dbk.privkey, ke->key.vch, 32);
        dbk.compressed = ke->key.fCompressed;
        dbk.created_at = (int64_t)time(NULL);

        if (db_wallet_key_save(ndb, &dbk))
            count++;
    }

    /* Sync Sapling keys */
    for (size_t i = 0; i < w->sapling_keys.num_keys; i++) {
        const struct sapling_key_entry *sk = &w->sapling_keys.keys[i];
        if (!sk->used) continue;

        if (db_sapling_key_find_by_ivk(ndb, sk->ivk, NULL))
            continue;

        struct db_sapling_key dbsk;
        memset(&dbsk, 0, sizeof(dbsk));
        memcpy(dbsk.ivk, sk->ivk, 32);
        memcpy(dbsk.xsk, &sk->xsk, sizeof(dbsk.xsk));
        memcpy(dbsk.xfvk, &sk->xfvk, sizeof(dbsk.xfvk));
        memcpy(dbsk.diversifier, sk->diversifier, 11);
        memcpy(dbsk.pk_d, sk->pk_d, 32);
        dbsk.child_index = sk->child_index;

        if (db_sapling_key_save(ndb, &dbsk))
            count++;
    }

    node_db_commit(ndb);
    if (count > 0)
        printf("SQLite: synced %d wallet keys\n", count);
    return count;
}

int node_db_sync_mempool_save(struct node_db *ndb,
                              const struct tx_mempool *mempool)
{
    if (!ndb->open || !mempool) return 0;

    int count = 0;
    node_db_begin(ndb);

    for (size_t i = 0; i < mempool->num_entries; i++) {
        const struct mempool_entry *me = &mempool->entries[i];

        size_t raw_len = 0;
        uint8_t *raw = serialize_tx(&me->tx, &raw_len);
        if (!raw) continue;

        struct db_mempool_entry e;
        memset(&e, 0, sizeof(e));
        memcpy(e.txid, me->tx.hash.data, 32);
        e.raw_tx = raw;
        e.raw_tx_len = raw_len;
        e.fee = me->fee;
        e.size = (int)raw_len;
        e.time_added = me->time;
        e.height_added = (int)me->height;
        e.spends_coinbase = me->spends_coinbase;

        if (db_mempool_save(ndb, &e))
            count++;
        free(raw);
    }

    node_db_commit(ndb);
    if (count > 0)
        printf("SQLite: saved %d mempool transactions\n", count);
    return count;
}

struct mempool_load_ctx {
    struct tx_mempool *pool;
    int loaded;
};

static void mempool_load_cb(const struct db_mempool_entry *e, void *ctx)
{
    struct mempool_load_ctx *lc = (struct mempool_load_ctx *)ctx;

    struct transaction tx;
    transaction_init(&tx);

    struct byte_stream s;
    stream_init_from_data(&s, e->raw_tx, e->raw_tx_len);
    if (!transaction_deserialize(&tx, &s)) {
        transaction_free(&tx);
        return;
    }
    transaction_compute_hash(&tx);

    struct mempool_entry me;
    mempool_entry_init(&me, &tx, e->fee, e->time_added,
                       0.0, (unsigned int)e->height_added,
                       true, e->spends_coinbase, 0);

    if (tx_mempool_add_unchecked(lc->pool, &tx.hash, &me))
        lc->loaded++;

    transaction_free(&tx);
}

int node_db_sync_mempool_load(struct node_db *ndb,
                              struct tx_mempool *mempool)
{
    if (!ndb->open || !mempool) return 0;

    struct mempool_load_ctx ctx = { .pool = mempool, .loaded = 0 };
    db_mempool_each(ndb, mempool_load_cb, &ctx);

    if (ctx.loaded > 0)
        printf("SQLite: loaded %d mempool transactions\n", ctx.loaded);

    /* Clear persisted mempool after loading */
    db_mempool_clear(ndb);

    return ctx.loaded;
}

/* ── Parallel UTXO import: LevelDB → SQLite ────────────────────────
 *
 * Architecture: Reader → Ring Buffer → N Decoders → Queue → Writer
 *
 * Reader thread:  Sequential LevelDB iteration, copies raw key+value
 *                 into chunks. Single-threaded (LevelDB not thread-safe).
 * Decoder threads: N parallel workers deserialize coins format, classify
 *                  scripts, extract height. Pure CPU, no shared state.
 * Writer thread:   Single SQLite writer with direct bind/step, no
 *                  ActiveRecord overhead. journal_mode=OFF for max speed.
 *
 * Eliminates: double-write (INSERT+UPDATE), per-txid prepare/finalize,
 *             validation callbacks, 10KB tx_out stack allocations.
 * ─────────────────────────────────────────────────────────────────── */

/* Compact row for the pipeline — 128 bytes vs 10KB for tx_out+db_utxo */
struct utxo_row {
    uint8_t  txid[32];
    uint8_t  address_hash[20];
    uint8_t  script[80];       /* inline for scripts ≤80 bytes (99.9%) */
    uint8_t *script_overflow;  /* heap alloc for rare large scripts */
    int64_t  value;
    int32_t  height;
    uint32_t vout;
    uint16_t script_len;
    uint8_t  script_type;
    uint8_t  has_address;
    uint8_t  is_coinbase;
};

/* A chunk of raw LevelDB entries for decode workers */
#define IMPORT_CHUNK_ENTRIES 2048
#define IMPORT_MAX_ROWS_PER_CHUNK (IMPORT_CHUNK_ENTRIES * 8)

struct raw_entry {
    uint8_t  txid[32];
    uint8_t *value;     /* heap copy of deobfuscated value */
    uint16_t value_len;
};

struct import_chunk {
    struct raw_entry entries[IMPORT_CHUNK_ENTRIES];
    int num_entries;
    struct utxo_row rows[IMPORT_MAX_ROWS_PER_CHUNK];
    int num_rows;
    _Atomic int state; /* 0=free, 1=filled, 2=decoded */
};

#define IMPORT_NUM_CHUNKS 48
#define IMPORT_NUM_DECODERS 8

struct import_context {
    struct import_chunk chunks[IMPORT_NUM_CHUNKS];
    _Atomic int total_txids;
    _Atomic int total_rows;
    _Atomic int decode_failures;
    _Atomic int skipped_outputs;
    _Atomic bool reader_done;
    _Atomic bool decoders_done;
    /* LevelDB reader state (single-threaded) */
    struct coins_view_db *cvdb;
    /* Writer state */
    struct node_db *ndb;
    int write_next; /* next chunk index to write (in-order) */
};

/* Decode a single raw coins entry into utxo_row structs.
 * Returns number of rows produced. Pure function, no shared state. */
static int decode_coins_entry(const struct raw_entry *raw,
                              struct utxo_row *out, int max_rows)
{
    struct byte_stream s;
    stream_init_from_data(&s, raw->value, raw->value_len);

    uint64_t nVersion = 0;
    if (!stream_read_varint(&s, &nVersion)) { stream_free(&s); return 0; }

    uint64_t nCode = 0;
    if (!stream_read_varint(&s, &nCode)) { stream_free(&s); return 0; }

    bool is_coinbase = (nCode & 1) != 0;
    bool vout0_present = (nCode & 2) != 0;
    bool vout1_present = (nCode & 4) != 0;
    unsigned int nMaskCode = (unsigned int)(nCode / 8) +
        ((vout0_present || vout1_present) ? 0 : 1);

    if (nMaskCode > 10000) { stream_free(&s); return 0; }

    /* Build availability vector (max 4096 vouts per tx, largest seen: 468) */
    size_t num_avail = 2;
    bool avail[4096];
    memset(avail, 0, sizeof(avail));
    avail[0] = vout0_present;
    avail[1] = vout1_present;

    unsigned int mask_remaining = nMaskCode;
    while (mask_remaining > 0) {
        unsigned char ch = 0;
        if (!stream_read_bytes(&s, &ch, 1)) break;
        for (unsigned int p = 0; p < 8 && num_avail < 4096; p++)
            avail[num_avail++] = (ch & (1 << p)) != 0;
        if (ch != 0) mask_remaining--;
    }

    /* Deserialize each available output */
    int nrows = 0;
    for (size_t vi = 0; vi < num_avail && nrows < max_rows; vi++) {
        if (!avail[vi]) continue;

        struct tx_out txout;
        tx_out_set_null(&txout);
        if (!compressed_txout_deserialize(&txout, &s))
            break;

        struct utxo_row *r = &out[nrows];
        memcpy(r->txid, raw->txid, 32);
        r->vout = (uint32_t)vi;
        r->value = txout.value;
        r->is_coinbase = is_coinbase;
        r->height = 0; /* set below after reading height varint */
        r->script_overflow = NULL;

        /* Copy script into inline buffer or heap */
        uint16_t slen = txout.script_pub_key.size;
        r->script_len = slen;
        if (slen <= sizeof(r->script)) {
            memcpy(r->script, txout.script_pub_key.data, slen);
        } else {
            r->script_overflow = malloc(slen);
            if (r->script_overflow)
                memcpy(r->script_overflow, txout.script_pub_key.data, slen);
        }

        /* Classify script for address indexing */
        const uint8_t *sc = r->script_overflow ? r->script_overflow : r->script;
        r->has_address = 0;
        if (slen == 25 && sc[0] == 0x76 && sc[1] == 0xa9 &&
            sc[2] == 0x14 && sc[23] == 0x88 && sc[24] == 0xac) {
            memcpy(r->address_hash, sc + 3, 20);
            r->has_address = 1;
            r->script_type = 1; /* P2PKH */
        } else if (slen == 23 && sc[0] == 0xa9 && sc[1] == 0x14 &&
                   sc[22] == 0x87) {
            memcpy(r->address_hash, sc + 2, 20);
            r->has_address = 1;
            r->script_type = 2; /* P2SH */
        } else if (slen > 0 && sc[0] == 0x6a) {
            r->script_type = 3; /* OP_RETURN */
        } else {
            r->script_type = 0; /* OTHER */
        }
        nrows++;
    }

    /* Read height varint (comes after all outputs) and stamp all rows.
     * Sanity-check: height must be ≤ 10M (no chain is taller). */
    uint64_t height = 0;
    if (stream_read_varint(&s, &height) && height <= 10000000) {
        for (int i = 0; i < nrows; i++)
            out[i].height = (int32_t)height;
    }

    stream_free(&s);
    return nrows;
}

/* Decoder worker thread — picks filled chunks, decodes, marks decoded */
static void *import_decoder_thread(void *arg)
{
    struct import_context *ctx = (struct import_context *)arg;

    for (;;) {
        /* Find a filled chunk to decode */
        struct import_chunk *chunk = NULL;
        for (int i = 0; i < IMPORT_NUM_CHUNKS; i++) {
            int expected = 1; /* filled */
            if (atomic_compare_exchange_weak(&ctx->chunks[i].state,
                                              &expected, -1)) {
                chunk = &ctx->chunks[i];
                break;
            }
        }
        if (!chunk) {
            if (atomic_load(&ctx->reader_done)) {
                /* Check once more for any remaining chunks */
                bool found = false;
                for (int i = 0; i < IMPORT_NUM_CHUNKS; i++) {
                    if (atomic_load(&ctx->chunks[i].state) == 1) {
                        found = true;
                        break;
                    }
                }
                if (!found) break;
            }
            /* Yield briefly — spin is fine on 32 cores */
            struct timespec ts = {0, 100000}; /* 100μs */
            nanosleep(&ts, NULL);
            continue;
        }

        /* Decode all entries in this chunk */
        chunk->num_rows = 0;
        for (int i = 0; i < chunk->num_entries; i++) {
            int space = IMPORT_MAX_ROWS_PER_CHUNK - chunk->num_rows;
            if (space <= 0) break;
            int n = decode_coins_entry(&chunk->entries[i],
                                       &chunk->rows[chunk->num_rows],
                                       space);
            chunk->num_rows += n;
        }

        atomic_store(&chunk->state, 2); /* decoded */
    }
    return NULL;
}

/* Writer thread — consumes decoded chunks, inserts into SQLite */
static void *import_writer_thread(void *arg)
{
    struct import_context *ctx = (struct import_context *)arg;
    struct node_db *ndb = ctx->ndb;
    sqlite3_stmt *ins = ndb->stmt_utxo_insert;
    int total_rows = 0;
    int next_chunk = 0;

    sqlite3_exec(ndb->db, "BEGIN", NULL, NULL, NULL);

    for (;;) {
        /* Look for any decoded chunk to write */
        struct import_chunk *chunk = NULL;
        for (int i = 0; i < IMPORT_NUM_CHUNKS; i++) {
            int idx = (next_chunk + i) % IMPORT_NUM_CHUNKS;
            int expected = 2; /* decoded */
            if (atomic_compare_exchange_weak(&ctx->chunks[idx].state,
                                              &expected, -1)) {
                chunk = &ctx->chunks[idx];
                next_chunk = (idx + 1) % IMPORT_NUM_CHUNKS;
                break;
            }
        }
        if (!chunk) {
            if (atomic_load(&ctx->decoders_done)) {
                /* Check once more */
                bool found = false;
                for (int i = 0; i < IMPORT_NUM_CHUNKS; i++) {
                    int s = atomic_load(&ctx->chunks[i].state);
                    if (s == 1 || s == 2) { found = true; break; }
                }
                if (!found) break;
            }
            struct timespec ts = {0, 100000}; /* 100μs */
            nanosleep(&ts, NULL);
            continue;
        }

        /* Insert all rows from this chunk */
        for (int ri = 0; ri < chunk->num_rows; ri++) {
            struct utxo_row *r = &chunk->rows[ri];
            const uint8_t *sc = r->script_overflow ?
                                r->script_overflow : r->script;

            sqlite3_reset(ins);
            sqlite3_bind_blob(ins, 1, r->txid, 32, SQLITE_STATIC);
            sqlite3_bind_int(ins, 2, (int)r->vout);
            sqlite3_bind_int64(ins, 3, r->value);
            sqlite3_bind_blob(ins, 4, sc, (int)r->script_len, SQLITE_STATIC);
            sqlite3_bind_int(ins, 5, r->script_type);
            if (r->has_address)
                sqlite3_bind_blob(ins, 6, r->address_hash, 20, SQLITE_STATIC);
            else
                sqlite3_bind_null(ins, 6);
            sqlite3_bind_int(ins, 7, r->height);
            sqlite3_bind_int(ins, 8, r->is_coinbase);
            sqlite3_step(ins);
            total_rows++;
        }

        /* Commit every ~100K rows */
        if (total_rows % 100000 < chunk->num_rows) {
            sqlite3_exec(ndb->db, "COMMIT", NULL, NULL, NULL);
            printf("UTXO import: %d rows written...\n", total_rows);
            fflush(stdout);
            sqlite3_exec(ndb->db, "BEGIN", NULL, NULL, NULL);
        }

        /* Free buffers and release chunk */
        for (int ei = 0; ei < chunk->num_entries; ei++)
            free(chunk->entries[ei].value);
        for (int ri = 0; ri < chunk->num_rows; ri++)
            free(chunk->rows[ri].script_overflow);
        chunk->num_entries = 0;
        chunk->num_rows = 0;
        atomic_store(&chunk->state, 0); /* free for reuse */
    }

    sqlite3_exec(ndb->db, "COMMIT", NULL, NULL, NULL);
    atomic_store(&ctx->total_rows, total_rows);
    return NULL;
}

int node_db_sync_import_utxos(struct node_db *ndb,
                               struct coins_view_db *cvdb)
{
    if (!ndb->open || !cvdb) return -1;

    printf("UTXO import: parallel pipeline (%d decoders, %d chunks)...\n",
           IMPORT_NUM_DECODERS, IMPORT_NUM_CHUNKS);
    fflush(stdout);

    struct timespec ts_start;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    /* ── SQLite turbo mode ─────────────────────────────────────────── */
    sqlite3_exec(ndb->db, "PRAGMA journal_mode=OFF", NULL, NULL, NULL);
    sqlite3_exec(ndb->db, "PRAGMA synchronous=OFF", NULL, NULL, NULL);
    sqlite3_exec(ndb->db, "PRAGMA locking_mode=EXCLUSIVE", NULL, NULL, NULL);
    sqlite3_exec(ndb->db, "PRAGMA cache_size=-524288", NULL, NULL, NULL);
    sqlite3_exec(ndb->db, "PRAGMA mmap_size=1073741824", NULL, NULL, NULL);
    sqlite3_busy_timeout(ndb->db, 10000);

    /* Drop all UTXO indexes for bulk load */
    sqlite3_exec(ndb->db, "DROP INDEX IF EXISTS idx_utxo_address",
                 NULL, NULL, NULL);
    sqlite3_exec(ndb->db, "DROP INDEX IF EXISTS idx_utxo_value",
                 NULL, NULL, NULL);
    sqlite3_exec(ndb->db, "DROP INDEX IF EXISTS idx_utxo_height",
                 NULL, NULL, NULL);
    sqlite3_exec(ndb->db, "DROP INDEX IF EXISTS idx_utxo_height_value",
                 NULL, NULL, NULL);

    /* Clear existing UTXOs */
    sqlite3_exec(ndb->db, "DELETE FROM utxos", NULL, NULL, NULL);

    /* ── Initialize pipeline context ───────────────────────────────── */
    struct import_context *ctx = calloc(1, sizeof(struct import_context));
    if (!ctx) return -1;
    ctx->cvdb = cvdb;
    ctx->ndb = ndb;
    atomic_store(&ctx->reader_done, false);
    atomic_store(&ctx->decoders_done, false);
    for (int i = 0; i < IMPORT_NUM_CHUNKS; i++)
        atomic_store(&ctx->chunks[i].state, 0);

    /* ── Start decoder + writer threads ────────────────────────────── */
    pthread_t decoders[IMPORT_NUM_DECODERS];
    for (int i = 0; i < IMPORT_NUM_DECODERS; i++)
        pthread_create(&decoders[i], NULL, import_decoder_thread, ctx);

    pthread_t writer;
    pthread_create(&writer, NULL, import_writer_thread, ctx);

    /* ── Reader (main thread): iterate LevelDB, fill chunks ────────── */
    struct db_iterator it;
    db_iter_init(&it, &cvdb->db);
    char seek_key[33];
    seek_key[0] = 'c';
    memset(seek_key + 1, 0, 32);
    db_iter_seek(&it, seek_key, 33);

    int chunk_idx = 0;
    int total_entries = 0;

    while (db_iter_valid(&it)) {
        /* Find a free chunk */
        struct import_chunk *chunk = NULL;
        while (!chunk) {
            for (int i = 0; i < IMPORT_NUM_CHUNKS; i++) {
                int idx = (chunk_idx + i) % IMPORT_NUM_CHUNKS;
                int expected = 0;
                if (atomic_compare_exchange_weak(&ctx->chunks[idx].state,
                                                  &expected, -1)) {
                    chunk = &ctx->chunks[idx];
                    chunk_idx = (idx + 1) % IMPORT_NUM_CHUNKS;
                    break;
                }
            }
            if (!chunk) {
                struct timespec ts = {0, 50000}; /* 50μs */
                nanosleep(&ts, NULL);
            }
        }

        /* Fill chunk with raw entries from LevelDB */
        chunk->num_entries = 0;
        chunk->num_rows = 0;

        while (chunk->num_entries < IMPORT_CHUNK_ENTRIES &&
               db_iter_valid(&it)) {
            size_t key_len;
            const char *key_data = db_iter_key(&it, &key_len);
            if (key_len < 1 || key_data[0] != 'c') goto reader_done;
            if (key_len < 33) { db_iter_next(&it); continue; }

            struct raw_entry *e = &chunk->entries[chunk->num_entries];
            memcpy(e->txid, key_data + 1, 32);

            size_t val_len;
            const char *val_data = db_iter_value(&it, &val_len);
            if (val_len > 65535) val_len = 65535;
            e->value = malloc(val_len);
            if (e->value) {
                memcpy(e->value, val_data, val_len);
                e->value_len = (uint16_t)val_len;
                chunk->num_entries++;
                total_entries++;
            }
            db_iter_next(&it);
        }

        if (chunk->num_entries > 0)
            atomic_store(&chunk->state, 1); /* filled → decoders */
        else
            atomic_store(&chunk->state, 0); /* empty, release */
    }
reader_done:
    db_iter_free(&it);
    atomic_store(&ctx->reader_done, true);

    printf("UTXO import: read %d txids from LevelDB\n", total_entries);
    fflush(stdout);

    /* ── Wait for decoders, then signal writer ─────────────────────── */
    for (int i = 0; i < IMPORT_NUM_DECODERS; i++)
        pthread_join(decoders[i], NULL);
    atomic_store(&ctx->decoders_done, true);

    /* ── Wait for writer ───────────────────────────────────────────── */
    pthread_join(writer, NULL);
    int total_rows = atomic_load(&ctx->total_rows);

    struct timespec ts_write;
    clock_gettime(CLOCK_MONOTONIC, &ts_write);
    double pipe_ms = (ts_write.tv_sec - ts_start.tv_sec) * 1000.0 +
                     (ts_write.tv_nsec - ts_start.tv_nsec) / 1e6;
    printf("UTXO import: %d rows written in %.0fms\n", total_rows, pipe_ms);
    fflush(stdout);

    /* ── Rebuild all indexes for power-node queries ────────────────── */
    printf("UTXO import: building indexes for fast queries...\n");
    fflush(stdout);

    struct timespec ts_idx;
    clock_gettime(CLOCK_MONOTONIC, &ts_idx);

    /* Address lookup — balance queries, listunspent */
    sqlite3_exec(ndb->db,
        "CREATE INDEX IF NOT EXISTS idx_utxo_address"
        " ON utxos(address_hash) WHERE address_hash IS NOT NULL",
        NULL, NULL, NULL);

    /* Top-value queries */
    sqlite3_exec(ndb->db,
        "CREATE INDEX IF NOT EXISTS idx_utxo_value"
        " ON utxos(value DESC)",
        NULL, NULL, NULL);

    /* Age queries */
    sqlite3_exec(ndb->db,
        "CREATE INDEX IF NOT EXISTS idx_utxo_height"
        " ON utxos(height)",
        NULL, NULL, NULL);

    /* HODL wave covering index — height+value for age distribution */
    sqlite3_exec(ndb->db,
        "CREATE INDEX IF NOT EXISTS idx_utxo_height_value"
        " ON utxos(height, value)",
        NULL, NULL, NULL);

    struct timespec ts_idx_done;
    clock_gettime(CLOCK_MONOTONIC, &ts_idx_done);
    double idx_ms = (ts_idx_done.tv_sec - ts_idx.tv_sec) * 1000.0 +
                    (ts_idx_done.tv_nsec - ts_idx.tv_nsec) / 1e6;
    printf("UTXO import: indexes built in %.0fms\n", idx_ms);

    /* ── Restore safe pragmas ──────────────────────────────────────── */
    sqlite3_exec(ndb->db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(ndb->db, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);
    sqlite3_exec(ndb->db, "PRAGMA locking_mode=NORMAL", NULL, NULL, NULL);
    sqlite3_exec(ndb->db, "PRAGMA cache_size=-65536", NULL, NULL, NULL);
    sqlite3_exec(ndb->db, "PRAGMA wal_autocheckpoint=1000",
                 NULL, NULL, NULL);
    sqlite3_wal_checkpoint_v2(ndb->db, NULL,
        SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL);

    double total_ms = (ts_idx_done.tv_sec - ts_start.tv_sec) * 1000.0 +
                      (ts_idx_done.tv_nsec - ts_start.tv_nsec) / 1e6;
    printf("UTXO import complete: %d outputs from %d txids in %.1fs "
           "(pipeline %.0fms + index %.0fms)\n",
           total_rows, total_entries, total_ms / 1000.0,
           pipe_ms, idx_ms);
    fflush(stdout);

    free(ctx);
    return total_rows;
}
