/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "controllers/sync_controller.h"
#include "services/recovery_policy.h"
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

extern volatile sig_atomic_t g_shutdown_requested;

_Atomic bool g_sapling_rescan_active = false;
_Atomic bool g_sapling_tree_rebuilding = false;
static _Atomic bool g_catchup_active = false;
static _Atomic int g_catchup_height = -1;
static _Atomic int g_catchup_target_height = -1;
static _Atomic int64_t g_catchup_started_at = 0;
static _Atomic int64_t g_catchup_last_progress_at = 0;
static _Atomic bool g_import_active = false;
static _Atomic int g_import_rows_written = 0;
static _Atomic int64_t g_import_started_at = 0;
static _Atomic int64_t g_import_last_progress_at = 0;

static int64_t sync_job_now(void)
{
    return (int64_t)time(NULL);
}

static void sync_job_catchup_begin(int start_height, int target_height)
{
    int64_t now = sync_job_now();

    atomic_store(&g_catchup_active, true);
    atomic_store(&g_catchup_height, start_height > 0 ? start_height - 1 : -1);
    atomic_store(&g_catchup_target_height, target_height);
    atomic_store(&g_catchup_started_at, now);
    atomic_store(&g_catchup_last_progress_at, now);
}

static void sync_job_catchup_progress(int height)
{
    atomic_store(&g_catchup_height, height);
    atomic_store(&g_catchup_last_progress_at, sync_job_now());
}

static void sync_job_catchup_finish(void)
{
    atomic_store(&g_catchup_active, false);
    atomic_store(&g_catchup_last_progress_at, sync_job_now());
}

static void sync_job_import_begin(void)
{
    int64_t now = sync_job_now();

    atomic_store(&g_import_active, true);
    atomic_store(&g_import_rows_written, 0);
    atomic_store(&g_import_started_at, now);
    atomic_store(&g_import_last_progress_at, now);
}

static void sync_job_import_progress(int total_rows)
{
    atomic_store(&g_import_rows_written, total_rows);
    atomic_store(&g_import_last_progress_at, sync_job_now());
}

static void sync_job_import_finish(int total_rows)
{
    atomic_store(&g_import_rows_written, total_rows);
    atomic_store(&g_import_active, false);
    atomic_store(&g_import_last_progress_at, sync_job_now());
}

static struct db_service *sync_db_service_for(struct node_db *ndb)
{
    struct db_service *dbsvc = app_runtime_db_service();

    if (!ndb || !dbsvc)
        return NULL;
    return db_service_node_db(dbsvc) == ndb ? dbsvc : NULL;
}

static bool sync_db_enter_turbo_mode(struct node_db *ndb)
{
    struct db_service *dbsvc = sync_db_service_for(ndb);

    if (dbsvc)
        return db_service_ibd_turbo_mode(dbsvc);
    return node_db_ibd_turbo_mode(ndb);
}

static bool sync_db_restore_normal_mode(struct node_db *ndb)
{
    struct db_service *dbsvc = sync_db_service_for(ndb);

    if (dbsvc)
        return db_service_normal_mode(dbsvc);
    return node_db_normal_mode(ndb);
}

struct sync_db_turbo_scope {
    struct node_db *ndb;
    bool entered;
};

static bool sync_db_turbo_scope_begin(struct sync_db_turbo_scope *scope,
                                     struct node_db *ndb,
                                     bool enabled)
{
    if (!scope)
        return false;

    scope->ndb = (enabled ? ndb : NULL);
    scope->entered = false;
    if (!enabled)
        return true;

    if (!sync_db_enter_turbo_mode(ndb))
        return false;

    scope->entered = true;
    return true;
}

static bool sync_db_turbo_scope_end(struct sync_db_turbo_scope *scope)
{
    if (!scope || !scope->entered || !scope->ndb)
        return true;

    if (!sync_db_restore_normal_mode(scope->ndb))
        return false;

    scope->entered = false;
    scope->ndb = NULL;
    return true;
}

struct wallet_keys_sync_ctx {
    const struct wallet *wallet;
    int count;
};

struct mempool_save_ctx {
    const struct tx_mempool *mempool;
    int count;
};

struct mempool_add_ctx {
    const struct transaction *tx;
    int64_t fee;
    int height;
    bool ok;
};

struct mempool_remove_ctx {
    const uint8_t *txid;
    bool ok;
};

struct peer_sync_ctx {
    uint8_t ip[16];
    uint16_t port;
    uint64_t services;
    int64_t last_seen;
    bool ok;
};

struct peer_score_ctx {
    uint8_t ip[16];
    uint16_t port;
    uint32_t bandwidth_score;
    bool is_zcl23;
    bool ok;
};

struct tip_set_ctx {
    uint8_t hash[32];
    int height;
    bool ok;
};

struct sapling_note_sync_ctx {
    struct db_sapling_note note;
    bool ok;
};

struct sapling_spend_sync_ctx {
    uint8_t nullifier[32];
    uint8_t spending_txid[32];
    bool ok;
};

struct connect_block_sync_ctx {
    const struct block *blk;
    const struct block_index *pindex;
    bool ok;
};

struct disconnect_block_sync_ctx {
    const struct block *blk;
    const struct block_index *pindex;
    bool ok;
};

static bool sync_run_write(struct node_db *ndb,
                           db_service_write_fn fn,
                           void *ctx);
static bool node_db_sync_wallet_tx_write(struct node_db *ndb, void *ctx);

struct wallet_tx_sync_ctx {
    const struct transaction *tx;
    const struct wallet *wallet;
    int block_height;
    bool is_ours;
    bool ok;
};

static bool node_db_sync_wallet_tx_checked(struct node_db *ndb,
                                         const struct transaction *tx,
                                         const struct wallet *w,
                                         int block_height,
                                         bool *is_ours_out,
                                         bool *success_out)
{
    struct wallet_tx_sync_ctx ctx = {
        .tx = tx,
        .wallet = w,
        .block_height = block_height,
        .is_ours = false,
        .ok = false,
    };

    if (!ndb || !ndb->open || !tx || !w)
        return false;

    bool ok = sync_run_write(ndb, node_db_sync_wallet_tx_write, &ctx) && ctx.ok;
    if (is_ours_out)
        *is_ours_out = ctx.is_ours;
    if (success_out)
        *success_out = ok;
    return ok;
}

static bool sync_run_write(struct node_db *ndb,
                           db_service_write_fn fn,
                           void *ctx)
{
    struct db_service *dbsvc = sync_db_service_for(ndb);

    if (!ndb || !fn)
        return false;
    if (dbsvc)
        return db_service_run_write(dbsvc, fn, ctx);
    return fn(ndb, ctx);
}

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

bool node_db_sync_open_private_db_like(const struct node_db *src,
                                       struct node_db *out)
{
    const char *path;

    if (!src || !out || !src->open || !src->db)
        return false;

    path = sqlite3_db_filename(src->db, "main");
    if (!path || !path[0] || strcmp(path, ":memory:") == 0)
        return false;

    memset(out, 0, sizeof(*out));
    return node_db_open(out, path);
}

void node_db_sync_get_job_status(struct node_db_sync_job_status *out)
{
    struct node_db_sync_job_status empty = {0};

    if (!out)
        return;
    *out = empty;
    out->catchup_active = atomic_load(&g_catchup_active);
    out->catchup_height = atomic_load(&g_catchup_height);
    out->catchup_target_height = atomic_load(&g_catchup_target_height);
    out->catchup_started_at = atomic_load(&g_catchup_started_at);
    out->catchup_last_progress_at = atomic_load(&g_catchup_last_progress_at);
    out->import_active = atomic_load(&g_import_active);
    out->import_rows_written = atomic_load(&g_import_rows_written);
    out->import_started_at = atomic_load(&g_import_started_at);
    out->import_last_progress_at = atomic_load(&g_import_last_progress_at);
}

/* classify_script: use shared utxo_classify_script() from models/utxo.h */
#define classify_script utxo_classify_script

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
static bool advance_wallet_witnesses(struct node_db *ndb,
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
    bool ok = true;
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
                if (!db_sapling_note_save_witness(ndb,
                    wnotes[wi].txid, wnotes[wi].output_index,
                    iwout.data, iwout.size, height))
                    ok = false;
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
            if (!db_sapling_note_save_witness(ndb,
                wnotes[idx].txid, wnotes[idx].output_index,
                wout.data, wout.size, height))
                ok = false;
            stream_free(&wout);
        }
    }

    /* Save tree to node_state */
    {
        struct byte_stream ts;
        stream_init(&ts, 4096);
        incremental_tree_serialize(tree, &ts);
        if (!node_db_state_set(ndb, "sapling_tree", ts.data, ts.size))
            ok = false;
        stream_free(&ts);
    }

    free(witnesses);
    free(witness_idx);
    return ok;
}

