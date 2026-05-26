/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* sync_controller_catchup: long-running maintenance jobs.
 *
 *   - sapling_tree_rebuild: replay all shielded outputs from block
 *     files to rebuild the Sapling commitment tree.
 *   - node_db_sync_catchup: bulk-index blocks (sqlite_tip+1 → chain_tip)
 *     into SQLite, optionally scanning for wallet transactions.
 *   - catchup + import job machinery (thread spawn / join / status).
 *   - wallet_keys copy (idempotent).
 *   - mempool save/load (on shutdown / startup).
 *
 * Split out of sync_controller.c. See sync_controller_internal.h for
 * cross-file glue. */

#include "platform/time_compat.h"
#include "controllers/sync_controller.h"
#include "sync_controller_internal.h"
#include "util/boot_progress.h"
#include "services/recovery_policy.h"
#include "models/db_txn.h"
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
#include "core/utiltime.h"
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
#include "config/runtime.h"
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
#include <signal.h>
#include "util/ar_step_readonly.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "util/thread_registry.h"

extern volatile sig_atomic_t g_shutdown_requested;

struct wallet_keys_sync_ctx {
    const struct wallet *wallet;
    int count;
};

struct mempool_save_ctx {
    const struct tx_mempool *mempool;
    int count;
};

/* Lean index: block header + txid index only.
 * No UTXO tracking, no nullifiers, no solution blob.
 * ~5x fewer SQLite ops than sync_block_inner. */
static bool sync_block_lean(struct node_db *ndb,
                            const struct block *blk,
                            const struct block_index *pindex)
{
    if (!ndb || !ndb->open || !blk || !pindex)
        LOG_FAIL("sync", "sync_block_lean: invalid args (ndb=%p, blk=%p, pindex=%p)",
                 (void *)ndb, (void *)blk, (void *)pindex);

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
    if ((db_blk.status & BLOCK_VALID_MASK) < BLOCK_VALID_TRANSACTIONS)
        db_blk.status = (db_blk.status & ~BLOCK_VALID_MASK) |
                        BLOCK_VALID_TRANSACTIONS;
    db_blk.status |= BLOCK_HAVE_DATA;
    db_blk.file_num = pindex->nFile;
    db_blk.data_pos = (int)pindex->nDataPos;
    db_blk.num_tx = (int)blk->num_vtx;

    if (!db_block_save(ndb, &db_blk))
        LOG_FAIL("sync", "sync_block_lean: db_block_save failed at height %d",
                 pindex->nHeight);

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
        if (!db_tx_save(ndb, &db_tx))
            LOG_FAIL("sync", "sync_block_lean: db_tx_save failed at height %d tx %zu",
                     pindex->nHeight, i);
    }

    return true;
}

/* Index drop/rebuild delegated to node_db_ibd_turbo_mode/normal_mode. */

/* Helper: mmap a block file, returning mapped data or NULL. */
static uint8_t *mmap_block_file(const char *datadir, int file_num,
                                size_t *out_size)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/blocks/blk%05d.dat",
             datadir, file_num);
    int fd = open(path, O_RDONLY);
    if (fd < 0) LOG_NULL("sync", "mmap_block_file: open failed for %s", path);
    struct stat fst;
    if (fstat(fd, &fst) != 0) { close(fd); LOG_NULL("sync", "mmap_block_file: fstat failed for %s", path); }
    uint8_t *data = mmap(NULL, (size_t)fst.st_size,
                         PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (data == MAP_FAILED) LOG_NULL("sync", "mmap_block_file: mmap failed for file %d", file_num);
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
                                        int height,
                                        bool *ok_out)
{
    if (!ndb || !tx || !w || tx->num_shielded_output == 0 ||
        w->sapling_keys.num_keys == 0) {
        if (ok_out)
            *ok_out = true;
        return 0;
    }

    int found = 0;
    bool ok = true;
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
            if (!ke->used)
                continue;

            uint8_t dhsecret[32];
            if (!sapling_ka_agree(od->ephemeral_key.data, ke->ivk, dhsecret))
                continue;

            uint8_t dec_key[32];
            if (!sapling_kdf(dec_key, dhsecret, od->ephemeral_key.data)) {
                memory_cleanse(dhsecret, sizeof(dhsecret));
                continue;
            }
            memory_cleanse(dhsecret, sizeof(dhsecret));

            uint8_t plaintext[564];
            if (!sapling_note_decrypt(dec_key, od->enc_ciphertext, 580,
                                      plaintext)) {
                memory_cleanse(dec_key, sizeof(dec_key));
                continue;
            }
            memory_cleanse(dec_key, sizeof(dec_key));

            if (plaintext[0] != 0x01)
                continue;

            uint8_t d[11];
            memcpy(d, plaintext + 1, sizeof(d));
            uint64_t value = 0;
            for (int b = 0; b < 8; b++)
                value |= ((uint64_t)plaintext[12 + b]) << (8 * b);
            uint8_t rcm[32];
            memcpy(rcm, plaintext + 20, sizeof(rcm));

            uint8_t pk_d[32];
            if (!sapling_ivk_to_pkd(ke->ivk, d, pk_d))
                continue;

            uint8_t cm[32];
            if (!sapling_compute_cm(d, pk_d, value, rcm, cm))
                continue;
            if (memcmp(cm, od->cm.data, sizeof(cm)) != 0)
                continue;

            uint8_t ak[32], nk[32];
            sapling_ask_to_ak(ke->xsk.expsk.ask, ak);
            sapling_nsk_to_nk(ke->xsk.expsk.nsk, nk);

            uint8_t nf[32];
            sapling_compute_nf(d, pk_d, value, rcm, ak, nk, 0, nf);

            if (!node_db_sync_sapling_note(ndb, txid.data, (uint32_t)oi,
                                          (int64_t)value, rcm,
                                          plaintext + 52, 512,
                                          ke->ivk, d, pk_d, cm, nf,
                                          height)) {
                ok = false;
            } else {
                found++;
            }

            memory_cleanse(plaintext, sizeof(plaintext));
            if (!ok)
                break;

            break;
        }

        if (!ok)
            break;
    }
    if (ok_out)
        *ok_out = ok;
    return found;
}

