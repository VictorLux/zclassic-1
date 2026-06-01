/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Fast wallet block scanner. Two-pass design:
 *
 * Pass 1 — Raw byte scan (no deserialization):
 *   mmap each block file and scan for P2PKH (76a914) / P2SH (a914)
 *   script patterns. Extract the 20-byte hash, check against hash table.
 *   Record file offsets of blocks that contain wallet matches.
 *   Speed: memory bandwidth limited (~10 GB/s on modern CPUs).
 *
 * Pass 2 — Selective deserialization:
 *   Only deserialize blocks identified in pass 1.
 *   Full tx parsing for correct UTXO create/spend tracking.
 *
 * Result: skips 99.9%+ of block deserialization. */

#include "platform/time_compat.h"
#include "views/format_helpers.h"
#include "controllers/wallet_scan.h"
#include "controllers/sync_controller.h"
#include "models/wallet_tx.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "validation/chainstate.h"
#include "wallet/wallet.h"
#include "wallet/keystore.h"
#include "core/serialize.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include "controllers/scan_util.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

static bool wallet_scan_begin_checked(struct node_db *ndb,
                                      const char *label)
{
    if (!ndb || !ndb->open || !node_db_begin(ndb)) {
        LOG_FAIL("wallet_scan", "wallet_scan: %s failed: %s",
                label, (ndb && ndb->db) ? sqlite3_errmsg(ndb->db)
                                        : "db unavailable");
    }
    return true;
}

static bool wallet_scan_commit_checked(struct node_db *ndb,
                                       const char *label)
{
    if (!ndb || !ndb->open || !node_db_commit(ndb)) {
        LOG_FAIL("wallet_scan", "wallet_scan: %s failed: %s",
                label, (ndb && ndb->db) ? sqlite3_errmsg(ndb->db)
                                        : "db unavailable");
    }
    return true;
}

static void wallet_scan_rollback_best_effort(struct node_db *ndb,
                                             const char *label)
{
    if (!ndb || !ndb->open)
        return;
    if (!node_db_rollback(ndb)) {
        LOG_WARN("wallet_scan", "wallet_scan: %s failed: %s", label, ndb->db ? sqlite3_errmsg(ndb->db) : "db unavailable");
    }
}

static bool wallet_scan_exec_checked(struct node_db *ndb,
                                     const char *sql,
                                     const char *label)
{
    if (!ndb || !ndb->open || !sql)
        LOG_FAIL("wallet_scan", "exec_checked: invalid args (ndb=%p sql=%p)", (void *)ndb, (void *)sql);
    if (!node_db_exec(ndb, sql))
        LOG_FAIL("wallet_scan", "wallet_scan: %s failed", label);
    return true;
}

static uint8_t *ser_tx(const struct transaction *tx, size_t *len)
{
    struct byte_stream s;
    stream_init(&s, 512);
    transaction_serialize(tx, &s);
    *len = s.size;
    return s.data;
}

/* --- Pass 1: Raw byte pattern scan --- */

/* Scan context for parallel file scanning */
struct file_scan_result {
    bool has_match;
};

/* Scan a single mmap'd block file for P2PKH/P2SH patterns
 * matching our address hash table. */
static bool scan_file_raw(const uint8_t *data, size_t size,
                           const struct addr_ht *ht)
{
    /* Scan for P2PKH: 76 a9 14 [20 bytes] 88 ac (25 bytes total)
     * Scan for P2SH:  a9 14 [20 bytes] 87      (23 bytes total) */
    for (size_t i = 0; i + 25 <= size; i++) {
        /* P2PKH check */
        if (data[i] == 0x76 && data[i + 1] == 0xa9 &&
            data[i + 2] == 0x14 &&
            data[i + 23] == 0x88 && data[i + 24] == 0xac) {
            if (aht_has(ht, data + i + 3))
                return true;
        }
        /* P2SH check */
        if (data[i] == 0xa9 && data[i + 1] == 0x14 &&
            i + 23 <= size && data[i + 22] == 0x87) {
            if (aht_has(ht, data + i + 2))
                return true;
        }
    }
    return false;
}

/* Parallel file scanner thread argument */
struct scan_thread_arg {
    const char *datadir;
    int file_num;
    const struct addr_ht *ht;
    bool result;
};

static void *scan_file_thread(void *arg)
{
    struct scan_thread_arg *a = (struct scan_thread_arg *)arg;
    char path[512];
    snprintf(path, sizeof(path), "%s/blocks/blk%05d.dat",
             a->datadir, a->file_num);
    int fd = open(path, O_RDONLY);
    if (fd < 0) { a->result = false; LOG_NULL("wallet_scan", "open failed for blk%05d.dat", a->file_num); }
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); a->result = false; LOG_NULL("wallet_scan", "fstat failed for blk%05d.dat", a->file_num); }
    size_t sz = (size_t)st.st_size;
    uint8_t *data = mmap(NULL, sz, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (data == MAP_FAILED) { a->result = false; LOG_NULL("wallet_scan", "mmap failed for blk%05d.dat (size=%zu)", a->file_num, sz); }
    posix_madvise(data, sz, POSIX_MADV_SEQUENTIAL);

    a->result = scan_file_raw(data, sz, a->ht);

    munmap(data, sz);
    return NULL;
}