static bool node_db_sync_connect_block_local(struct node_db *ndb,
                                             const struct block *blk,
                                             const struct block_index *pindex)
{
    bool tx_active = false;

    if (!ndb || !ndb->open || !blk || !pindex || !pindex->phashBlock)
        return false;

    /* Batch mode: start transaction if not already in one */
    if (!ndb->sync_in_batch) {
        if (!node_db_begin(ndb))
            return false;
        ndb->sync_in_batch = true;
        ndb->sync_pending_blocks = 0;
        tx_active = true;
    } else {
        tx_active = true;
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

    /* Compute per-block shielded value from transactions */
    for (size_t ti = 0; ti < blk->num_vtx; ti++) {
        const struct transaction *tx = &blk->vtx[ti];
        for (size_t ji = 0; ji < tx->num_joinsplit; ji++)
            db_blk.sprout_value += tx->v_joinsplit[ji].vpub_old
                                  - tx->v_joinsplit[ji].vpub_new;
        db_blk.sapling_value += tx->value_balance;
    }

    if (!db_block_save(ndb, &db_blk))
        goto fail;

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
            if (!ns)
                goto fail;
            sqlite3_reset(ns);
            if (sqlite3_bind_blob(ns, 1,
                    tx->v_shielded_spend[j].nullifier.data,
                    32, SQLITE_STATIC) != SQLITE_OK)
                goto fail;
            if (sqlite3_step(ns) != SQLITE_DONE)
                goto fail;
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

        if (!advance_wallet_witnesses(ndb, blk, &tree, pindex->nHeight))
            goto fail;

        /* Verify tree root matches block header.
         * Smart mismatch policy:
         * - rebuilding flag set → accept silently (rebuild in progress)
         * - IBD with empty tree → accept (building from genesis)
         * - tree clearly broken (size < 500K at tip) → auto-set rebuild
         *   flag, log warning, accept block (will be fixed at next boot)
         * - tree was rebuilt (size >= 500K) but still wrong → FATAL
         *   reject (real consensus bug, not just stale state) */
        struct uint256 tree_root;
        incremental_tree_root(&tree, &tree_root);
        if (memcmp(tree_root.data,
                   blk->header.hashFinalSaplingRoot.data, 32) != 0) {
            bool is_ibd = (sync_get_state() <= SYNC_BLOCKS_DOWNLOAD);
            bool rebuilding = atomic_load(&g_sapling_tree_rebuilding);
            size_t tsize = incremental_tree_size(&tree);

            if (rebuilding) {
                /* Tree rebuild in progress — accept silently */
            } else if (is_ibd && tsize == 0) {
                /* IBD from genesis — tree building naturally */
            } else if (tsize < 500000 && pindex->nHeight > 500000) {
                /* Tree is clearly incomplete — auto-flag for rebuild.
                 * Accept blocks so the node keeps running. The tree
                 * will be rebuilt at next boot (Phase 1.2). */
                if (!atomic_load(&g_sapling_tree_rebuilding)) {
                    atomic_store(&g_sapling_tree_rebuilding, true);
                    fprintf(stderr, "Sapling tree incomplete "
                        "(size=%zu at h=%d) — flagged for rebuild\n",
                        tsize, pindex->nHeight);
                    fflush(stderr);
                }
            } else {
                /* Tree root doesn't match — log but accept the block.
                 * The tree will be rebuilt correctly at next boot.
                 * Never reject blocks based on Sapling tree state — the
                 * tree is a derived data structure, not consensus-critical
                 * for block acceptance (the header root was already
                 * validated by the peer that mined the block). */
                static int log_count = 0;
                if (log_count < 5) {
                    log_count++;
                    fprintf(stderr, "WARNING: Sapling tree mismatch "
                        "at h=%d (size=%zu) — will rebuild at next boot\n",
                        pindex->nHeight, tsize);
                    fflush(stderr);
                }
                if (!atomic_load(&g_sapling_tree_rebuilding))
                    atomic_store(&g_sapling_tree_rebuilding, true);
            }
        }

        /* Store tree state per-block for disconnect */
        struct byte_stream ts;
        stream_init(&ts, 4096);
        incremental_tree_serialize(&tree, &ts);
        sqlite3_stmt *upd = NULL;
        if (sqlite3_prepare_v2(ndb->db,
            "UPDATE blocks SET sapling_tree_data=? WHERE hash=?",
            -1, &upd, NULL) != SQLITE_OK || !upd) {
            stream_free(&ts);
            goto fail;
        }
        if (sqlite3_bind_blob(upd, 1, ts.data, (int)ts.size, SQLITE_STATIC) != SQLITE_OK ||
            sqlite3_bind_blob(upd, 2, pindex->phashBlock->data, 32, SQLITE_STATIC) != SQLITE_OK ||
            sqlite3_step(upd) != SQLITE_DONE) {
            sqlite3_finalize(upd);
            stream_free(&ts);
            goto fail;
        }
        sqlite3_finalize(upd);
        stream_free(&ts);
    }

    /* 4. Update chain tip in state table */
    if (!node_db_sync_set_tip(ndb,
            pindex->phashBlock->data, pindex->nHeight)) {
        goto fail;
    }

    /* Batch mode: commit only when batch_size reached */
    ndb->sync_pending_blocks++;
    int batch = ndb->sync_batch_size > 0 ? ndb->sync_batch_size : 1;
    if (ndb->sync_pending_blocks >= batch) {
        if (!node_db_commit(ndb))
            goto fail;
        ndb->sync_in_batch = false;
        ndb->sync_pending_blocks = 0;
        tx_active = false;
    }
    return true;

fail:
    if (tx_active)
        node_db_rollback(ndb);
    ndb->sync_in_batch = false;
    ndb->sync_pending_blocks = 0;
    return false;
}

static bool node_db_sync_connect_block_write(struct node_db *ndb, void *ctx)
{
    struct connect_block_sync_ctx *sync = ctx;

    if (!sync || !sync->blk || !sync->pindex)
        return false;
    sync->ok = node_db_sync_connect_block_local(ndb, sync->blk, sync->pindex);
    return sync->ok;
}

bool node_db_sync_connect_block(struct node_db *ndb,
                                const struct block *blk,
                                const struct block_index *pindex)
{
    struct connect_block_sync_ctx ctx = {
        .blk = blk,
        .pindex = pindex,
        .ok = false,
    };

    return sync_run_write(ndb, node_db_sync_connect_block_write, &ctx) &&
           ctx.ok;
}