/* ── Sapling tree rebuild — replay all shielded outputs from block files ── */

int sapling_tree_rebuild(struct node_db *ndb,
                         const struct active_chain *chain,
                         const char *datadir)
{
    if (!ndb || !chain || !datadir)
        LOG_ERR("sync", "sapling_tree_rebuild: invalid args (ndb=%p, chain=%p, datadir=%p)",
                (void *)ndb, (void *)chain, (void *)datadir);

    int chain_tip = active_chain_height(chain);
    int sapling_height = 476969; /* ZClassic Sapling activation */
    if (chain_tip < sapling_height) return 0;

    struct incremental_merkle_tree tree;
    sapling_tree_init(&tree);
    int total_commitments = 0;
    int mismatches = 0;
    int start_height = sapling_height;

    /* Try to resume from a persisted checkpoint to avoid replaying
     * 2.6M blocks on every crash recovery. Two candidates, most
     * authoritative first:
     *   (1) Flat-file checkpoint at <datadir>/sapling_tree_ckpt.dat
     * flushed every 10K blocks, SHA3-verified.
     *   (2) node_state["sapling_tree"] — flushed every 100K blocks,
     *       SQLite-backed, legacy path.
     * The flat-file path short-circuits the node_state path on a hit;
     * a miss falls through to the original node_state probe. */
    int64_t ckpt_h = 0;
    {
        char ckpt_path[512];
        int n = snprintf(ckpt_path, sizeof(ckpt_path),
                         "%s/sapling_tree_ckpt.dat", datadir);
        if (n > 0 && (size_t)n < sizeof(ckpt_path)) {
            int64_t flat_h = 0;
            struct incremental_merkle_tree ff_tree;
            sapling_tree_init(&ff_tree);
            if (sapling_tree_load_checkpoint(&ff_tree, &flat_h, ckpt_path)
                && flat_h > sapling_height && flat_h <= chain_tip) {
                /* Verify against the block-index root at that height
                 * when we have it — an empty root slot just means the
                 * block_index.bin load hasn't populated it yet, which
                 * is fine for crash-recovery paths (the SHA3 trailer
                 * already proved the file wasn't tampered with). */
                const struct block_index *ckpt_bi =
                    active_chain_at(chain, (int)flat_h);
                static const uint8_t zeros32[32] = {0};
                bool root_known = ckpt_bi &&
                    memcmp(ckpt_bi->hashFinalSaplingRoot.data,
                           zeros32, 32) != 0;
                bool root_match = true;
                if (root_known) {
                    struct uint256 ffr;
                    incremental_tree_root(&ff_tree, &ffr);
                    root_match = memcmp(ffr.data,
                        ckpt_bi->hashFinalSaplingRoot.data, 32) == 0;
                }
                if (root_match) {
                    tree = ff_tree;
                    start_height = (int)flat_h + 1;
                    total_commitments =
                        (int)incremental_tree_size(&tree);
                    ckpt_h = flat_h; /* skip the SQLite fallback below */
                    LOG_INFO("sapling_tree_rebuild", "sapling_tree_rebuild: resuming " "from flat-file checkpoint h=%lld " "(%d commitments,)", (long long)flat_h, total_commitments);
                    fflush(stderr);
                }
            }
        }
    }

    if (ckpt_h == 0
        && node_db_state_get_int(ndb, "sapling_tree_rebuild_height", &ckpt_h)
        && ckpt_h > sapling_height && ckpt_h <= chain_tip) {
        uint8_t tbuf[8192];
        size_t tlen = 0;
        if (node_db_state_get(ndb, "sapling_tree", tbuf, sizeof(tbuf), &tlen)
            && tlen > 0) {
            struct byte_stream ts;
            stream_init_from_data(&ts, tbuf, tlen);
            if (incremental_tree_deserialize(&tree, &ts)) {
                /* Verify checkpoint root against the block at that height */
                const struct block_index *ckpt_bi =
                    active_chain_at(chain, (int)ckpt_h);
                if (ckpt_bi &&
                    memcmp(ckpt_bi->hashFinalSaplingRoot.data,
                           (uint8_t[32]){0}, 32) != 0) {
                    struct uint256 ckpt_root;
                    incremental_tree_root(&tree, &ckpt_root);
                    if (memcmp(ckpt_root.data,
                               ckpt_bi->hashFinalSaplingRoot.data,
                               32) == 0) {
                        start_height = (int)ckpt_h + 1;
                        total_commitments =
                            (int)incremental_tree_size(&tree);
                        LOG_INFO("sapling_tree_rebuild", "sapling_tree_rebuild: resuming " "from checkpoint h=%d (%d commitments)", (int)ckpt_h, total_commitments);
                        fflush(stderr);
                    } else {
                        /* Checkpoint root doesn't match — start fresh */
                        sapling_tree_init(&tree);
                    }
                } else {
                    /* Can't verify against a local root; accept the
                     * checkpoint only on a 100K boundary (our interval). */
                    if ((ckpt_h - sapling_height) % 100000 == 0) {
                        start_height = (int)ckpt_h + 1;
                        total_commitments =
                            (int)incremental_tree_size(&tree);
                        LOG_INFO("sapling_tree_rebuild", "sapling_tree_rebuild: resuming " "from unverified checkpoint h=%d " "(%d commitments)", (int)ckpt_h, total_commitments);
                        fflush(stderr);
                    } else {
                        sapling_tree_init(&tree);
                    }
                }
            }
        }
    }

    LOG_INFO("sapling_tree_rebuild", "sapling_tree_rebuild: replaying h=%d..%d", start_height, chain_tip);
    fflush(stderr);

    int64_t t_replay_start = GetTimeMillis();

    /* mmap cache — local, thread-safe */
    int cached_file = -1;
    uint8_t *cached_data = NULL;
    size_t cached_size = 0;

    for (int h = start_height; h <= chain_tip; h++) {
        /* Wave 11B — pump systemd watchdog liveness during long
         * sapling-tree replay loops. Without this the unit's
         * WatchdogSec timer expires and the bulk replay is killed. */
        if ((h % 100) == 0)
            boot_progress_tick("sapling_tree_rebuild");
        const struct block_index *bi = active_chain_at(chain, h);
        if (!bi) continue;
        if (!(bi->nStatus & BLOCK_HAVE_DATA)) continue;

        /* mmap new file if needed */
        if (bi->nFile != cached_file) {
            if (cached_data) munmap(cached_data, cached_size);
            cached_data = mmap_block_file(datadir, bi->nFile, &cached_size);
            cached_file = cached_data ? bi->nFile : -1;
            if (!cached_data) continue;
        }

        if (bi->nDataPos >= cached_size) continue;

        /* Parse block from mmap'd data */
        struct block blk;
        block_init(&blk);
        size_t remaining = cached_size - bi->nDataPos;
        struct byte_stream s;
        stream_init_from_data(&s, cached_data + bi->nDataPos, remaining);
        if (!block_deserialize(&blk, &s)) {
            block_free(&blk);
            continue;
        }

        /* Append all Sapling output commitments */
        for (size_t i = 0; i < blk.num_vtx; i++) {
            const struct transaction *tx = &blk.vtx[i];
            for (size_t j = 0; j < tx->num_shielded_output; j++) {
                incremental_tree_append(&tree,
                    &tx->v_shielded_output[j].cm);
                total_commitments++;
            }
        }

        /* Verify root at checkpoints (every 100K blocks). Use the
         * block header's root (block_index may not have it populated). */
        bool is_checkpoint = ((h - sapling_height) % 100000 == 0 &&
                              h > sapling_height);
        if (is_checkpoint) {
            struct uint256 computed;
            incremental_tree_root(&tree, &computed);
            if (memcmp(computed.data,
                       blk.header.hashFinalSaplingRoot.data, 32) != 0)
                mismatches++;
        }

        block_free(&blk);

        if (is_checkpoint) {

            LOG_WARN("sapling_tree_rebuild", "  sapling_tree_rebuild: h=%d/%d " "commitments=%d mismatches=%d", h, chain_tip, total_commitments, mismatches);
            fflush(stderr);

            /* Persist tree checkpoint to survive crashes */
            struct byte_stream ts;
            stream_init(&ts, 4096);
            incremental_tree_serialize(&tree, &ts);
            node_db_state_set(ndb, "sapling_tree", ts.data, ts.size);
            node_db_state_set_int(ndb, "sapling_tree_rebuild_height",
                                  (int64_t)h);
            stream_free(&ts);
        }
    }

    if (cached_data) munmap(cached_data, cached_size);

    /* Persist final tree */
    {
        struct byte_stream ts;
        stream_init(&ts, 4096);
        incremental_tree_serialize(&tree, &ts);
        node_db_state_set(ndb, "sapling_tree", ts.data, ts.size);
        node_db_state_set_int(ndb, "sapling_tree_rebuild_height",
                              (int64_t)chain_tip);
        stream_free(&ts);
    }

    /* Verify against chain tip */
    const struct block_index *tip = active_chain_tip(chain);
    struct uint256 final_root;
    incremental_tree_root(&tree, &final_root);
    bool match = tip && memcmp(final_root.data,
                               tip->hashFinalSaplingRoot.data, 32) == 0;

    char root_hex[65];
    uint256_get_hex(&final_root, root_hex);
    int64_t replay_ms = GetTimeMillis() - t_replay_start;
    int replayed_blocks = (chain_tip >= start_height)
                          ? (chain_tip - start_height + 1)
                          : 0;
    LOG_INFO("sapling_tree_rebuild", "sapling_tree_rebuild: replayed %d blocks in %lld ms", replayed_blocks, (long long)replay_ms);
    LOG_WARN("sapling_tree_rebuild", "sapling_tree_rebuild: DONE commitments=%d " "mismatches=%d root=%s match=%s", total_commitments, mismatches, root_hex, match ? "YES" : "NO");
    fflush(stderr);

    /* Copy to caller via ndb — the boot code will read it back */
    (void)tree; /* tree is stack-local; persisted via node_state */

    return total_commitments;
}