/* --- Pass 2: Selective deserialization --- */

/* Process a single block: check outputs for ownership, inputs for spends */
static bool scan_block_txs(const struct block *blk, int height,
                            const struct addr_ht *ht,
                            struct utxo_set *uset,
                            struct wtx_list *wl)
{
    bool any_found = false;

    for (size_t ti = 0; ti < blk->num_vtx; ti++) {
        struct transaction *tx = &blk->vtx[ti];
        bool is_ours = false, from_me = false;
        int64_t debit = 0;

        for (size_t vi = 0; vi < tx->num_vout; vi++) {
            uint8_t ah[20];
            if (!extract_addr(tx->vout[vi].script_pub_key.data,
                              tx->vout[vi].script_pub_key.size, ah))
                continue;
            if (!aht_has(ht, ah)) continue;

            is_ours = true;
            struct mem_utxo u;
            memset(&u, 0, sizeof(u));
            memcpy(u.txid, tx->hash.data, 32);
            u.vout = (uint32_t)vi;
            u.value = tx->vout[vi].value;
            memcpy(u.addr_hash, ah, 20);
            size_t sl = tx->vout[vi].script_pub_key.size;
            if (sl > sizeof(u.script)) sl = sizeof(u.script);
            memcpy(u.script, tx->vout[vi].script_pub_key.data, sl);
            u.script_len = (uint8_t)sl;
            u.height = height;
            u.is_coinbase = (ti == 0);
            uset_add(uset, &u);
        }

        if (ti > 0) {
            for (size_t vi = 0; vi < tx->num_vin; vi++) {
                int ui = uset_find(uset,
                    tx->vin[vi].prevout.hash.data,
                    tx->vin[vi].prevout.n);
                if (ui >= 0) {
                    is_ours = true;
                    from_me = true;
                    debit += uset->items[ui].value;
                    uset->items[ui].spent = true;
                    memcpy(uset->items[ui].spent_txid,
                           tx->hash.data, 32);
                    uset->items[ui].spent_vin = (int)vi;
                }
            }
        }

        if (is_ours) {
            any_found = true;
            struct mem_wtx wt;
            memset(&wt, 0, sizeof(wt));
            memcpy(wt.txid, tx->hash.data, 32);
            wt.raw = ser_tx(tx, &wt.raw_len);
            wt.height = height;
            wt.time = blk->header.nTime;
            wt.from_me = from_me;
            if (from_me) {
                int64_t vout_total = transaction_get_value_out(tx);
                wt.fee = debit > vout_total ? debit - vout_total : 0;
            }
            wl_add(wl, &wt);
        }
    }
    return any_found;
}

/* --- Main entry point --- */