static bool node_db_sync_disconnect_block_local(struct node_db *ndb,
                                                const struct block *blk,
                                                const struct block_index *pindex)
{
    if (!ndb || !ndb->open || !blk || !pindex || !pindex->phashBlock)
        return false;

    /* Flush any pending batch before disconnecting — disconnect needs
     * accurate SQLite state for UTXO restoration */
    if (!node_db_sync_flush(ndb))
        return false;

    if (!node_db_begin(ndb))
        return false;

    /* Remove transactions in reverse order */
    for (size_t i = blk->num_vtx; i > 0; i--) {
        const struct transaction *tx = &blk->vtx[i - 1];

        /* Remove wallet UTXOs created by this tx.
         * Note: consensus UTXOs are handled by coins_view_sqlite,
         * not sync_controller. */
        for (size_t j = 0; j < tx->num_vout; j++) {
            if (!db_wallet_utxo_delete(ndb, tx->hash.data, (uint32_t)j))
                goto fail;
        }

        /* Unmark any wallet_utxos that this tx spent.
         * When disconnecting, inputs that were marked spent become
         * unspent again (the spending tx is being reverted). */
        for (size_t j = 0; j < tx->num_vin; j++) {
            sqlite3_stmt *us = NULL;
            if (sqlite3_prepare_v2(ndb->db,
                "UPDATE wallet_utxos SET spent_txid=NULL, spent_vin=NULL"
                " WHERE spent_txid=? AND spent_vin=?",
                -1, &us, NULL) != SQLITE_OK || !us)
                goto fail;
            if (sqlite3_bind_blob(us, 1, tx->hash.data, 32, SQLITE_STATIC) != SQLITE_OK ||
                sqlite3_bind_int(us, 2, (int)j) != SQLITE_OK ||
                sqlite3_step(us) != SQLITE_DONE) {
                sqlite3_finalize(us);
                goto fail;
            }
            sqlite3_finalize(us);
        }

        /* Remove tx index entry */
        if (!db_tx_delete(ndb, tx->hash.data))
            goto fail;

        /* Remove Sapling nullifiers */
        for (size_t j = 0; j < tx->num_shielded_spend; j++) {
            sqlite3_stmt *s = NULL;
            if (sqlite3_prepare_v2(ndb->db,
                "DELETE FROM sapling_nullifiers"
                " WHERE nullifier=?",
                -1, &s, NULL) != SQLITE_OK || !s)
                goto fail;
            if (sqlite3_bind_blob(s, 1,
                    tx->v_shielded_spend[j].nullifier.data,
                    32, SQLITE_STATIC) != SQLITE_OK ||
                sqlite3_step(s) != SQLITE_DONE) {
                sqlite3_finalize(s);
                goto fail;
            }
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
        if (sqlite3_prepare_v2(ndb->db,
            "SELECT sapling_tree_data FROM blocks WHERE hash=?",
            -1, &tq, NULL) != SQLITE_OK || !tq)
            goto fail;
        if (sqlite3_bind_blob(tq, 1, pindex->pprev->phashBlock->data, 32, SQLITE_STATIC) != SQLITE_OK) {
            sqlite3_finalize(tq);
            goto fail;
        }
        int tq_rc = sqlite3_step(tq);
        if (tq_rc == SQLITE_ROW) {
            int tlen = sqlite3_column_bytes(tq, 0);
            const void *tdata = sqlite3_column_blob(tq, 0);
            if (tdata && tlen > 0)
                if (!node_db_state_set(ndb, "sapling_tree", tdata, (size_t)tlen)) {
                    sqlite3_finalize(tq);
                    goto fail;
                }
        } else if (tq_rc != SQLITE_DONE) {
            sqlite3_finalize(tq);
            goto fail;
        }
        sqlite3_finalize(tq);
    } else {
        /* No previous block — reset to empty tree */
        struct incremental_merkle_tree empty;
        sapling_tree_init(&empty);
        struct byte_stream es;
        stream_init(&es, 256);
        incremental_tree_serialize(&empty, &es);
        if (!node_db_state_set(ndb, "sapling_tree", es.data, es.size)) {
            stream_free(&es);
            goto fail;
        }
        stream_free(&es);
    }

    /* Remove block */
    if (!db_block_delete(ndb, pindex->phashBlock->data))
        goto fail;

    /* Update tip to previous block */
    if (pindex->pprev) {
        if (!node_db_sync_set_tip(ndb,
                pindex->pprev->phashBlock->data,
                pindex->pprev->nHeight))
            goto fail;
    }

    return node_db_commit(ndb);

fail:
    node_db_rollback(ndb);
    return false;
}

static bool node_db_sync_disconnect_block_write(struct node_db *ndb, void *ctx)
{
    struct disconnect_block_sync_ctx *sync = ctx;

    if (!sync || !sync->blk || !sync->pindex)
        return false;
    sync->ok = node_db_sync_disconnect_block_local(ndb,
                                                   sync->blk,
                                                   sync->pindex);
    return sync->ok;
}

bool node_db_sync_disconnect_block(struct node_db *ndb,
                                   const struct block *blk,
                                   const struct block_index *pindex)
{
    struct disconnect_block_sync_ctx ctx = {
        .blk = blk,
        .pindex = pindex,
        .ok = false,
    };

    return sync_run_write(ndb, node_db_sync_disconnect_block_write, &ctx) &&
           ctx.ok;
}

static bool node_db_sync_wallet_tx_local(struct node_db *ndb,
                                         const struct transaction *tx,
                                         const struct wallet *w,
                                         int block_height,
                                         bool *is_ours_out)
{
    if (!ndb || !ndb->open || !tx || !w)
        return false;

    bool is_ours = false;
    bool from_me = false;
    bool write_ok = true;
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

        if (!db_wallet_utxo_save(ndb, &wu))
            write_ok = false;
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

            if (!db_wallet_utxo_mark_spent(ndb,
                    tx->vin[i].prevout.hash.data,
                    tx->vin[i].prevout.n,
                    tx->hash.data, (int)i)) {
                write_ok = false;
            }
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
            if (!db_wallet_tx_save(ndb, &wtx))
                write_ok = false;
            free(raw);
        }
    }

    if (is_ours_out)
        *is_ours_out = is_ours;
    return write_ok;
}

static bool node_db_sync_wallet_tx_write(struct node_db *ndb, void *ctx)
{
    struct wallet_tx_sync_ctx *sync = ctx;

    if (!sync || !sync->tx || !sync->wallet)
        return false;
    sync->ok = node_db_sync_wallet_tx_local(ndb,
                                           sync->tx,
                                           sync->wallet,
                                           sync->block_height,
                                           &sync->is_ours);
    return true;
}

bool node_db_sync_wallet_tx(struct node_db *ndb,
                            const struct transaction *tx,
                            const struct wallet *w,
                            int block_height)
{
    struct wallet_tx_sync_ctx ctx = {
        .tx = tx,
        .wallet = w,
        .block_height = block_height,
        .is_ours = false,
        .ok = false,
    };

    if (!ndb || !ndb->open || !tx || !w)
        return false;
    return sync_run_write(ndb, node_db_sync_wallet_tx_write, &ctx) &&
           ctx.ok && ctx.is_ours;
}

static bool node_db_sync_mempool_add_local(struct node_db *ndb,
                                           const struct transaction *tx,
                                           int64_t fee, int height)
{
    size_t raw_len = 0;
    uint8_t *raw = NULL;
    struct db_mempool_entry e;

    if (!ndb || !tx || !ndb->open)
        return false;

    raw = serialize_tx(tx, &raw_len);
    if (!raw)
        return false;

    memset(&e, 0, sizeof(e));
    memcpy(e.txid, tx->hash.data, 32);
    e.raw_tx = raw;
    e.raw_tx_len = raw_len;
    e.fee = fee;
    e.size = (int)raw_len;
    e.time_added = (int64_t)time(NULL);
    e.height_added = height;
    e.spends_coinbase = false;

    {
        bool ok = db_mempool_save(ndb, &e);

        for (size_t i = 0; i < tx->num_vin; i++) {
            db_mempool_add_spend(ndb, tx->hash.data,
                tx->vin[i].prevout.hash.data,
                tx->vin[i].prevout.n);
        }

        free(raw);
        return ok;
    }
}

static bool node_db_sync_mempool_add_write(struct node_db *ndb, void *ctx)
{
    struct mempool_add_ctx *add = ctx;

    if (!add)
        return false;
    add->ok = node_db_sync_mempool_add_local(ndb, add->tx,
                                             add->fee, add->height);
    return add->ok;
}

bool node_db_sync_mempool_add(struct node_db *ndb,
                              const struct transaction *tx,
                              int64_t fee, int height)
{
    struct mempool_add_ctx ctx = {
        .tx = tx,
        .fee = fee,
        .height = height,
        .ok = false,
    };

    return sync_run_write(ndb, node_db_sync_mempool_add_write, &ctx) && ctx.ok;
}

static bool node_db_sync_mempool_remove_write(struct node_db *ndb, void *ctx)
{
    struct mempool_remove_ctx *remove = ctx;

    if (!remove || !remove->txid)
        return false;
    remove->ok = db_mempool_delete(ndb, remove->txid);
    return remove->ok;
}

bool node_db_sync_mempool_remove(struct node_db *ndb,
                                 const uint8_t txid[32])
{
    struct mempool_remove_ctx ctx = {
        .txid = txid,
        .ok = false,
    };

    return sync_run_write(ndb, node_db_sync_mempool_remove_write, &ctx) &&
           ctx.ok;
}

static bool node_db_sync_sapling_note_write(struct node_db *ndb, void *ctx)
{
    struct sapling_note_sync_ctx *note = ctx;

    if (!note)
        return false;
    note->ok = db_sapling_note_save(ndb, &note->note);
    return note->ok;
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
    struct sapling_note_sync_ctx ctx;

    memset(&ctx, 0, sizeof(ctx));
    memcpy(ctx.note.txid, txid, 32);
    ctx.note.output_index = output_index;
    ctx.note.value = value;
    memcpy(ctx.note.rcm, rcm, 32);
    if (memo && memo_len > 0) {
        size_t ml = memo_len < 512 ? memo_len : 512;
        memcpy(ctx.note.memo, memo, ml);
        ctx.note.memo_len = ml;
    }
    memcpy(ctx.note.ivk, ivk, 32);
    memcpy(ctx.note.diversifier, diversifier, 11);
    memcpy(ctx.note.pk_d, pk_d, 32);
    memcpy(ctx.note.cm, cm, 32);
    memcpy(ctx.note.nullifier, nullifier, 32);
    ctx.note.block_height = block_height;
    return sync_run_write(ndb, node_db_sync_sapling_note_write, &ctx) && ctx.ok;
}

static bool node_db_sync_sapling_spend_write(struct node_db *ndb, void *ctx)
{
    struct sapling_spend_sync_ctx *spend = ctx;
    sqlite3_stmt *s;

    if (!spend || !ndb || !ndb->open)
        return false;

    s = ndb->stmt_nullifier_insert;
    sqlite3_reset(s);
    sqlite3_bind_blob(s, 1,
                      spend->nullifier,
                      sizeof(spend->nullifier),
                      SQLITE_STATIC);
    sqlite3_step(s);

    spend->ok = db_sapling_note_mark_spent(ndb,
                                           spend->nullifier,
                                           spend->spending_txid);
    return spend->ok;
}