int node_db_sync_catchup(struct node_db *ndb,
                         const struct active_chain *chain,
                         const struct wallet *w,
                         const char *datadir)
{
    bool interrupted = false;
    bool tx_open = false;
    bool failed = false;
    int last_indexed_height = 0;
    int last_committed_height = -1;
    const struct block_index *last_indexed_tip = NULL;
    struct sync_db_turbo_scope turbo_mode = {0};
    bool restore_ok = true;

    if (!ndb || !ndb->open || !chain)
        LOG_ERR("sync", "catchup: invalid args (ndb=%p, chain=%p)", (void *)ndb, (void *)chain);

    int db_tip = node_db_sync_get_tip_height(ndb);
    int chain_tip = active_chain_height(chain);
    if (db_tip >= chain_tip) return 0;
    if (!datadir)
        LOG_ERR("sync", "catchup: datadir is NULL (db_tip=%d, chain_tip=%d)",
                db_tip, chain_tip);

    /* Keep pre-existing fast path behavior when there is no catchup work. */
    last_indexed_height = db_tip;
    last_committed_height = db_tip;

    int start = db_tip + 1;
    if (start < 0) start = 0;
    int total = chain_tip - start + 1;
    sync_job_catchup_begin(start, chain_tip);

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
    if (!sync_db_turbo_scope_begin(&turbo_mode, ndb, bulk_mode)) {
        fprintf(stderr, "catchup: failed to enter turbo mode\n");
        sync_job_catchup_finish();
        return -1; // raw-return-ok:logged-above
    }

    /* Verify connection works before starting */
    if (!node_db_begin(ndb)) {
        LOG_WARN("catchup", "catchup: BEGIN failed — aborting");
        if (!sync_db_turbo_scope_end(&turbo_mode))
            fprintf(stderr, "catchup: failed to restore normal mode after BEGIN failure\n");
        restore_ok = false;
        sync_job_catchup_finish();
        return -1; // raw-return-ok:logged-above
    }
    tx_open = true;
    if (!node_db_commit(ndb)) {
        LOG_WARN("catchup", "catchup: initial COMMIT failed — aborting");
        node_db_rollback(ndb);
        if (!sync_db_turbo_scope_end(&turbo_mode))
            fprintf(stderr, "catchup: failed to restore normal mode after initial COMMIT failure\n");
        restore_ok = false;
        sync_job_catchup_finish();
        return -1; // raw-return-ok:logged-above
    }
    tx_open = false;

    int indexed = 0;
    int wallet_hits = 0;
    int batch_size = 100000;
    int64_t t_start = (int64_t)platform_time_wall_time_t();

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

    if (!node_db_begin(ndb)) {
        LOG_WARN("catchup", "catchup: failed to open main transaction");
        if (!sync_db_turbo_scope_end(&turbo_mode))
            fprintf(stderr, "catchup: failed to restore normal mode after tx open failure\n");
        restore_ok = false;
        sync_job_catchup_finish();
        return -1; // raw-return-ok:logged-above
    }
    tx_open = true;

    for (int h = start; h <= chain_tip; h++) {
        if (g_shutdown_requested) {
            interrupted = true;
            break;
        }
        /* Wave 11B — pump systemd watchdog liveness during long
         * node_db sync-catchup loops (block indexing replay). */
        if ((h % 100) == 0)
            boot_progress_tick("node_db_sync_catchup");
        const struct block_index *pindex = active_chain_at(chain, h);
        if (!pindex) continue;
        if (!(pindex->nStatus & BLOCK_HAVE_DATA)) continue;

        /* mmap new file if needed */
        if (pindex->nFile != cached_file) {
            if (cached_data) munmap(cached_data, cached_size);
            cached_data = mmap_block_file(datadir, pindex->nFile,
                                          &cached_size);
            cached_file = cached_data ? pindex->nFile : -1;
            if (!cached_data) {
                LOG_WARN("catchup", "catchup: failed to mmap file blk%05d.dat", pindex->nFile);
                failed = true;
                break;
            }
        }

        if (pindex->nDataPos >= cached_size) {
            LOG_INFO("catchup", "catchup: malformed block data offset at height %d", h);
            failed = true;
            break;
        }

        struct block blk;
        block_init(&blk);

        size_t remaining = cached_size - pindex->nDataPos;
        struct byte_stream s;
        stream_init_from_data(&s, cached_data + pindex->nDataPos,
                              remaining);
        if (!block_deserialize(&blk, &s)) {
            block_free(&blk);
            failed = true;
            break;
        }

        /* Lean index: block header + txid index */
        if (!sync_block_lean(ndb, &blk, pindex)) {
            LOG_WARN("catchup", "catchup: lean index failed at height %d (sqlite=%s)", h, sqlite3_errmsg(ndb->db));
            block_free(&blk);
            failed = true;
            break;
        }

        /* Wallet scan */
        if (w) {
            for (size_t i = 0; i < blk.num_vtx; i++) {
                bool tx_is_ours = false;
                bool tx_ok = false;
                if (!node_db_sync_wallet_tx_checked(ndb, &blk.vtx[i], w, h,
                                                   &tx_is_ours, &tx_ok) || !tx_ok) {
                    LOG_WARN("catchup", "catchup: wallet tx sync failed at height %d " "(tx=%d)", h, (int)i);
                    block_free(&blk);
                    failed = true;
                    break;
                }
                if (tx_is_ours)
                    wallet_hits++;
                bool decrypt_ok = true;
                catchup_try_sapling_decrypt(ndb, &blk.vtx[i], w, h,
                                           &decrypt_ok);
                if (!decrypt_ok) {
                    LOG_WARN("catchup", "catchup: sapling decrypt failed at height %d " "(tx=%d)", h, (int)i);
                    block_free(&blk);
                    failed = true;
                    break;
                }
                for (size_t si = 0; si < blk.vtx[i].num_shielded_spend; si++) {
                    struct transaction *mtx = (struct transaction *)&blk.vtx[i];
                    transaction_compute_hash(mtx);
                    if (!node_db_sync_sapling_spend(
                        ndb,
                        blk.vtx[i].v_shielded_spend[si].nullifier.data,
                        mtx->hash.data)) {
                        LOG_WARN("catchup", "catchup: sapling spend update failed at height %d " "(tx=%d, spend=%zu)", h, (int)i, si);
                        block_free(&blk);
                        failed = true;
                        break;
                    }
                }
                if (failed) break;
            }
        }
        if (failed) break;

        /* Advance Sapling tree + wallet witnesses */
        if (!advance_wallet_witnesses(ndb, &blk, &sapling_tree, h)) {
            LOG_WARN("catchup", "catchup: witness/tree advance failed at height %d", h);
            block_free(&blk);
            failed = true;
            break;
        }

        block_free(&blk);
        indexed++;
        last_indexed_height = h;
        last_indexed_tip = pindex;
        sync_job_catchup_progress(h);

        if (indexed % batch_size == 0) {
            if (pindex->phashBlock) {
                if (!node_db_sync_set_tip(ndb, pindex->phashBlock->data, h)) {
                    LOG_WARN("catchup", "catchup: failed to set tip at batch commit %d", h);
                    failed = true;
                    break;
                }
            } else {
                LOG_WARN("catchup", "catchup: missing hash at batch commit %d", h);
                failed = true;
                break;
            }
            if (!node_db_commit(ndb)) {
                LOG_WARN("catchup", "catchup: batch COMMIT failed at height %d", h);
                node_db_rollback(ndb);
                tx_open = false;
                failed = true;
                break;
            }
            tx_open = false;
            last_committed_height = h;
            int64_t elapsed = (int64_t)platform_time_wall_time_t() - t_start;
            int rate = elapsed > 0 ? indexed / (int)elapsed : 0;
            printf("SQLite: %d/%d blocks (height %d, %d blk/s, %d wallet txs)\n",
                   indexed, total, h, rate, wallet_hits);
            fflush(stdout);
            if (!node_db_begin(ndb)) {
                LOG_WARN("catchup", "catchup: failed to reopen transaction after batch commit");
                failed = true;
                break;
            }
            tx_open = true;
        }
    }

    if (cached_data) munmap(cached_data, cached_size);

    if (failed) {
        if (tx_open && !node_db_rollback(ndb))
            LOG_WARN("catchup", "catchup: rollback failed after failure");
        tx_open = false;
    }

    /* Final commit */
    if (tx_open && !failed) {
        if (last_indexed_tip && last_indexed_tip->phashBlock) {
            if (!node_db_sync_set_tip(ndb,
                                      last_indexed_tip->phashBlock->data,
                                      last_indexed_height)) {
                LOG_WARN("catchup", "catchup: failed to set tip before final commit");
                failed = true;
            }
        } else {
            LOG_WARN("catchup", "catchup: final commit missing tip hash");
            failed = true;
        }
        if (!failed) {
            if (!node_db_commit(ndb)) {
                LOG_WARN("catchup", "catchup: final COMMIT failed");
                if (!node_db_rollback(ndb))
                    LOG_WARN("catchup", "catchup: final ROLLBACK failed");
                tx_open = false;
                failed = true;
                last_indexed_height = last_committed_height;
            } else {
                tx_open = false;
                last_committed_height = last_indexed_height;
            }
        }
    }

    if (failed) {
        if (tx_open && !node_db_rollback(ndb))
            LOG_WARN("catchup", "catchup: final rollback path failed");
        interrupted = false;
    }

    /* Restore safe pragmas and rebuild indexes */
    if (!sync_db_turbo_scope_end(&turbo_mode)) {
        LOG_WARN("catchup", "catchup: failed to restore normal mode");
        restore_ok = false;
    }

    if (failed || !restore_ok) {
        sync_job_catchup_finish();
        LOG_ERR("sync", "catchup: aborting (failed=%d, restore_ok=%d, indexed=%d)",
                failed, restore_ok, indexed);
    }

    int64_t elapsed = (int64_t)platform_time_wall_time_t() - t_start;
    printf("SQLite catchup %s: %d blocks in %llds (%d blk/s, tip=%d)\n",
           interrupted ? "stopped" : "complete",
           indexed, (long long)elapsed,
           elapsed > 0 ? indexed / (int)elapsed : indexed,
           last_committed_height);
    fflush(stdout);

    /* Checkpoint WAL after bulk catchup to reclaim disk space */
    if (indexed > 10000)
        node_db_wal_checkpoint(ndb);

    sync_job_catchup_finish();
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

static void *node_db_sync_catchup_job_thread(void *arg)
{
    struct node_db_sync_catchup_job *job = arg;
    struct node_db catchup_db;
    struct node_db *work_db = NULL;
    bool private_open = false;
    bool owns_db = false;

    if (!job) {
        LOG_NULL("sync", "catchup_job_thread: job is NULL");
    }

    memset(&catchup_db, 0, sizeof(catchup_db));
    private_open = node_db_sync_open_private_db_like(job->args.ndb, &catchup_db);
    if (private_open) {
        work_db = &catchup_db;
        owns_db = true;
    } else if (job->args.datadir) {
        char path[1024];
        if (snprintf(path, sizeof(path), "%s/node.db",
                     job->args.datadir) >= (int)sizeof(path)) {
            LOG_INFO("catchup", "catchup: datadir path too long: %s", job->args.datadir);
        } else {
            private_open = node_db_open(&catchup_db, path);
            if (private_open) {
                work_db = &catchup_db;
                owns_db = true;
            }
        }
    }

    if (!work_db && job->args.ndb && job->args.ndb->open) {
        work_db = job->args.ndb;
    }

    if (!work_db || !work_db->open) {
        fprintf(stderr, "catchup: no usable database handle\n");
        job->result = -1;
        atomic_store(&job->finished, true);
        return NULL;
    }

    job->result = node_db_sync_catchup(work_db, job->args.chain,
                                       job->args.w, job->args.datadir);

    if (owns_db)
        node_db_close(&catchup_db);
    atomic_store(&job->finished, true);
    return NULL;
}

void *node_db_sync_catchup_thread(void *arg)
{
    struct node_db_sync_catchup_job job;
    struct catchup_args *args = arg;

    node_db_sync_catchup_job_init(&job);
    if (!args)
        LOG_NULL("sync", "catchup_thread: args is NULL");
    job.args.ndb = args->ndb;
    job.args.chain = args->chain;
    job.args.w = args->w;
    job.args.datadir = args->datadir;
    if (!node_db_sync_catchup_job_start(&job, job.args.ndb, job.args.chain,
                                        job.args.w, job.args.datadir))
        LOG_NULL("sync", "catchup_thread: job_start failed");
    node_db_sync_catchup_job_join(&job, NULL);
    return NULL;
}

void node_db_sync_catchup_job_init(struct node_db_sync_catchup_job *job)
{
    if (!job)
        return;
    memset(job, 0, sizeof(*job));
    job->result = -1;
    atomic_store(&job->finished, false);
}

bool node_db_sync_catchup_job_start(struct node_db_sync_catchup_job *job,
                                    struct node_db *ndb,
                                    const struct active_chain *chain,
                                    const struct wallet *w,
                                    const char *datadir)
{
    if (!job || job->started || !ndb || !chain)
        LOG_FAIL("sync", "catchup_job_start: invalid args (job=%p, ndb=%p, chain=%p)",
                 (void *)job, (void *)ndb, (void *)chain);

    job->args.ndb = ndb;
    job->args.chain = chain;
    job->args.w = w;
    job->args.datadir = datadir;
    job->result = -1;
    atomic_store(&job->finished, false);
    job->started = true;
    if (thread_registry_spawn_ex("zcl_catchup",
                                  node_db_sync_catchup_job_thread, job,
                                  &job->thread) != 0) {
        job->started = false;
        atomic_store(&job->finished, false);
        LOG_FAIL("sync", "catchup_job_start: thread_registry_spawn_ex failed");
    }
    return true;
}

bool node_db_sync_catchup_job_join(struct node_db_sync_catchup_job *job,
                                   int *result_out)
{
    int join_rc;

    if (!job || !job->started)
        LOG_FAIL("sync", "catchup_job_join: invalid args (job=%p, started=%d)",
                 (void *)job, job ? job->started : 0);
    join_rc = pthread_join(job->thread, NULL);
    if (join_rc != 0)
        LOG_FAIL("sync", "catchup_job_join: pthread_join failed (rc=%d)", join_rc);
    job->started = false;
    atomic_store(&job->finished, false);
    if (result_out)
        *result_out = job->result;
    return true;
}

bool node_db_sync_catchup_job_is_started(
    const struct node_db_sync_catchup_job *job)
{
    return job && job->started;
}

static void *node_db_sync_import_job_thread(void *arg)
{
    struct node_db_sync_import_job *job = arg;

    if (!job) {
        LOG_NULL("sync", "import_job_thread: job is NULL");
    }

    job->result = node_db_sync_import_utxos(job->args.ndb, job->args.cvdb);
    return NULL;
}

void node_db_sync_import_job_init(struct node_db_sync_import_job *job)
{
    if (!job)
        return;
    memset(job, 0, sizeof(*job));
    job->result = -1;
}

bool node_db_sync_import_job_start(struct node_db_sync_import_job *job,
                                   struct node_db *ndb,
                                   struct coins_view_db *cvdb)
{
    if (!job || job->started || !ndb || !cvdb)
        LOG_FAIL("sync", "import_job_start: invalid args (job=%p, ndb=%p, cvdb=%p)",
                 (void *)job, (void *)ndb, (void *)cvdb);

    job->args.ndb = ndb;
    job->args.cvdb = cvdb;
    job->result = -1;
    if (thread_registry_spawn_ex("zcl_db_import",
                                  node_db_sync_import_job_thread, job,
                                  &job->thread) != 0)
        LOG_FAIL("sync", "import_job_start: thread_registry_spawn_ex failed");
    job->started = true;
    return true;
}

bool node_db_sync_import_job_join(struct node_db_sync_import_job *job,
                                  int *result_out)
{
    int join_rc;

    if (!job || !job->started)
        LOG_FAIL("sync", "import_job_join: invalid args (job=%p, started=%d)",
                 (void *)job, job ? job->started : 0);
    join_rc = pthread_join(job->thread, NULL);
    if (join_rc != 0)
        LOG_FAIL("sync", "import_job_join: pthread_join failed (rc=%d)", join_rc);
    job->started = false;
    if (result_out)
        *result_out = job->result;
    return true;
}

bool node_db_sync_import_job_is_started(
    const struct node_db_sync_import_job *job)
{
    return job && job->started;
}

static bool node_db_sync_wallet_keys_write(struct node_db *ndb, void *ctx)
{
    struct wallet_keys_sync_ctx *sync = ctx;
    const struct wallet *w = sync ? sync->wallet : NULL;

    if (!ndb->open || !w)
        return true;

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
        return true;

    int count = 0;
    bool tx_open = false;
    bool ok = true;
    if (!node_db_begin(ndb))
        LOG_FAIL("sync", "wallet_keys_write: BEGIN failed");
    tx_open = true;

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
        dbk.created_at = (int64_t)platform_time_wall_time_t();

        if (db_wallet_key_save(ndb, &dbk)) {
            count++;
        } else {
            ok = false;
            break;
        }
    }

    /* Sync Sapling keys */
    for (size_t i = 0; ok && i < w->sapling_keys.num_keys; i++) {
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

        if (db_sapling_key_save(ndb, &dbsk)) {
            count++;
        } else {
            ok = false;
            break;
        }
    }

    if (!ok) {
        if (tx_open)
            node_db_rollback(ndb);
        LOG_FAIL("sync", "wallet_keys_write: key save failed (count=%d)", count);
    }

    if (!node_db_commit(ndb)) {
        if (tx_open)
            node_db_rollback(ndb);
        LOG_FAIL("sync", "wallet_keys_write: COMMIT failed (count=%d)", count);
    }
    if (sync)
        sync->count = count;
    return true;
}

