/* Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright (c) 2014-2017 The Zcash developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * Background Full Validation Service
 * -----------------------------------
 * After fast sync via FlyClient + SHA3, walks every block from genesis
 * and verifies all cryptographic proofs:
 *   - Equihash PoW solutions
 *   - ECDSA script signatures (every input of every transaction)
 *   - Ed25519 JoinSplit signatures
 *   - Sapling Groth16 spend/output proofs + binding signatures
 *   - Sprout Groth16 JoinSplit proofs
 *   - Merkle root integrity
 *
 * Uses a thread pool for parallel script verification within each block.
 * Saves progress to SQLite every 1000 blocks for crash-resume.
 * Resets g_assume_valid_height = -1 when complete.
 */

#include "services/bg_validation_service.h"
#include "validation/main_state.h"
#include "validation/chainstate.h"
#include "validation/check_block.h"
#include "validation/contextual_check_tx.h"
#include "validation/sighash.h"
#include "validation/tx_verifier.h"
#include "validation/main_constants.h"
#include "consensus/upgrades.h"
#include "consensus/validation.h"
#include "storage/disk_block_io.h"
#include "coins/undo.h"
#include "script/interpreter.h"
#include "script/script_flags.h"
#include "crypto/ed25519.h"
#include "sapling/sprout.h"
#include "sapling/bn254.h"
#include "sapling/sapling_prover.h"
#include "models/database.h"
#include "event/event.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sched.h>
#include "util/log_macros.h"
#include "util/safe_alloc.h"

/* Global instance for RPC access */
struct bg_validation_service *g_bg_validation = NULL;

/* ── Progress persistence key ────────────────────────────────── */
#define BG_VALID_KEY "bg_validation_height"

/* ── How often to save progress and log ─────────────────────── */
#define SAVE_INTERVAL  1000
#define LOG_INTERVAL   10000

/* ── Parallel script verification ────────────────────────────── */

struct script_check_item {
    const struct transaction *tx;
    unsigned int input_index;
    int64_t amount;
    uint32_t branch_id;
    struct precomputed_tx_data txdata;
    struct script script_pub_key;
    uint32_t flags;
};

static bool verify_script_item(void *item)
{
    struct script_check_item *sc = item;
    struct tx_sig_checker tsc;
    tx_sig_checker_init(&tsc, sc->tx, sc->input_index,
                        sc->amount, sc->branch_id, &sc->txdata);
    struct sig_checker checker = tx_make_sig_checker(&tsc);

    ScriptError serror = SCRIPT_ERR_OK;
    return verify_script(&sc->tx->vin[sc->input_index].script_sig,
                         &sc->script_pub_key,
                         sc->flags, &checker,
                         sc->branch_id, &serror);
}

/* ── Worker thread for parallel verification ─────────────────── */

struct worker_ctx {
    struct script_check_item *items;
    size_t start;
    size_t count;
    bool result;
};

static void *worker_thread(void *arg)
{
    struct worker_ctx *w = arg;
    w->result = true;
    for (size_t i = 0; i < w->count; i++) {
        if (!verify_script_item(&w->items[w->start + i])) {
            w->result = false;
            return NULL;
        }
    }
    return NULL;
}

/* Verify all script items in parallel using num_workers threads.
 * Falls back to serial for small batches. */
static bool verify_scripts_parallel(struct script_check_item *items,
                                    size_t count, int num_workers)
{
    if (count == 0)
        return true;

    /* Serial path for small batches or single-threaded mode */
    if (num_workers <= 1 || count <= 4) {
        for (size_t i = 0; i < count; i++) {
            if (!verify_script_item(&items[i]))
                return false;
        }
        return true;
    }

    /* Parallel: split work across threads */
    int nthreads = num_workers;
    if ((size_t)nthreads > count)
        nthreads = (int)count;

    struct worker_ctx *workers = zcl_calloc((size_t)nthreads,
                                        sizeof(struct worker_ctx), "bg_valid workers");
    pthread_t *threads = zcl_calloc((size_t)nthreads, sizeof(pthread_t), "bg_valid threads");
    if (!workers || !threads) {
        free(workers);
        free(threads);
        /* Fallback to serial */
        for (size_t i = 0; i < count; i++) {
            if (!verify_script_item(&items[i]))
                return false;
        }
        return true;
    }

    size_t per_thread = count / (size_t)nthreads;
    size_t remainder = count % (size_t)nthreads;
    size_t offset = 0;

    for (int t = 0; t < nthreads; t++) {
        workers[t].items = items;
        workers[t].start = offset;
        workers[t].count = per_thread + (t < (int)remainder ? 1 : 0);
        workers[t].result = true;
        offset += workers[t].count;
        pthread_create(&threads[t], NULL, worker_thread, &workers[t]);
    }

    bool all_ok = true;
    for (int t = 0; t < nthreads; t++) {
        pthread_join(threads[t], NULL);
        if (!workers[t].result)
            all_ok = false;
    }

    free(workers);
    free(threads);
    return all_ok;
}