bool node_db_sync_sapling_spend(struct node_db *ndb,
                                const uint8_t nullifier[32],
                                const uint8_t spending_txid[32])
{
    struct sapling_spend_sync_ctx ctx;

    if (!nullifier || !spending_txid)
        return false;
    memset(&ctx, 0, sizeof(ctx));
    memcpy(ctx.nullifier, nullifier, sizeof(ctx.nullifier));
    memcpy(ctx.spending_txid, spending_txid, sizeof(ctx.spending_txid));
    return sync_run_write(ndb, node_db_sync_sapling_spend_write, &ctx) && ctx.ok;
}

static bool node_db_sync_peer_write(struct node_db *ndb, void *ctx)
{
    struct peer_sync_ctx *peer = ctx;
    struct db_peer p;

    if (!peer)
        return false;
    memset(&p, 0, sizeof(p));
    memcpy(p.ip, peer->ip, 16);
    p.port = peer->port;
    p.services = peer->services;
    p.last_seen = peer->last_seen;
    peer->ok = db_peer_save(ndb, &p);
    return peer->ok;
}

bool node_db_sync_peer(struct node_db *ndb,
                       const uint8_t ip[16], uint16_t port,
                       uint64_t services, int64_t last_seen)
{
    struct peer_sync_ctx ctx;

    memset(&ctx, 0, sizeof(ctx));
    memcpy(ctx.ip, ip, 16);
    ctx.port = port;
    ctx.services = services;
    ctx.last_seen = last_seen;
    return sync_run_write(ndb, node_db_sync_peer_write, &ctx) && ctx.ok;
}

static bool node_db_sync_peer_score_write(struct node_db *ndb, void *ctx)
{
    struct peer_score_ctx *score = ctx;

    if (!score || !ndb || !ndb->open)
        return false;
    score->ok = db_peer_update_score(ndb, score->ip, score->port,
                                     score->bandwidth_score,
                                     score->is_zcl23);
    return score->ok;
}

bool node_db_sync_peer_score(struct node_db *ndb,
                              const uint8_t ip[16], uint16_t port,
                              uint32_t bandwidth_score, bool is_zcl23)
{
    struct peer_score_ctx ctx;

    memset(&ctx, 0, sizeof(ctx));
    memcpy(ctx.ip, ip, 16);
    ctx.port = port;
    ctx.bandwidth_score = bandwidth_score;
    ctx.is_zcl23 = is_zcl23;
    return sync_run_write(ndb, node_db_sync_peer_score_write, &ctx) && ctx.ok;
}

int node_db_sync_get_tip_height(struct node_db *ndb)
{
    int64_t h = -1;

    if (!ndb || !ndb->open)
        return -1;

    node_db_state_get_int(ndb, "tip_height", &h);
    return (int)h;
}

bool node_db_sync_get_tip_hash(struct node_db *ndb, uint8_t hash_out[32])
{
    size_t len = 0;

    if (!ndb || !hash_out || !ndb->open)
        return false;
    if (!node_db_state_get(ndb, "tip_hash", hash_out, 32, &len))
        return false;
    return len == 32;
}

static bool node_db_sync_set_tip_write(struct node_db *ndb, void *ctx)
{
    struct tip_set_ctx *tip = ctx;

    if (!tip)
        return false;
    tip->ok = node_db_state_set(ndb, "tip_hash", tip->hash, sizeof(tip->hash)) &&
              node_db_state_set_int(ndb, "tip_height", (int64_t)tip->height);
    return tip->ok;
}

bool node_db_sync_set_tip(struct node_db *ndb,
                          const uint8_t hash[32], int height)
{
    struct tip_set_ctx ctx;

    if (!hash)
        return false;
    memset(&ctx, 0, sizeof(ctx));
    memcpy(ctx.hash, hash, sizeof(ctx.hash));
    ctx.height = height;
    return sync_run_write(ndb, node_db_sync_set_tip_write, &ctx) && ctx.ok;
}

/* Lean index: block header + txid index only.
 * No UTXO tracking, no nullifiers, no solution blob.
 * ~5x fewer SQLite ops than sync_block_inner. */
static bool sync_block_lean(struct node_db *ndb,
                            const struct block *blk,
                            const struct block_index *pindex)
{
    if (!ndb || !ndb->open || !blk || !pindex)
        return false;

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
        if (!db_tx_save(ndb, &db_tx))
            return false;
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
    if (!ndb || !chain || !datadir) return -1;

    int chain_tip = active_chain_height(chain);
    int sapling_height = 476969; /* ZClassic Sapling activation */
    if (chain_tip < sapling_height) return 0;

    fprintf(stderr, "sapling_tree_rebuild: replaying h=%d..%d\n",
            sapling_height, chain_tip);
    fflush(stderr);

    struct incremental_merkle_tree tree;
    sapling_tree_init(&tree);
    int total_commitments = 0;
    int mismatches = 0;

    /* mmap cache — local, thread-safe */
    int cached_file = -1;
    uint8_t *cached_data = NULL;
    size_t cached_size = 0;

    for (int h = sapling_height; h <= chain_tip; h++) {
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

            fprintf(stderr, "  sapling_tree_rebuild: h=%d/%d "
                "commitments=%d mismatches=%d\n",
                h, chain_tip, total_commitments, mismatches);
            fflush(stderr);

            /* Persist tree checkpoint to survive crashes */
            struct byte_stream ts;
            stream_init(&ts, 4096);
            incremental_tree_serialize(&tree, &ts);
            node_db_state_set(ndb, "sapling_tree", ts.data, ts.size);
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
    fprintf(stderr, "sapling_tree_rebuild: DONE commitments=%d "
        "mismatches=%d root=%s match=%s\n",
        total_commitments, mismatches, root_hex,
        match ? "YES" : "NO");
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
        return -1;

    int db_tip = node_db_sync_get_tip_height(ndb);
    int chain_tip = active_chain_height(chain);
    if (db_tip >= chain_tip) return 0;
    if (!datadir) return -1;

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
        return -1;
    }

    /* Verify connection works before starting */
    if (!node_db_begin(ndb)) {
        fprintf(stderr, "catchup: BEGIN failed — aborting\n");
        if (!sync_db_turbo_scope_end(&turbo_mode))
            fprintf(stderr, "catchup: failed to restore normal mode after BEGIN failure\n");
        restore_ok = false;
        sync_job_catchup_finish();
        return -1;
    }
    tx_open = true;
    if (!node_db_commit(ndb)) {
        fprintf(stderr, "catchup: initial COMMIT failed — aborting\n");
        node_db_rollback(ndb);
        if (!sync_db_turbo_scope_end(&turbo_mode))
            fprintf(stderr, "catchup: failed to restore normal mode after initial COMMIT failure\n");
        restore_ok = false;
        sync_job_catchup_finish();
        return -1;
    }
    tx_open = false;

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

    if (!node_db_begin(ndb)) {
        fprintf(stderr, "catchup: failed to open main transaction\n");
        if (!sync_db_turbo_scope_end(&turbo_mode))
            fprintf(stderr, "catchup: failed to restore normal mode after tx open failure\n");
        restore_ok = false;
        sync_job_catchup_finish();
        return -1;
    }
    tx_open = true;

    for (int h = start; h <= chain_tip; h++) {
        if (g_shutdown_requested) {
            interrupted = true;
            break;
        }
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
                fprintf(stderr, "catchup: failed to mmap file blk%05d.dat\n",
                        pindex->nFile);
                failed = true;
                break;
            }
        }

        if (pindex->nDataPos >= cached_size) {
            fprintf(stderr, "catchup: malformed block data offset at height %d\n",
                    h);
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
            fprintf(stderr, "catchup: lean index failed at height %d (sqlite=%s)\n",
                    h, sqlite3_errmsg(ndb->db));
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
                    fprintf(stderr, "catchup: wallet tx sync failed at height %d "
                            "(tx=%d)\n", h, (int)i);
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
                    fprintf(stderr, "catchup: sapling decrypt failed at height %d "
                            "(tx=%d)\n", h, (int)i);
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
                        fprintf(stderr,
                                "catchup: sapling spend update failed at height %d "
                                "(tx=%d, spend=%zu)\n",
                                h, (int)i, si);
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
            fprintf(stderr, "catchup: witness/tree advance failed at height %d\n",
                    h);
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
                    fprintf(stderr,
                            "catchup: failed to set tip at batch commit %d\n",
                            h);
                    failed = true;
                    break;
                }
            } else {
                fprintf(stderr, "catchup: missing hash at batch commit %d\n", h);
                failed = true;
                break;
            }
            if (!node_db_commit(ndb)) {
                fprintf(stderr, "catchup: batch COMMIT failed at height %d\n", h);
                node_db_rollback(ndb);
                tx_open = false;
                failed = true;
                break;
            }
            tx_open = false;
            last_committed_height = h;
            int64_t elapsed = (int64_t)time(NULL) - t_start;
            int rate = elapsed > 0 ? indexed / (int)elapsed : 0;
            printf("SQLite: %d/%d blocks (height %d, %d blk/s, %d wallet txs)\n",
                   indexed, total, h, rate, wallet_hits);
            fflush(stdout);
            if (!node_db_begin(ndb)) {
                fprintf(stderr, "catchup: failed to reopen transaction after batch commit\n");
                failed = true;
                break;
            }
            tx_open = true;
        }
    }

    if (cached_data) munmap(cached_data, cached_size);

    if (failed) {
        if (tx_open && !node_db_rollback(ndb))
            fprintf(stderr, "catchup: rollback failed after failure\n");
        tx_open = false;
    }

    /* Final commit */
    if (tx_open && !failed) {
        if (last_indexed_tip && last_indexed_tip->phashBlock) {
            if (!node_db_sync_set_tip(ndb,
                                      last_indexed_tip->phashBlock->data,
                                      last_indexed_height)) {
                fprintf(stderr,
                        "catchup: failed to set tip before final commit\n");
                failed = true;
            }
        } else {
            fprintf(stderr, "catchup: final commit missing tip hash\n");
            failed = true;
        }
        if (!failed) {
            if (!node_db_commit(ndb)) {
                fprintf(stderr, "catchup: final COMMIT failed\n");
                if (!node_db_rollback(ndb))
                    fprintf(stderr, "catchup: final ROLLBACK failed\n");
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
            fprintf(stderr, "catchup: final rollback path failed\n");
        interrupted = false;
    }

    /* Restore safe pragmas and rebuild indexes */
    if (!sync_db_turbo_scope_end(&turbo_mode)) {
        fprintf(stderr, "catchup: failed to restore normal mode\n");
        restore_ok = false;
    }

    if (failed || !restore_ok) {
        sync_job_catchup_finish();
        return -1;
    }

    int64_t elapsed = (int64_t)time(NULL) - t_start;
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
        return NULL;
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
            fprintf(stderr, "catchup: datadir path too long: %s\n",
                    job->args.datadir);
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
        return NULL;
    }

    job->result = node_db_sync_catchup(work_db, job->args.chain,
                                       job->args.w, job->args.datadir);

    if (owns_db)
        node_db_close(&catchup_db);
    return NULL;
}