int node_db_sync_wallet_keys(struct node_db *ndb,
                             const struct wallet *w)
{
    struct wallet_keys_sync_ctx ctx = {.wallet = w, .count = 0};

    if (!ndb->open || !w)
        return 0;

    if (!sync_run_write(ndb, node_db_sync_wallet_keys_write, &ctx)) {
        LOG_WARN("sync", "SQLite: wallet key sync failed");
        return 0;
    }

    if (ctx.count > 0)
        printf("SQLite: synced %d wallet keys\n", ctx.count);
    return ctx.count;
}

static bool node_db_sync_mempool_save_write(struct node_db *ndb, void *ctx)
{
    struct mempool_save_ctx *save = ctx;
    const struct tx_mempool *mempool = save ? save->mempool : NULL;

    if (!ndb->open || !mempool)
        return true;

    int count = 0;
    bool tx_open = false;
    bool ok = true;
    if (!node_db_begin(ndb))
        LOG_FAIL("sync", "mempool_save_write: BEGIN failed");
    tx_open = true;

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

        if (db_mempool_save(ndb, &e)) {
            count++;
        } else {
            ok = false;
        }
        free(raw);
        if (!ok)
            break;
    }

    if (!ok) {
        if (tx_open)
            node_db_rollback(ndb);
        LOG_FAIL("sync", "mempool_save_write: save failed (count=%d)", count);
    }

    if (!node_db_commit(ndb)) {
        if (tx_open)
            node_db_rollback(ndb);
        LOG_FAIL("sync", "mempool_save_write: COMMIT failed (count=%d)", count);
    }
    if (save)
        save->count = count;
    return true;
}

int node_db_sync_mempool_save(struct node_db *ndb,
                              const struct tx_mempool *mempool)
{
    struct mempool_save_ctx ctx = {.mempool = mempool, .count = 0};

    if (!ndb->open || !mempool) return 0;

    if (!sync_run_write(ndb, node_db_sync_mempool_save_write, &ctx)) {
        LOG_WARN("sync", "SQLite: mempool save failed");
        return 0;
    }

    if (ctx.count > 0)
        printf("SQLite: saved %d mempool transactions\n", ctx.count);
    return ctx.count;
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