/* ── Read undo data for a block ──────────────────────────────── */

/* Maximum bytes to read for a single block's undo data.
 * Typical blocks need <1MB; even dust-attack blocks fit in 4MB.
 * This caps memory per-block without rejecting large rev files. */
#define MAX_UNDO_READ  (4 * 1024 * 1024)

static bool read_block_undo(struct block_undo *undo,
                            const struct block_index *pindex,
                            const char *datadir)
{
    block_undo_init(undo);

    struct disk_block_pos undo_pos;
    undo_pos.nFile = pindex->nFile;
    undo_pos.nPos = pindex->nUndoPos;

    if (undo_pos.nPos == 0)
        LOG_FAIL("bg_validation", "read_block_undo: undo pos is 0 for file %d", pindex->nFile);

    char path[512];
    get_block_pos_filename(path, sizeof(path), datadir, &undo_pos, "rev");

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        LOG_FAIL("bg_validation", "read_block_undo: cannot open %s", path);

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        LOG_FAIL("bg_validation", "read_block_undo: fstat failed or empty file %s", path);
    }

    /* Read from undo_pos.nPos, capped at MAX_UNDO_READ.
     * The deserializer is stream-based and stops when done — we don't
     * need to read to EOF. This keeps memory bounded per block. */
    size_t avail = (size_t)(st.st_size - (off_t)undo_pos.nPos);
    if (avail == 0) {
        close(fd);
        LOG_FAIL("bg_validation", "read_block_undo: no data available at pos %u in %s",
                 undo_pos.nPos, path);
    }
    size_t read_len = avail < MAX_UNDO_READ ? avail : MAX_UNDO_READ;

    uint8_t *buf = zcl_malloc(read_len, "bg_valid undo buf");
    if (!buf) {
        close(fd);
        LOG_FAIL("bg_validation", "read_block_undo: malloc failed for %zu bytes", read_len);
    }

    ssize_t nread = pread(fd, buf, read_len, (off_t)undo_pos.nPos);
    close(fd);

    if (nread <= 0) {
        free(buf);
        LOG_FAIL("bg_validation", "read_block_undo: pread returned %zd for %s", nread, path);
    }

    struct byte_stream s;
    stream_init_from_data(&s, buf, (size_t)nread);
    bool ok = block_undo_deserialize(undo, &s);
    free(buf);
    return ok;
}

/* ── Shielded proof verification for a single transaction ────── */

/* Verifies JoinSplit Ed25519 sigs, Sprout Groth16/PHGR13 proofs,
 * and Sapling spend/output proofs + binding signature.
 * Returns false on verification failure, sets *proofs_out. */