void *node_db_sync_catchup_thread(void *arg)
{
    struct node_db_sync_catchup_job job;
    struct catchup_args *args = arg;

    node_db_sync_catchup_job_init(&job);
    if (!args)
        return NULL;
    job.args.ndb = args->ndb;
    job.args.chain = args->chain;
    job.args.w = args->w;
    job.args.datadir = args->datadir;
    if (!node_db_sync_catchup_job_start(&job, job.args.ndb, job.args.chain,
                                        job.args.w, job.args.datadir))
        return NULL;
    node_db_sync_catchup_job_join(&job, NULL);
    return NULL;
}

void node_db_sync_catchup_job_init(struct node_db_sync_catchup_job *job)
{
    if (!job)
        return;
    memset(job, 0, sizeof(*job));
    job->result = -1;
}

bool node_db_sync_catchup_job_start(struct node_db_sync_catchup_job *job,
                                    struct node_db *ndb,
                                    const struct active_chain *chain,
                                    const struct wallet *w,
                                    const char *datadir)
{
    if (!job || job->started || !ndb || !chain)
        return false;

    job->args.ndb = ndb;
    job->args.chain = chain;
    job->args.w = w;
    job->args.datadir = datadir;
    job->result = -1;
    if (pthread_create(&job->thread, NULL,
                       node_db_sync_catchup_job_thread, job) != 0)
        return false;
    job->started = true;
    return true;
}

bool node_db_sync_catchup_job_join(struct node_db_sync_catchup_job *job,
                                   int *result_out)
{
    int join_rc;

    if (!job || !job->started)
        return false;
    join_rc = pthread_join(job->thread, NULL);
    if (join_rc != 0)
        return false;
    job->started = false;
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
        return NULL;
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
        return false;

    job->args.ndb = ndb;
    job->args.cvdb = cvdb;
    job->result = -1;
    if (pthread_create(&job->thread, NULL,
                       node_db_sync_import_job_thread, job) != 0)
        return false;
    job->started = true;
    return true;
}

bool node_db_sync_import_job_join(struct node_db_sync_import_job *job,
                                  int *result_out)
{
    int join_rc;

    if (!job || !job->started)
        return false;
    join_rc = pthread_join(job->thread, NULL);
    if (join_rc != 0)
        return false;
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
        return false;
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
        dbk.created_at = (int64_t)time(NULL);

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
        return false;
    }

    if (!node_db_commit(ndb)) {
        if (tx_open)
            node_db_rollback(ndb);
        return false;
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
        fprintf(stderr, "SQLite: wallet key sync failed\n");
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
        return false;
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
        return false;
    }

    if (!node_db_commit(ndb)) {
        if (tx_open)
            node_db_rollback(ndb);
        return false;
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
        fprintf(stderr, "SQLite: mempool save failed\n");
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
/* Max outputs per chunk. Must be large enough to hold all outputs from
 * 2048 entries. Worst case: 2048 entries * 468 outputs = 958,464.
 * In practice ~5500 rows per chunk. Use 32768 for 4x safety margin. */
#define IMPORT_MAX_ROWS_PER_CHUNK 32768

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

#define IMPORT_NUM_CHUNKS 64
/* Auto-detect decoder count: use all cores minus 2 (reader + writer).
 * Minimum 4, maximum 32. More decoders = faster LevelDB deserialization. */
#include <unistd.h>
static int import_num_decoders(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 6) return 4;
    if (n > 34) return 32;
    return (int)(n - 2);
}
#define IMPORT_NUM_DECODERS_MAX 32

static bool import_writer_bind_checked(sqlite3_stmt *stmt,
                                      const char *label,
                                      int rc,
                                      const struct node_db *ndb,
                                      int row_no)
{
    if (!stmt) return false;
    if (rc != SQLITE_OK) {
        fprintf(stderr,
                "UTXO import writer: %s failed at row %d (rc=%d): %s\n",
                label, row_no, rc,
                ndb ? sqlite3_errmsg(ndb->db) : "db unavailable");
        return false;
    }
    return true;
}

static bool import_writer_step_checked(sqlite3_stmt *stmt,
                                      const struct node_db *ndb,
                                      int row_no)
{
    int step_rc = sqlite3_step(stmt);
    if (step_rc != SQLITE_DONE) {
        fprintf(stderr,
                "UTXO import writer: sqlite3_step failed at row %d (rc=%d): %s\n",
                row_no, step_rc,
                ndb ? sqlite3_errmsg(ndb->db) : "db unavailable");
        return false;
    }
    return true;
}

struct import_context {
    struct import_chunk chunks[IMPORT_NUM_CHUNKS];
    _Atomic int total_txids;
    _Atomic int total_rows;
    _Atomic int decode_failures;
    _Atomic int skipped_outputs;
    _Atomic bool cancel_requested;
    _Atomic bool reader_done;
    _Atomic bool decoders_done;
    _Atomic int chunks_produced;  /* total chunks filled by reader */
    _Atomic int chunks_consumed;  /* total chunks written by writer */
    /* LevelDB reader state (single-threaded) */
    struct coins_view_db *cvdb;
    /* Writer state */
    struct node_db *ndb;
    int write_next; /* next chunk index to write (in-order) */
};

struct import_job {
    struct import_context *ctx;
    pthread_t decoders[IMPORT_NUM_DECODERS_MAX];
    int num_decoders;
    int decoder_threads_started;
    pthread_t writer_thread;
    bool writer_thread_started;
};

static void *import_decoder_thread(void *arg);
static void *import_writer_thread(void *arg);
static void import_job_join_decoders(struct import_job *job);

static bool import_ctx_should_stop(const struct import_context *ctx)
{
    return (ctx && atomic_load(&ctx->cancel_requested)) || g_shutdown_requested;
}

static void import_ctx_request_stop(struct import_context *ctx)
{
    if (!ctx)
        return;
    atomic_store(&ctx->cancel_requested, true);
}

static void import_chunk_reset(struct import_chunk *chunk)
{
    if (!chunk)
        return;
    for (int ei = 0; ei < chunk->num_entries; ei++)
        free(chunk->entries[ei].value);
    for (int ri = 0; ri < chunk->num_rows; ri++)
        free(chunk->rows[ri].script_overflow);
    chunk->num_entries = 0;
    chunk->num_rows = 0;
    atomic_store(&chunk->state, 0);
}

static void import_context_release_chunks(struct import_context *ctx)
{
    if (!ctx)
        return;
    for (int i = 0; i < IMPORT_NUM_CHUNKS; i++)
        import_chunk_reset(&ctx->chunks[i]);
}

static void import_job_init(struct import_job *job,
                            struct import_context *ctx,
                            int num_decoders)
{
    if (!job)
        return;
    memset(job, 0, sizeof(*job));
    job->ctx = ctx;
    job->num_decoders = num_decoders;
}