int wallet_scan_blocks(struct node_db *ndb,
                       const struct active_chain *chain,
                       const struct wallet *w,
                       const char *datadir,
                       int start_height,
                       int end_height)
{
    if (!ndb || !ndb->open || !chain || !w || !datadir)
        LOG_ERR("wallet_scan", "scan_blocks: invalid args (ndb=%p chain=%p w=%p datadir=%p)",
                (void *)ndb, (void *)chain, (void *)w, (void *)datadir);

    /* range fast-path. Empty range = nothing to do. */
    if (start_height > end_height) {
        printf("wallet_scan: range empty (start=%d > end=%d), "
               "skipping\n", start_height, end_height);
        return 0;
    }

    struct timespec ts_start, ts_p1, ts_p2;
    platform_time_monotonic_timespec(&ts_start);

    /* Build address hash table */
    struct addr_ht aht;
    aht_init(&aht);
    for (size_t i = 0; i < w->keystore.num_keys; i++)
        if (w->keystore.keys[i].used)
            aht_insert(&aht, w->keystore.keys[i].keyid.id.data);
    for (size_t i = 0; i < w->keystore.num_scripts; i++)
        if (w->keystore.scripts[i].used)
            aht_insert(&aht, w->keystore.scripts[i].script_id.data);

    printf("wallet_scan: %d address hashes loaded\n", aht.count);
    fflush(stdout);

    /* zero-keys fast-path. Without any keys to
     * match, the parallel raw scan would read every block file
     * looking for hashes that aren't in the set — minutes of
     * pointless disk I/O. */
    if (aht.count == 0) {
        printf("wallet_scan: no wallet keys, skipping block scan\n");
        return 0;
    }

    /* Determine which block files exist */
    int num_files = 0;
    for (int f = 0; f < 200; f++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/blocks/blk%05d.dat", datadir, f);
        if (access(path, R_OK) != 0) break;
        num_files = f + 1;
    }
    printf("wallet_scan: %d block files to scan\n", num_files);
    fflush(stdout);

    /* ========== PASS 1: Parallel raw byte scan ========== */
    printf("wallet_scan: pass 1 — parallel raw byte scan...\n");
    fflush(stdout);

    /* Launch threads — up to 8 at a time */
    bool *file_has_match = zcl_calloc((size_t)num_files, sizeof(bool), "wallet scan file match");
    int batch = 8;

    for (int base = 0; base < num_files; base += batch) {
        int n = num_files - base;
        if (n > batch) n = batch;

        struct scan_thread_arg args[8];
        pthread_t threads[8];
        int started = 0;

        for (int i = 0; i < n; i++) {
            args[i].datadir = datadir;
            args[i].file_num = base + i;
            args[i].ht = &aht;
            args[i].result = false;
            /* raw-pthread-ok: short-burst-joined-immediately */
            if (pthread_create(&threads[i], NULL,
                               scan_file_thread, &args[i]) != 0) {
                LOG_WARN("wallet_scan", "wallet_scan: failed to start pass-1 scan thread");
                for (int j = 0; j < started; j++)
                    pthread_join(threads[j], NULL);
                aht_free(&aht);
                free(file_has_match);
                return -1; // raw-return-ok:logged-above
            }
            started++;
        }
        for (int i = 0; i < n; i++) {
            pthread_join(threads[i], NULL);
            file_has_match[base + i] = args[i].result;
        }
    }

    platform_time_monotonic_timespec(&ts_p1);
    double p1_ms = (double)(ts_p1.tv_sec - ts_start.tv_sec) * 1000.0 +
                   (double)(ts_p1.tv_nsec - ts_start.tv_nsec) / 1e6;

    int matched_files = 0;
    for (int i = 0; i < num_files; i++)
        if (file_has_match[i]) matched_files++;

    printf("wallet_scan: pass 1 done in %.1f ms — %d/%d files contain wallet data\n",
           p1_ms, matched_files, num_files);
    fflush(stdout);

    if (matched_files == 0) {
        printf("wallet_scan: no wallet transactions found\n");
        aht_free(&aht);
        free(file_has_match);
        /* Still write empty results to clear any stale data */
        if (!wallet_scan_begin_checked(ndb, "empty-result reset begin"))
            LOG_ERR("wallet_scan", "empty-result: failed to begin db transaction");
        if (!wallet_scan_exec_checked(ndb, "DELETE FROM wallet_utxos",
                                      "empty-result clear wallet_utxos") ||
            !wallet_scan_exec_checked(ndb, "DELETE FROM wallet_transactions",
                                      "empty-result clear wallet_transactions")) {
            wallet_scan_rollback_best_effort(ndb,
                                             "empty-result reset rollback");
            LOG_ERR("wallet_scan", "empty-result: failed to clear wallet tables");
        }
        if (!wallet_scan_commit_checked(ndb, "empty-result reset commit"))
            LOG_ERR("wallet_scan", "empty-result: failed to commit db transaction");
        return 0;
    }

    /* ========== PASS 2: Selective block deserialization ========== */
    printf("wallet_scan: pass 2 — deserializing blocks in %d matched files...\n",
           matched_files);
    fflush(stdout);

    struct utxo_set uset;
    uset_init(&uset);
    struct wtx_list wl;
    wl_init(&wl);

    /* We need to process blocks in height order for correct spend tracking.
     * Build a set of heights whose blocks are in matched files, then
     * iterate heights in order. */

    /* First, find which heights map to matched files */
    int blocks_deserialized = 0;
    int found = 0;
    int cached_file = -1;
    uint8_t *fdata = NULL;
    size_t fsize = 0;

    for (int h = start_height; h <= end_height; h++) {
        const struct block_index *pi = active_chain_at(chain, h);
        if (!pi) continue;
        if (!(pi->nStatus & BLOCK_HAVE_DATA)) continue;

        /* Skip files that pass 1 ruled out */
        if (!file_has_match[pi->nFile]) continue;

        if (pi->nFile != cached_file) {
            if (fdata) munmap(fdata, fsize);
            char path[512];
            snprintf(path, sizeof(path), "%s/blocks/blk%05d.dat",
                     datadir, pi->nFile);
            int fd = open(path, O_RDONLY);
            if (fd < 0) { fdata = NULL; cached_file = -1; continue; }
            struct stat st;
            if (fstat(fd, &st) != 0) { close(fd); continue; }
            fsize = (size_t)st.st_size;
            fdata = mmap(NULL, fsize, PROT_READ, MAP_PRIVATE, fd, 0);
            close(fd);
            if (fdata == MAP_FAILED) {
                fdata = NULL; cached_file = -1; continue;
            }
            cached_file = pi->nFile;
            posix_madvise(fdata, fsize, POSIX_MADV_SEQUENTIAL);
        }
        if (!fdata || pi->nDataPos >= fsize) continue;

        struct block blk;
        block_init(&blk);
        size_t rem = fsize - pi->nDataPos;
        struct byte_stream bs;
        stream_init_from_data(&bs, fdata + pi->nDataPos, rem);
        if (!block_deserialize(&blk, &bs)) {
            block_free(&blk);
            continue;
        }
        blocks_deserialized++;

        if (scan_block_txs(&blk, h, &aht, &uset, &wl))
            found++;

        block_free(&blk);
    }
    if (fdata) munmap(fdata, fsize);

    platform_time_monotonic_timespec(&ts_p2);
    double p2_ms = (double)(ts_p2.tv_sec - ts_p1.tv_sec) * 1000.0 +
                   (double)(ts_p2.tv_nsec - ts_p1.tv_nsec) / 1e6;
    double total_ms = (double)(ts_p2.tv_sec - ts_start.tv_sec) * 1000.0 +
                      (double)(ts_p2.tv_nsec - ts_start.tv_nsec) / 1e6;

    /* Compute balance */
    int64_t balance = 0;
    int unspent = 0;
    for (int i = 0; i < uset.count; i++) {
        if (!uset.items[i].spent) {
            balance += uset.items[i].value;
            unspent++;
        }
    }

    printf("wallet_scan: pass 2 done in %.1f ms — %d blocks deserialized, "
           "%d wallet txs\n", p2_ms, blocks_deserialized, wl.count);
    printf("wallet_scan: TOTAL %.1f ms — %d unspent UTXOs, "
           "balance %.8f ZCL\n",
           total_ms, unspent, (double)balance / (double)ZATOSHI_PER_ZCL);
    fflush(stdout);

    /* ========== Write results to SQLite ========== */
    {
        bool write_tx_open = false;
        if (!wallet_scan_begin_checked(ndb, "result write begin")) {
            found = -1;
            goto write_cleanup;
        }
        write_tx_open = true;
        if (!wallet_scan_exec_checked(ndb, "DELETE FROM wallet_utxos",
                                      "result write clear wallet_utxos") ||
            !wallet_scan_exec_checked(ndb, "DELETE FROM wallet_transactions",
                                      "result write clear wallet_transactions")) {
            found = -1;
            goto write_fail;
        }

        for (int i = 0; i < uset.count; i++) {
            struct mem_utxo *u = &uset.items[i];
            struct db_wallet_utxo du;
            memset(&du, 0, sizeof(du));
            memcpy(du.txid, u->txid, 32);
            du.vout = u->vout;
            du.value = u->value;
            memcpy(du.address_hash, u->addr_hash, 20);
            du.script = u->script;
            du.script_len = u->script_len;
            du.height = u->height;
            du.is_coinbase = u->is_coinbase;
            if (!db_wallet_utxo_save(ndb, &du)) {
                LOG_WARN("wallet_scan", "wallet_scan: wallet_utxo save failed");
                found = -1;
                goto write_fail;
            }
            if (u->spent &&
                !db_wallet_utxo_mark_spent(ndb, u->txid, u->vout,
                                           u->spent_txid, u->spent_vin)) {
                LOG_WARN("wallet_scan", "wallet_scan: wallet_utxo mark_spent failed");
                found = -1;
                goto write_fail;
            }
        }

        for (int i = 0; i < wl.count; i++) {
            struct mem_wtx *t = &wl.items[i];
            struct db_wallet_tx dt;
            memset(&dt, 0, sizeof(dt));
            memcpy(dt.txid, t->txid, 32);
            dt.raw_tx = t->raw;
            dt.raw_tx_len = t->raw_len;
            dt.has_block = true;
            dt.block_height = t->height;
            dt.time_received = (int64_t)t->time;
            dt.from_me = t->from_me;
            dt.fee = t->fee;
            if (!db_wallet_tx_save(ndb, &dt)) {
                LOG_WARN("wallet_scan", "wallet_scan: wallet_tx save failed");
                found = -1;
                goto write_fail;
            }
        }

        if (!wallet_scan_commit_checked(ndb, "result write commit")) {
            found = -1;
            write_tx_open = false;
            goto write_cleanup;
        }
        write_tx_open = false;
        goto write_cleanup;

write_fail:
        if (write_tx_open)
            wallet_scan_rollback_best_effort(ndb, "result write rollback");
write_cleanup:
        ;
    }

    /* Cleanup */
    aht_free(&aht);
    uset_free(&uset);
    wl_free(&wl);
    free(file_has_match);

    return found < 0 ? -1 : wl.count;
}