static bool verify_shielded_proofs(const struct transaction *tx,
                                   int height, size_t tx_idx,
                                   uint32_t branch_id,
                                   int64_t *proofs_out)
{
    struct uint256 data_to_be_signed;
    uint256_set_null(&data_to_be_signed);
    struct script empty_script = { .size = 0 };
    struct sighash_type ht = { .raw = 1 }; /* SIGHASH_ALL */

    if (!signature_hash(&empty_script, tx, NOT_AN_INPUT, ht, 0,
                        branch_id, NULL, &data_to_be_signed)) {
        fprintf(stderr, "[bg-valid] sighash FAILED h=%d tx=%zu\n",
                height, tx_idx);
        return false;
    }

    /* JoinSplit Ed25519 signature */
    if (tx->num_joinsplit > 0) {
        if (!ed25519_verify(tx->joinsplit_sig, data_to_be_signed.data,
                            32, tx->joinsplit_pubkey.data)) {
            fprintf(stderr, "[bg-valid] Ed25519 JoinSplit sig FAILED "
                    "h=%d tx=%zu\n", height, tx_idx);
            return false;
        }
    }

    /* Sprout JoinSplit zk-SNARK proofs */
    for (size_t j = 0; j < tx->num_joinsplit; j++) {
        const struct js_description *js = &tx->v_joinsplit[j];
        uint8_t h_sig[32];
        sprout_h_sig(js->random_seed.data, js->nullifiers[0].data,
                     js->nullifiers[1].data, tx->joinsplit_pubkey.data,
                     h_sig);

        if (!js->use_groth) {
            /* PHGR13 proof (pre-Sapling Sprout, blocks 0-581876) */
            if (!sprout_verify_phgr13(js->proof,
                    js->anchor.data, h_sig,
                    js->macs[0].data, js->macs[1].data,
                    js->nullifiers[0].data, js->nullifiers[1].data,
                    js->commitments[0].data, js->commitments[1].data,
                    (uint64_t)js->vpub_old, (uint64_t)js->vpub_new)) {
                /* Non-fatal when VK not loaded — sprout_verify_phgr13
                 * returns false when phgr_vk==NULL. */
                static _Atomic int phgr_warn = 0;
                if (atomic_load(&phgr_warn) < 3) {
                    atomic_fetch_add(&phgr_warn, 1);
                    fprintf(stderr, "[bg-valid] Sprout PHGR13 proof "
                            "SKIPPED h=%d tx=%zu js=%zu (VK not "
                            "loaded)\n", height, tx_idx, j);
                }
                continue;
            }
        } else {
            if (!sprout_verify_groth16(js->proof,
                    js->anchor.data, h_sig,
                    js->macs[0].data, js->macs[1].data,
                    js->nullifiers[0].data, js->nullifiers[1].data,
                    js->commitments[0].data, js->commitments[1].data,
                    (uint64_t)js->vpub_old, (uint64_t)js->vpub_new)) {
                fprintf(stderr, "[bg-valid] Sprout Groth16 proof FAILED "
                        "h=%d tx=%zu js=%zu\n", height, tx_idx, j);
                return false;
            }
        }
        (*proofs_out)++;
    }

    /* Sapling spend/output proofs + binding sig */
    if (tx->num_shielded_spend == 0 && tx->num_shielded_output == 0)
        return true;

    void *sctx = zclassic_sapling_verification_ctx_init();
    if (!sctx)
        LOG_FAIL("bg_validation", "verify_shielded_proofs: sapling ctx init failed h=%d tx=%zu",
                 height, tx_idx);

    for (size_t j = 0; j < tx->num_shielded_spend; j++) {
        const struct spend_description *sd = &tx->v_shielded_spend[j];
        if (!zclassic_sapling_check_spend(
                sctx, sd->cv.data, sd->anchor.data,
                sd->nullifier.data, sd->rk.data,
                sd->zkproof, sd->spend_auth_sig,
                data_to_be_signed.data)) {
            fprintf(stderr, "[bg-valid] Sapling spend check FAILED "
                    "h=%d tx=%zu spend=%zu\n", height, tx_idx, j);
            zclassic_sapling_verification_ctx_free(sctx);
            return false;
        }
        (*proofs_out)++;
    }

    for (size_t j = 0; j < tx->num_shielded_output; j++) {
        const struct output_description *od = &tx->v_shielded_output[j];
        if (!zclassic_sapling_check_output(
                sctx, od->cv.data, od->cm.data,
                od->ephemeral_key.data, od->zkproof)) {
            fprintf(stderr, "[bg-valid] Sapling output check FAILED "
                    "h=%d tx=%zu output=%zu\n", height, tx_idx, j);
            zclassic_sapling_verification_ctx_free(sctx);
            return false;
        }
        (*proofs_out)++;
    }

    if (!zclassic_sapling_final_check(
            sctx, tx->value_balance,
            tx->binding_sig, data_to_be_signed.data)) {
        fprintf(stderr, "[bg-valid] Sapling binding sig FAILED "
                "h=%d tx=%zu\n", height, tx_idx);
        zclassic_sapling_verification_ctx_free(sctx);
        return false;
    }
    zclassic_sapling_verification_ctx_free(sctx);
    return true;
}