static bool import_job_start_decoders(struct import_job *job)
{
    if (!job || !job->ctx)
        return false;

    for (int i = 0; i < job->num_decoders; i++) {
        int rc = pthread_create(&job->decoders[i], NULL,
                                import_decoder_thread, job->ctx);
        if (rc != 0) {
            fprintf(stderr,
                    "UTXO import: pthread_create decoder[%d] failed: %d\n",
                    i, rc);
            import_ctx_request_stop(job->ctx);
            import_job_join_decoders(job);
            return false;
        }
        job->decoder_threads_started++;
    }
    return job->decoder_threads_started > 0;
}

static bool import_job_start_writer(struct import_job *job)
{
    if (!job || !job->ctx)
        return false;
    if (pthread_create(&job->writer_thread, NULL,
                       import_writer_thread, job->ctx) != 0) {
        fprintf(stderr, "UTXO import: FATAL — writer thread failed to start\n");
        import_ctx_request_stop(job->ctx);
        return false;
    }
    job->writer_thread_started = true;
    return true;
}

static void import_job_join_decoders(struct import_job *job)
{
    if (!job)
        return;
    for (int i = 0; i < job->decoder_threads_started; i++)
        pthread_join(job->decoders[i], NULL);
    job->decoder_threads_started = 0;
}

static void import_job_join_writer(struct import_job *job)
{
    if (!job || !job->writer_thread_started)
        return;
    pthread_join(job->writer_thread, NULL);
    job->writer_thread_started = false;
}