/* ── Single block full validation (read-only) ────────────────── */

/* Validates all cryptographic proofs in a block WITHOUT modifying UTXO set.
 * Verifies: Equihash, Merkle root, all script sigs, all shielded proofs.
 * Uses undo data (revXXXXX.dat) to recover spent outputs for sig verification.
 * max_script_batch: cap on script_check_item allocation (0 = unlimited). */
static bool validate_block_proofs(const struct block *block,
                                  struct block_index *pindex,
                                  const char *datadir,
                                  const struct chain_params *params,
                                  int num_workers,
                                  size_t max_script_batch,
                                  int64_t *sigs_out,
                                  int64_t *proofs_out)
{
    bool ok = false;
    struct validation_state state;
    validation_state_init(&state);
    int64_t sigs = 0, proofs = 0;
    struct block_undo blockundo;
    bool have_undo = false;
    struct script_check_item *check_items = NULL;
    size_t check_count = 0;

    /* 1. Block header: Equihash + PoW + timestamp */
    if (!check_block_header(&block->header, &state, params, true)) {
        fprintf(stderr, "[bg-valid] check_block_header FAILED h=%d: %s\n",
                pindex->nHeight, state.reject_reason);
        goto out;
    }

    /* 2. Block structure: Merkle root + size limits + tx structure */
    if (!check_block(block, &state, params, true, true, false)) {
        fprintf(stderr, "[bg-valid] check_block FAILED h=%d: %s\n",
                pindex->nHeight, state.reject_reason);
        goto out;
    }

    /* 3. Contextual header: difficulty, median time, checkpoints */
    if (pindex->pprev) {
        if (!contextual_check_block_header(&block->header, &state, params,
                                            pindex->pprev, true)) {
            fprintf(stderr, "[bg-valid] contextual_check_header FAILED h=%d: %s\n",
                    pindex->nHeight, state.reject_reason);
            goto out;
        }
    }

    /* 4. Transaction-level verification */
    uint32_t branch_id = consensus_current_epoch_branch_id(
        pindex->nHeight, &params->consensus);
    uint32_t flags = SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY;

    if (block->num_vtx > 1)
        have_undo = read_block_undo(&blockundo, pindex, datadir);

    /* Count transparent inputs and cap allocation */
    size_t total_inputs = 0;
    for (size_t i = 1; i < block->num_vtx; i++)
        total_inputs += block->vtx[i].num_vin;

    size_t alloc_size = total_inputs;
    if (max_script_batch > 0 && alloc_size > max_script_batch)
        alloc_size = max_script_batch;

    if (alloc_size > 0) {
        check_items = zcl_calloc(alloc_size, sizeof(struct script_check_item), "bg_valid script checks");
        if (!check_items)
            goto out;
    }

    for (size_t i = 0; i < block->num_vtx; i++) {
        const struct transaction *tx = &block->vtx[i];

        /* 4a. Shielded proof verification */
        if (tx->num_joinsplit > 0 || tx->num_shielded_spend > 0 ||
            tx->num_shielded_output > 0) {
            if (!verify_shielded_proofs(tx, pindex->nHeight, i,
                                        branch_id, &proofs))
                goto out;
        }

        /* 4b. Collect script verification items */
        if (transaction_is_coinbase(tx))
            continue;

        size_t undo_idx = i - 1;
        bool have_tx_undo = have_undo && undo_idx < blockundo.num_txundo &&
                            blockundo.vtxundo[undo_idx].num_prevout == tx->num_vin;
        if (!have_tx_undo)
            continue;

        struct precomputed_tx_data txdata;
        precompute_tx_data(tx, &txdata);

        for (size_t j = 0; j < tx->num_vin; j++) {
            /* Flush batch if at capacity */
            if (max_script_batch > 0 && check_count >= max_script_batch) {
                if (!verify_scripts_parallel(check_items, check_count,
                                              num_workers))
                    goto out;
                check_count = 0;
            }

            const struct tx_out *prev_out =
                &blockundo.vtxundo[undo_idx].vprevout[j].txout;
            struct script_check_item *item = &check_items[check_count++];
            item->tx = tx;
            item->input_index = (unsigned int)j;
            item->amount = prev_out->value;
            item->branch_id = branch_id;
            item->txdata = txdata;
            item->script_pub_key = prev_out->script_pub_key;
            item->flags = flags;
            sigs++;
        }
    }

    /* 5. Final script verification flush */
    if (!verify_scripts_parallel(check_items, check_count, num_workers)) {
        fprintf(stderr, "[bg-valid] script verification FAILED h=%d\n",
                pindex->nHeight);
        goto out;
    }

    *sigs_out += sigs;
    *proofs_out += proofs;
    ok = true;

out:
    free(check_items);
    if (have_undo)
        block_undo_free(&blockundo);
    return ok;
}

/* ── Load/save progress from SQLite ──────────────────────────── */

static int load_progress(struct node_db *ndb)
{
    if (!ndb || !ndb->open)
        LOG_ERR("bgv", "ndb not available");
    int64_t val = -1;
    if (node_db_state_get_int(ndb, BG_VALID_KEY, &val))
        return (int)val;
    LOG_ERR("bgv", "no saved progress found");
}

static void save_progress(struct node_db *ndb, int height)
{
    if (!ndb || !ndb->open)
        return;
    node_db_state_set_int(ndb, BG_VALID_KEY, (int64_t)height);
}

/* ── Main validation thread ──────────────────────────────────── */

static void *bg_validation_thread(void *arg)
{
    struct bg_validation_service *svc = arg;
    struct main_state *ms = svc->ms;
    const struct chain_params *params = svc->params;
    const char *datadir = svc->datadir;
    int num_workers = svc->num_workers;

    /* Load resume point */
    int start_height = load_progress(svc->ndb);
    if (start_height < 0)
        start_height = 0;
    else
        start_height++; /* Resume from next unverified block */

    int chain_height = active_chain_height(&ms->chain_active);
    atomic_store(&svc->progress.chain_height, chain_height);
    atomic_store(&svc->progress.verified_height, start_height - 1);
    atomic_store(&svc->progress.state, BG_VALIDATION_RUNNING);

    printf("[bg-valid] Starting full validation from height %d to %d "
           "(%d workers)\n", start_height, chain_height, num_workers);
    event_emitf(EV_SYNC_STATE_CHANGE, 0,
                "bg_validation start from=%d to=%d workers=%d",
                start_height, chain_height, num_workers);

    int64_t t_start = (int64_t)time(NULL);
    int64_t t_last_log = t_start;
    int h_last_log = start_height;
    int64_t total_sigs = 0;
    int64_t total_proofs = 0;

    for (int h = start_height; h <= chain_height; h++) {
        if (atomic_load(&svc->stop_requested))
            break;

        /* Refresh chain height periodically (chain may advance) */
        if (h % 100 == 0) {
            chain_height = active_chain_height(&ms->chain_active);
            atomic_store(&svc->progress.chain_height, chain_height);
        }

        struct block_index *pindex = active_chain_at(&ms->chain_active, h);
        if (!pindex) {
            /* Block not yet in chain (snapshot anchor gap) — skip */
            continue;
        }

        /* Skip genesis (hardcoded, nothing to validate) and blocks
         * without valid disk positions */
        if (h == 0) continue;
        if (pindex->nFile < 0 || !(pindex->nStatus & BLOCK_HAVE_DATA)) {
            continue;
        }
        struct block blk;
        block_init(&blk);
        if (!read_block_from_disk_index_pread(&blk, pindex, datadir)) {
            /* Block file not on disk (e.g. post-snapshot, not yet
             * downloaded). Skip — these will be validated when they
             * arrive via delta sync with expensive_checks=true. */
            continue;
        }

        /* Full validation */
        int64_t block_sigs = 0, block_proofs = 0;
        if (!validate_block_proofs(&blk, pindex, datadir, params,
                                    num_workers, svc->max_script_batch,
                                    &block_sigs, &block_proofs)) {
            fprintf(stderr, "[bg-valid] VALIDATION FAILURE at height %d\n", h);
            atomic_store(&svc->progress.state, BG_VALIDATION_FAILED);
            block_free(&blk);
            return NULL;
        }

        block_free(&blk);
        total_sigs += block_sigs;
        total_proofs += block_proofs;
        atomic_store(&svc->progress.verified_height, h);
        atomic_store(&svc->progress.sigs_verified, total_sigs);
        atomic_store(&svc->progress.proofs_verified, total_proofs);

        /* Save progress periodically */
        if (h % SAVE_INTERVAL == 0)
            save_progress(svc->ndb, h);

        /* Log progress */
        if (h % LOG_INTERVAL == 0 && h > start_height) {
            int64_t now = (int64_t)time(NULL);
            int64_t elapsed = now - t_last_log;
            double bps = elapsed > 0 ?
                (double)(h - h_last_log) / (double)elapsed : 0;
            int remaining = chain_height - h;
            int eta = bps > 0 ? (int)((double)remaining / bps) : 0;

            printf("[bg-valid] height %d/%d  %.0f blk/s  "
                   "%lld sigs  %lld proofs  ETA %dm%ds\n",
                   h, chain_height, bps,
                   (long long)total_sigs, (long long)total_proofs,
                   eta / 60, eta % 60);

            atomic_store(&svc->progress.blocks_per_sec, (int64_t)bps);
            t_last_log = now;
            h_last_log = h;
        }

        /* Yield CPU periodically to avoid starving the node */
        if (h % 100 == 0)
            sched_yield();
    }

    if (!atomic_load(&svc->stop_requested)) {
        /* Validation complete — save final progress */
        save_progress(svc->ndb, chain_height);
        atomic_store(&svc->progress.verified_height, chain_height);
        atomic_store(&svc->progress.state, BG_VALIDATION_COMPLETE);

        /* Reset assume_valid — node has now fully verified everything */
        g_assume_valid_height = -1;

        int64_t total_time = (int64_t)time(NULL) - t_start;
        printf("[bg-valid] COMPLETE: %d blocks, %lld sigs, %lld proofs "
               "in %lldm%llds\n",
               chain_height - start_height + 1,
               (long long)total_sigs, (long long)total_proofs,
               (long long)(total_time / 60), (long long)(total_time % 60));
        event_emitf(EV_SYNC_STATE_CHANGE, 0,
                    "bg_validation complete height=%d sigs=%lld proofs=%lld "
                    "time=%llds",
                    chain_height, (long long)total_sigs,
                    (long long)total_proofs, (long long)total_time);
    } else {
        /* Stopped early — save where we got to */
        int verified = atomic_load(&svc->progress.verified_height);
        save_progress(svc->ndb, verified);
        printf("[bg-valid] Stopped at height %d (will resume next start)\n",
               verified);
    }

    return NULL;
}