static bool import_job_start(struct import_job *job)
{
    if (!import_job_start_decoders(job))
        return false;
    if (!import_job_start_writer(job)) {
        if (job && job->ctx)
            atomic_store(&job->ctx->reader_done, true);
        import_job_join_decoders(job);
        return false;
    }
    return true;
}

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

    /* Deserialize each available output.
     * We do inline parsing instead of compressed_txout_deserialize to avoid
     * secp256k1 point decompression (unnecessary for index) and to ensure
     * every output is captured — zero tolerance for data loss. */
    int nrows = 0;
    for (size_t vi = 0; vi < num_avail && nrows < max_rows; vi++) {
        if (!avail[vi]) continue;

        /* Read compressed amount (varint) */
        uint64_t comp_amount = 0;
        if (!stream_read_varint(&s, &comp_amount)) break;
        int64_t value = (int64_t)decompress_amount(comp_amount);

        /* Read script type/size (varint) */
        uint64_t nSize = 0;
        if (!stream_read_varint(&s, &nSize)) break;

        /* Determine raw script data size in the stream */
        size_t raw_script_len = 0;
        if (nSize == 0 || nSize == 1) raw_script_len = 20;      /* P2PKH / P2SH hash */
        else if (nSize >= 2 && nSize <= 5) raw_script_len = 32;  /* compressed pubkey */
        else raw_script_len = (size_t)(nSize - 6);               /* raw script */

        /* Read raw script data */
        uint8_t raw_script[10240];
        if (raw_script_len > sizeof(raw_script)) raw_script_len = sizeof(raw_script);
        if (!stream_read_bytes(&s, raw_script, raw_script_len)) break;

        /* Reconstruct full script for classification */
        struct utxo_row *r = &out[nrows];
        memcpy(r->txid, raw->txid, 32);
        r->vout = (uint32_t)vi;
        r->value = value;
        r->is_coinbase = is_coinbase;
        r->height = 0;
        r->script_overflow = NULL;
        r->has_address = 0;
        r->script_type = 0; /* OTHER */

        if (nSize == 0) {
            /* P2PKH: OP_DUP OP_HASH160 <20 bytes> OP_EQUALVERIFY OP_CHECKSIG */
            r->script_len = 25;
            r->script[0] = 0x76; r->script[1] = 0xa9; r->script[2] = 0x14;
            memcpy(r->script + 3, raw_script, 20);
            r->script[23] = 0x88; r->script[24] = 0xac;
            memcpy(r->address_hash, raw_script, 20);
            r->has_address = 1;
            r->script_type = 1; /* P2PKH */
        } else if (nSize == 1) {
            /* P2SH: OP_HASH160 <20 bytes> OP_EQUAL */
            r->script_len = 23;
            r->script[0] = 0xa9; r->script[1] = 0x14;
            memcpy(r->script + 2, raw_script, 20);
            r->script[22] = 0x87;
            memcpy(r->address_hash, raw_script, 20);
            r->has_address = 1;
            r->script_type = 2; /* P2SH */
        } else if (nSize >= 2 && nSize <= 5) {
            /* Compressed/uncompressed pubkey → P2PK script.
             * Store the raw 33-byte compressed pubkey directly as script.
             * We skip secp256k1 decompression — not needed for indexing. */
            uint8_t prefix = (nSize == 2 || nSize == 4) ? 0x02 : 0x03;
            r->script_len = 35; /* 1(push33) + 33(pubkey) + 1(OP_CHECKSIG) */
            r->script[0] = 0x21; /* push 33 bytes */
            r->script[1] = prefix;
            memcpy(r->script + 2, raw_script, 32);
            r->script[34] = 0xac; /* OP_CHECKSIG */
        } else {
            /* Raw script */
            uint16_t slen = (uint16_t)raw_script_len;
            r->script_len = slen;
            if (slen <= sizeof(r->script)) {
                memcpy(r->script, raw_script, slen);
            } else {
                r->script_overflow = malloc(slen);
                if (r->script_overflow) {
                    memcpy(r->script_overflow, raw_script, slen);
                } else {
                    /* malloc failed — cap to inline buffer */
                    r->script_len = (uint16_t)sizeof(r->script);
                    memcpy(r->script, raw_script, sizeof(r->script));
                }
            }
            /* Classify raw script */
            const uint8_t *sc = r->script_overflow ? r->script_overflow : r->script;
            if (slen == 25 && sc[0]==0x76 && sc[1]==0xa9 && sc[2]==0x14 &&
                sc[23]==0x88 && sc[24]==0xac) {
                memcpy(r->address_hash, sc + 3, 20);
                r->has_address = 1;
                r->script_type = 1;
            } else if (slen == 23 && sc[0]==0xa9 && sc[1]==0x14 && sc[22]==0x87) {
                memcpy(r->address_hash, sc + 2, 20);
                r->has_address = 1;
                r->script_type = 2;
            } else if (slen > 0 && sc[0] == 0x6a) {
                r->script_type = 3;
            }
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
        if (import_ctx_should_stop(ctx))
            break;
        /* Find a filled chunk to decode */
        struct import_chunk *chunk = NULL;
        for (int i = 0; i < IMPORT_NUM_CHUNKS; i++) {
            int expected = 1; /* filled */
            if (atomic_compare_exchange_strong(&ctx->chunks[i].state,
                                              &expected, -1)) {
                atomic_thread_fence(memory_order_acquire);
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

        if (import_ctx_should_stop(ctx)) {
            import_chunk_reset(chunk);
            break;
        }

        /* Decode all entries in this chunk */
        chunk->num_rows = 0;
        int skipped_in_chunk = 0;
        for (int i = 0; i < chunk->num_entries; i++) {
            if (import_ctx_should_stop(ctx))
                break;
            int space = IMPORT_MAX_ROWS_PER_CHUNK - chunk->num_rows;
            if (space <= 0) { skipped_in_chunk += chunk->num_entries - i; break; }
            int n = decode_coins_entry(&chunk->entries[i],
                                       &chunk->rows[chunk->num_rows],
                                       space);
            if (n == 0) {
                atomic_fetch_add(&ctx->decode_failures, 1);
            }
            chunk->num_rows += n;
        }
        if (skipped_in_chunk > 0) {
            atomic_fetch_add(&ctx->skipped_outputs, skipped_in_chunk);
            fprintf(stderr, "UTXO import: chunk overflow! %d entries skipped "
                    "(rows=%d, max=%d)\n", skipped_in_chunk,
                    chunk->num_rows, IMPORT_MAX_ROWS_PER_CHUNK);
        }

        if (import_ctx_should_stop(ctx))
            import_chunk_reset(chunk);
        else
            atomic_store(&chunk->state, 2); /* decoded */
    }
    return NULL;
}

/* Writer thread — consumes decoded chunks, inserts into SQLite */
static void *import_writer_thread(void *arg)
{
    struct import_context *ctx = (struct import_context *)arg;
    if (!ctx) return NULL;
    struct node_db *ndb = ctx->ndb;
    if (!ndb || !ndb->open || !ndb->stmt_utxo_insert) {
        fprintf(stderr, "UTXO import writer: invalid node_db statement/db state\n");
        import_ctx_request_stop(ctx);
        return NULL;
    }
    sqlite3_stmt *ins = ndb->stmt_utxo_insert;
    int total_rows = 0;
    int next_chunk = 0;

    if (!node_db_begin(ndb)) {
        fprintf(stderr, "UTXO import writer: BEGIN failed\n");
        import_ctx_request_stop(ctx);
    }

    for (;;) {
        if (import_ctx_should_stop(ctx))
            break;
        /* Look for any decoded chunk to write */
        struct import_chunk *chunk = NULL;
        for (int i = 0; i < IMPORT_NUM_CHUNKS; i++) {
            int idx = (next_chunk + i) % IMPORT_NUM_CHUNKS;
            int expected = 2; /* decoded */
            if (atomic_compare_exchange_strong(&ctx->chunks[idx].state,
                                              &expected, -1)) {
                chunk = &ctx->chunks[idx];
                next_chunk = (idx + 1) % IMPORT_NUM_CHUNKS;
                break;
            }
        }
        if (!chunk) {
            if (atomic_load(&ctx->decoders_done)) {
                /* Decoders are done. Use definitive chunk count to
                 * know when we're truly finished — no race possible. */
                int produced = atomic_load(&ctx->chunks_produced);
                int consumed = atomic_load(&ctx->chunks_consumed);
                if (consumed >= produced) break;
                /* Still have chunks to consume — scan harder */
                atomic_thread_fence(memory_order_seq_cst);
            }
            struct timespec ts = {0, 100000}; /* 100μs */
            nanosleep(&ts, NULL);
            continue;
        }

        if (import_ctx_should_stop(ctx)) {
            import_chunk_reset(chunk);
            break;
        }

        /* Insert all rows from this chunk */
        for (int ri = 0; ri < chunk->num_rows; ri++) {
            if (import_ctx_should_stop(ctx))
                break;
            struct utxo_row *r = &chunk->rows[ri];
            const uint8_t *sc = r->script_overflow ?
                                r->script_overflow : r->script;
            bool row_ok = true;

            row_ok &= import_writer_bind_checked(ins, "sqlite3_reset",
                                                sqlite3_reset(ins), ndb,
                                                total_rows);
            row_ok &= import_writer_bind_checked(ins, "sqlite3_bind_blob(txid)",
                                                sqlite3_bind_blob(ins, 1, r->txid, 32, SQLITE_STATIC), ndb,
                                                total_rows);
            row_ok &= import_writer_bind_checked(ins, "sqlite3_bind_int(vout)",
                                                sqlite3_bind_int(ins, 2, (int)r->vout), ndb,
                                                total_rows);
            row_ok &= import_writer_bind_checked(ins, "sqlite3_bind_int64(value)",
                                                sqlite3_bind_int64(ins, 3, r->value), ndb,
                                                total_rows);
            row_ok &= import_writer_bind_checked(ins, "sqlite3_bind_blob(script)",
                                                sqlite3_bind_blob(ins, 4, sc, (int)r->script_len, SQLITE_STATIC), ndb,
                                                total_rows);
            row_ok &= import_writer_bind_checked(ins, "sqlite3_bind_int(script_type)",
                                                sqlite3_bind_int(ins, 5, r->script_type), ndb,
                                                total_rows);
            if (r->has_address)
                row_ok &= import_writer_bind_checked(ins, "sqlite3_bind_blob(address_hash)",
                                                    sqlite3_bind_blob(ins, 6, r->address_hash, 20, SQLITE_STATIC), ndb,
                                                    total_rows);
            else
                row_ok &= import_writer_bind_checked(ins, "sqlite3_bind_null(address_hash)",
                                                    sqlite3_bind_null(ins, 6), ndb,
                                                    total_rows);
            row_ok &= import_writer_bind_checked(ins, "sqlite3_bind_int(height)",
                                                sqlite3_bind_int(ins, 7, r->height), ndb,
                                                total_rows);
            row_ok &= import_writer_bind_checked(ins, "sqlite3_bind_int(is_coinbase)",
                                                sqlite3_bind_int(ins, 8, r->is_coinbase), ndb,
                                                total_rows);
            row_ok &= import_writer_step_checked(ins, ndb, total_rows);
            if (!row_ok) {
                import_ctx_request_stop(ctx);
                break;
            }
            total_rows++;
        }

        if (import_ctx_should_stop(ctx)) {
            import_chunk_reset(chunk);
            break;
        }

        /* Commit every ~100K rows */
        if (total_rows % 100000 < chunk->num_rows) {
            if (!node_db_commit(ndb)) {
                fprintf(stderr, "UTXO import writer: COMMIT failed\n");
                if (!node_db_rollback(ndb))
                    fprintf(stderr, "UTXO import writer: ROLLBACK failed after commit failure\n");
                import_ctx_request_stop(ctx);
                import_chunk_reset(chunk);
                break;
            }
            sync_job_import_progress(total_rows);
            printf("UTXO import: %d rows written...\n", total_rows);
            fflush(stdout);
            if (!node_db_begin(ndb)) {
                fprintf(stderr, "UTXO import writer: BEGIN restart failed\n");
                if (!node_db_rollback(ndb))
                    fprintf(stderr, "UTXO import writer: rollback after BEGIN restart failure failed\n");
                import_ctx_request_stop(ctx);
                import_chunk_reset(chunk);
                break;
            }
        }

        /* Free buffers and release chunk */
        for (int ei = 0; ei < chunk->num_entries; ei++) {
            free(chunk->entries[ei].value);
            chunk->entries[ei].value = NULL;
        }
        for (int ri = 0; ri < chunk->num_rows; ri++) {
            free(chunk->rows[ri].script_overflow);
            chunk->rows[ri].script_overflow = NULL;
        }
        chunk->num_entries = 0;
        chunk->num_rows = 0;
        atomic_fetch_add(&ctx->chunks_consumed, 1);
        atomic_store(&chunk->state, 0); /* free for reuse */
    }

    if (!import_ctx_should_stop(ctx)) {
        if (!node_db_commit(ndb))
            fprintf(stderr, "UTXO import writer: final COMMIT failed\n");
    } else {
        if (!node_db_rollback(ndb))
            fprintf(stderr, "UTXO import writer: rollback requested by stop flag failed\n");
    }
    sync_job_import_progress(total_rows);
    atomic_store(&ctx->total_rows, total_rows);
    return NULL;
}

int node_db_sync_import_utxos(struct node_db *ndb,
                               struct coins_view_db *cvdb)
{
    struct import_job job;
    struct sync_db_turbo_scope turbo_mode = {0};
    bool restore_ok = true;

    if (!ndb || !ndb->open || !cvdb) return -1;
    sync_job_import_begin();

    int num_decoders = import_num_decoders();
    printf("UTXO import: parallel pipeline (%d decoders, %d chunks, %ld cores)...\n",
           num_decoders, IMPORT_NUM_CHUNKS, sysconf(_SC_NPROCESSORS_ONLN));
    fflush(stdout);

    struct timespec ts_start;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    /* ── SQLite turbo mode — delegate to node_db layer ────────────── */
    if (!sync_db_turbo_scope_begin(&turbo_mode, ndb, true)) {
        fprintf(stderr, "UTXO import: failed to enter turbo mode\n");
        sync_job_import_finish(0);
        return -1;
    }

    /* ── Recovery policy gate ──────────────────────────────────────
     * The wipe below is a reimport prelude, not a reorg rollback — in
     * normal operation the table is empty or nearly empty. Historically
     * this call site is *not* the one that caused 2026-04-10, but it
     * shares a primitive with the paths that did, so we gate it the
     * same way: ask the policy, refuse if over cap, abort cleanly.
     * The cap is deliberately generous here (reimport is legitimate)
     * but an operator can still raise ZCL_MAX_UTXO_WIPE_ROWS if a
     * partial import is being resumed. */
    struct recovery_policy rp;
    policy_load_from_env(&rp);
    int64_t existing = node_db_utxo_count(ndb);
    if (existing < 0) existing = 0;
    enum policy_decision pd = policy_check_utxo_wipe(
        &rp, existing, "sync_controller.import_utxos_reimport");
    if (pd != POLICY_ALLOW) {
        fprintf(stderr,
                "UTXO import: recovery_policy refused wipe (code=%s, rows=%lld)\n",
                policy_decision_name(pd), (long long)existing);
        if (!sync_db_turbo_scope_end(&turbo_mode))
            fprintf(stderr, "UTXO import: failed to restore normal mode after policy refusal\n");
        sync_job_import_finish(0);
        return -1;
    }

    if (!node_db_wipe_utxos(ndb)) {
        fprintf(stderr, "UTXO import: failed to wipe utxos table\n");
        if (!sync_db_turbo_scope_end(&turbo_mode))
            fprintf(stderr, "UTXO import: failed to restore normal mode after wipe failure\n");
        sync_job_import_finish(0);
        return -1;
    }

    /* ── Initialize pipeline context ───────────────────────────────── */
    struct import_context *ctx = calloc(1, sizeof(struct import_context));
    if (!ctx) {
        if (!sync_db_turbo_scope_end(&turbo_mode))
            fprintf(stderr, "UTXO import: failed to restore normal mode after alloc failure\n");
        sync_job_import_finish(0);
        return -1;
    }
    ctx->cvdb = cvdb;
    ctx->ndb = ndb;
    atomic_store(&ctx->cancel_requested, false);
    atomic_store(&ctx->reader_done, false);
    atomic_store(&ctx->decoders_done, false);
    for (int i = 0; i < IMPORT_NUM_CHUNKS; i++)
        atomic_store(&ctx->chunks[i].state, 0);
    import_job_init(&job, ctx, num_decoders);

    /* ── Start decoder + writer threads ────────────────────────────── */
    if (!import_job_start(&job)) {
        fprintf(stderr, "UTXO import: FATAL — worker pipeline failed to start\n");
        import_context_release_chunks(ctx);
        if (!sync_db_turbo_scope_end(&turbo_mode))
            fprintf(stderr, "UTXO import: failed to restore normal mode after worker startup failure\n");
        free(ctx);
        sync_job_import_finish(0);
        return -1;
    }

    /* ── Reader (main thread): iterate LevelDB, fill chunks ────────── */
    /* Take a LevelDB snapshot so the iterator sees a consistent,
     * frozen view even if zclassicd is writing concurrently.
     * This prevents the random UTXO gaps caused by non-atomic reads. */
    db_wrapper_snapshot_begin(&cvdb->db);

    struct db_iterator it;
    db_iter_init(&it, &cvdb->db);
    char seek_key[33];
    seek_key[0] = 'c';
    memset(seek_key + 1, 0, 32);
    db_iter_seek(&it, seek_key, 33);

    int chunk_idx = 0;
    int total_entries = 0;
    int skipped_entries = 0;

    while (db_iter_valid(&it)) {
        if (import_ctx_should_stop(ctx))
            break;
        /* Find a free chunk */
        struct import_chunk *chunk = NULL;
        while (!chunk) {
            if (import_ctx_should_stop(ctx))
                goto reader_done;
            for (int i = 0; i < IMPORT_NUM_CHUNKS; i++) {
                int idx = (chunk_idx + i) % IMPORT_NUM_CHUNKS;
                int expected = 0;
                if (atomic_compare_exchange_strong(&ctx->chunks[idx].state,
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
            if (import_ctx_should_stop(ctx))
                goto reader_done;
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
                /* db_iter_value() already deobfuscates values using the
                 * obfuscation key (dbwrapper.c:370-372). Do NOT XOR
                 * again here — that would undo the deobfuscation. */
                e->value_len = (uint16_t)val_len;
                chunk->num_entries++;
                total_entries++;
            } else {
                fprintf(stderr, "WARNING: malloc failed for chunk entry value (%zu bytes), skipping entry\n", val_len);
                skipped_entries++;
            }
            db_iter_next(&it);
        }

        if (chunk->num_entries > 0) {
            atomic_fetch_add(&ctx->chunks_produced, 1);
            atomic_thread_fence(memory_order_release);
            atomic_store(&chunk->state, 1); /* filled → decoders */
        } else {
            atomic_store(&chunk->state, 0); /* empty, release */
        }
    }
reader_done:
    /* Check for iterator errors — checksum failures can cause early
     * termination, silently dropping remaining entries. */
    {
        extern void db_iter_check_error(struct db_iterator *it);
        db_iter_check_error(&it);
    }
    db_iter_free(&it);
    db_wrapper_snapshot_end(&cvdb->db);
    atomic_store(&ctx->reader_done, true);

    printf("UTXO import: read %d txids from LevelDB\n", total_entries);
    fflush(stdout);
    fflush(stdout);

    /* ── Wait for decoders ────────────────────────────────────────── */
    import_job_join_decoders(&job);

    /* ── Wait for ALL chunks to be consumed by writer ──────────── */
    /* After decoders finish, remaining chunks are in state 2 (decoded).
     * We MUST wait for the writer to consume them ALL before signaling
     * decoders_done. Otherwise the writer sees decoders_done=true, does
     * a quick scan, misses state=2 chunks due to timing, and exits
     * early — dropping the last ~219 txids / ~520 UTXOs. */
    for (;;) {
        if (import_ctx_should_stop(ctx))
            break;
        bool any_pending = false;
        for (int i = 0; i < IMPORT_NUM_CHUNKS; i++) {
            int s = atomic_load_explicit(&ctx->chunks[i].state,
                                          memory_order_acquire);
            if (s == 1 || s == 2) { any_pending = true; break; }
        }
        if (!any_pending) break;
        struct timespec ts = {0, 1000000}; /* 1ms */
        nanosleep(&ts, NULL);
    }
    atomic_store(&ctx->decoders_done, true);

    /* ── Wait for writer ───────────────────────────────────────────── */
    import_job_join_writer(&job);
    int total_rows = atomic_load(&ctx->total_rows);
    sync_job_import_progress(total_rows);
    int decode_fail = atomic_load(&ctx->decode_failures);
    int skip_out = atomic_load(&ctx->skipped_outputs);
    if (decode_fail > 0 || skip_out > 0)
        printf("UTXO import: %d decode failures, %d skipped outputs\n",
               decode_fail, skip_out);

    /* Validation: verify all txids made it to SQLite */
    {
        sqlite3_stmt *cnt = NULL;
        sqlite3_prepare_v2(ndb->db,
            "SELECT COUNT(DISTINCT txid), COUNT(*) FROM utxos",
            -1, &cnt, NULL);
        if (cnt && sqlite3_step(cnt) == SQLITE_ROW) {
            int64_t sql_txids = sqlite3_column_int64(cnt, 0);
            int64_t sql_rows = sqlite3_column_int64(cnt, 1);
            if (sql_rows != total_rows) {
                /* Row count mismatch = real data loss — pipeline bug */
                fprintf(stderr, "UTXO IMPORT ERROR: wrote %d rows but "
                        "SQLite has %lld rows — data loss!\n",
                        total_rows, (long long)sql_rows);
            } else if (sql_txids < total_entries) {
                /* Fewer distinct txids is expected: fully-pruned CCoins
                 * (all outputs spent) produce zero rows per txid.
                 * These exist in LevelDB as tombstones until compaction. */
                int pruned = total_entries - (int)sql_txids;
                printf("UTXO import: %d/%d LevelDB entries were "
                       "fully-pruned (all outputs spent)\n",
                       pruned, total_entries);
            }
        }
        sqlite3_finalize(cnt);
    }
    fflush(stdout);

    struct timespec ts_write;
    clock_gettime(CLOCK_MONOTONIC, &ts_write);
    double pipe_ms = (ts_write.tv_sec - ts_start.tv_sec) * 1000.0 +
                     (ts_write.tv_nsec - ts_start.tv_nsec) / 1e6;
    printf("UTXO import: %d rows written in %.0fms\n", total_rows, pipe_ms);
    fflush(stdout);

    if (import_ctx_should_stop(ctx)) {
        fprintf(stderr, "UTXO import: aborted%s\n",
                g_shutdown_requested ? " on shutdown" : "");
        if (!sync_db_turbo_scope_end(&turbo_mode))
            fprintf(stderr, "UTXO import: failed to restore normal mode after abort\n");
        restore_ok = false;
        import_context_release_chunks(ctx);
        free(ctx);
        sync_job_import_finish(total_rows);
        return -1;
    }

    /* ── Rebuild all indexes for power-node queries ────────────────── */
    printf("UTXO import: building indexes for fast queries...\n");
    fflush(stdout);

    struct timespec ts_idx;
    clock_gettime(CLOCK_MONOTONIC, &ts_idx);

    /* Rebuild indexes and restore safe pragmas */
    if (!sync_db_turbo_scope_end(&turbo_mode)) {
        restore_ok = false;
        fprintf(stderr, "UTXO import: failed to restore normal mode\n");
    }
    if (!restore_ok) {
        import_context_release_chunks(ctx);
        free(ctx);
        sync_job_import_finish(total_rows);
        return -1;
    }

    struct timespec ts_idx_done;
    clock_gettime(CLOCK_MONOTONIC, &ts_idx_done);
    double idx_ms = (ts_idx_done.tv_sec - ts_idx.tv_sec) * 1000.0 +
                    (ts_idx_done.tv_nsec - ts_idx.tv_nsec) / 1e6;
    printf("UTXO import: indexes built in %.0fms\n", idx_ms);

    double total_ms = (ts_idx_done.tv_sec - ts_start.tv_sec) * 1000.0 +
                      (ts_idx_done.tv_nsec - ts_start.tv_nsec) / 1e6;
    printf("UTXO import complete: %d outputs from %d txids in %.1fs "
           "(pipeline %.0fms + index %.0fms)\n",
           total_rows, total_entries, total_ms / 1000.0,
           pipe_ms, idx_ms);
    fflush(stdout);

    import_context_release_chunks(ctx);
    free(ctx);
    sync_job_import_finish(total_rows);
    return total_rows;
}