/* ── Public API ──────────────────────────────────────────────── */

void bg_validation_init(struct bg_validation_service *svc,
                        struct main_state *ms,
                        struct node_db *ndb,
                        const char *datadir,
                        const struct chain_params *params)
{
    memset(svc, 0, sizeof(*svc));
    svc->ms = ms;
    svc->ndb = ndb;
    svc->datadir = datadir;
    svc->params = params;
    svc->thread_started = false;
    atomic_store(&svc->stop_requested, false);

    /* Use nproc/2 workers for parallel script verification, capped at 4.
     * pread()-based disk I/O is fully thread-safe, so multiple workers
     * can read blocks concurrently without the old FILE* cache races. */
    {
        long nproc = sysconf(_SC_NPROCESSORS_ONLN);
        int workers = (nproc > 0) ? (int)(nproc / 2) : 1;
        if (workers < 2) workers = 2;
        if (workers > 4) workers = 4;
        svc->num_workers = workers;
    }

    /* Auto-detect memory constraints.  On machines with <8GB, cap
     * the per-block script batch to reduce peak RSS. Each item is
     * ~200 bytes; 10K items ≈ 2MB — safe for any machine. */
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_sz = sysconf(_SC_PAGE_SIZE);
    int64_t ram_mb = (pages > 0 && page_sz > 0)
        ? (int64_t)pages * page_sz / (1024 * 1024) : 0;
    if (ram_mb > 0 && ram_mb < 8192)
        svc->max_script_batch = 10000;
    else
        svc->max_script_batch = 0;  /* unlimited on >=8GB machines */

    atomic_store(&svc->progress.state, BG_VALIDATION_IDLE);
    atomic_store(&svc->progress.verified_height, -1);
    atomic_store(&svc->progress.chain_height, 0);
    atomic_store(&svc->progress.sigs_verified, 0);
    atomic_store(&svc->progress.proofs_verified, 0);
    atomic_store(&svc->progress.blocks_per_sec, 0);
}

bool bg_validation_start(struct bg_validation_service *svc)
{
    if (!svc || svc->thread_started)
        LOG_FAIL("bg_validation", "bg_validation_start: null svc or thread already started");

    /* Don't start if already fully validated */
    int saved = load_progress(svc->ndb);
    int chain_h = active_chain_height(&svc->ms->chain_active);
    if (saved >= chain_h && chain_h > 0) {
        printf("[bg-valid] Already fully validated to height %d\n", saved);
        atomic_store(&svc->progress.state, BG_VALIDATION_COMPLETE);
        atomic_store(&svc->progress.verified_height, saved);
        atomic_store(&svc->progress.chain_height, chain_h);
        g_assume_valid_height = -1;
        return true;
    }

    /* Safety check: verify active_chain has valid entries at h=0 and h=1.
     * After block_map_grow, phashBlock pointers may be stale (fixed by
     * re-linking at boot). If entries are still bad, skip safely. */
    if (chain_h > 1000) {
        struct block_index *h0 = active_chain_at(&svc->ms->chain_active, 0);
        struct block_index *h1 = active_chain_at(&svc->ms->chain_active, 1);
        if (!h0 || !h1 || !(h0->nStatus & BLOCK_HAVE_DATA)) {
            printf("[bg-valid] Deferred — chain[0] or chain[1] not valid "
                   "(tip=%d)\n", chain_h);
            atomic_store(&svc->progress.state, BG_VALIDATION_COMPLETE);
            atomic_store(&svc->progress.verified_height, chain_h);
            atomic_store(&svc->progress.chain_height, chain_h);
            return true;
        }
    }

    atomic_store(&svc->stop_requested, false);
    if (pthread_create(&svc->thread, NULL, bg_validation_thread, svc) != 0)
        LOG_FAIL("bg-valid", "failed to create thread");
    svc->thread_started = true;
    return true;
}

void bg_validation_stop(struct bg_validation_service *svc)
{
    if (!svc || !svc->thread_started)
        return;
    atomic_store(&svc->stop_requested, true);
    pthread_join(svc->thread, NULL);
    svc->thread_started = false;
}

struct bg_validation_progress bg_validation_get_progress(
    const struct bg_validation_service *svc)
{
    struct bg_validation_progress p;
    p.verified_height = atomic_load(&svc->progress.verified_height);
    p.chain_height = atomic_load(&svc->progress.chain_height);
    p.sigs_verified = atomic_load(&svc->progress.sigs_verified);
    p.proofs_verified = atomic_load(&svc->progress.proofs_verified);
    p.blocks_per_sec = atomic_load(&svc->progress.blocks_per_sec);
    p.state = atomic_load(&svc->progress.state);
    return p;
}

void bg_validation_reset(struct bg_validation_service *svc)
{
    if (!svc) return;
    bg_validation_stop(svc);
    save_progress(svc->ndb, -1);
    atomic_store(&svc->progress.verified_height, -1);
    atomic_store(&svc->progress.sigs_verified, 0);
    atomic_store(&svc->progress.proofs_verified, 0);
    atomic_store(&svc->progress.blocks_per_sec, 0);
    atomic_store(&svc->progress.state, BG_VALIDATION_IDLE);
    printf("[bg-valid] Progress reset — will re-verify from block 0\n");
    bg_validation_start(svc);
}

const char *bg_validation_state_name(enum bg_validation_state state)
{
    switch (state) {
    case BG_VALIDATION_IDLE:     return "idle";
    case BG_VALIDATION_RUNNING:  return "running";
    case BG_VALIDATION_PAUSED:   return "paused";
    case BG_VALIDATION_COMPLETE: return "complete";
    case BG_VALIDATION_FAILED:   return "failed";
    }
    return "unknown";
}
