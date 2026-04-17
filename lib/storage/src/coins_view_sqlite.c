/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * SQLite-backed coins view: the canonical UTXO set lives in node.db.
 * Implements coins_view_vtable so coins_view_cache can flush here.
 * No LevelDB needed at runtime — LevelDB is import-only. */

#include <time.h>
#include "storage/coins_view_sqlite.h"
#include "coins/coins.h"
#include "coins/utxo_commitment.h"
#include "event/event.h"
#include "models/utxo.h"
#include "script/standard.h"
#include "util/log_macros.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── vtable implementations ────────────────────────────────────── */

static bool cvs_get_coins_impl(void *self, const struct uint256 *txid,
                                struct coins *out)
{
    struct coins_view_sqlite *cvs = (struct coins_view_sqlite *)self;
    return coins_view_sqlite_get_coins(cvs, txid, out);
}

static bool cvs_have_coins_impl(void *self, const struct uint256 *txid)
{
    struct coins_view_sqlite *cvs = (struct coins_view_sqlite *)self;
    return coins_view_sqlite_have_coins(cvs, txid);
}

static bool cvs_get_best_block_impl(void *self, struct uint256 *hash)
{
    struct coins_view_sqlite *cvs = (struct coins_view_sqlite *)self;
    return coins_view_sqlite_get_best_block(cvs, hash);
}

static bool cvs_batch_write_impl(void *self, struct coins_map *map_coins,
                                  const struct uint256 *hash_block)
{
    struct coins_view_sqlite *cvs = (struct coins_view_sqlite *)self;
    return coins_view_sqlite_batch_write(cvs, map_coins, hash_block);
}

static struct coins_view_vtable cvs_vtable = {
    .get_coins     = cvs_get_coins_impl,
    .have_coins    = cvs_have_coins_impl,
    .get_best_block = cvs_get_best_block_impl,
    .batch_write   = cvs_batch_write_impl,
    .get_stats     = NULL,
};

/* Boot-time integrity check: the UTXO set (`utxos` table) must not be
 * strictly ahead of the coins-flush anchor (`coins_best_block`).  This
 * guards against the class of crash-mid-flush corruption where UTXOs
 * landed but the tip pointer didn't — a silent heal here would
 * de-sync the node from consensus and potentially double-spend on
 * restart.  Returns true on OK, false on detected mismatch; the
 * operator is expected to halt + investigate on false.
 *
 * Semantics:
 *   (a) no UTXOs, no tip                 → fresh/clean
 *   (b) UTXOs present, no tip            → MISMATCH (orphan UTXOs)
 *   (c) tip set, no UTXOs                → benign (post-wipe anchor)
 *   (d) tip hash not yet in blocks table → soft WARN (block index lag)
 *   (e) max_utxo_height > tip_height     → MISMATCH (UTXOs ahead)
 */
static bool coins_view_sqlite_check_tip_consistency(sqlite3 *db)
{
    int64_t max_height = -1;
    bool have_utxos = false;

    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT MAX(height), COUNT(*) FROM utxos",
            -1, &s, NULL) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW) {
            if (sqlite3_column_type(s, 0) == SQLITE_INTEGER)
                max_height = sqlite3_column_int64(s, 0);
            have_utxos = sqlite3_column_int64(s, 1) > 0;
        }
        sqlite3_finalize(s);
    }

    uint8_t tip_hash[32];
    bool tip_set = false;
    s = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT value FROM node_state WHERE key='coins_best_block'",
            -1, &s, NULL) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW) {
            int len = sqlite3_column_bytes(s, 0);
            const void *data = sqlite3_column_blob(s, 0);
            if (data && len == 32) {
                memcpy(tip_hash, data, 32);
                tip_set = true;
            }
        }
        sqlite3_finalize(s);
    }

    if (!have_utxos && !tip_set) {
        printf("[coins] tip check: fresh DB (no utxos, no tip)\n");
        return true;
    }
    if (have_utxos && !tip_set) {
        fprintf(stderr,
            "[coins] DB_ERR_TIP_MISMATCH: utxos present "
            "(max_height=%lld) but coins_best_block is unset — "
            "probable crash between UTXO flush and tip update. "
            "Halt and investigate; do not auto-heal.\n",
            (long long)max_height);
        return false;
    }
    if (!have_utxos && tip_set) {
        printf("[coins] tip check: coins_best_block set, utxos empty — "
               "post-wipe or pre-import anchor, continuing\n");
        return true;
    }

    int64_t tip_height = -1;
    s = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT height FROM blocks WHERE hash=? AND status>=3",
            -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_blob(s, 1, tip_hash, 32, SQLITE_STATIC);
        if (sqlite3_step(s) == SQLITE_ROW)
            tip_height = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
    }

    if (tip_height < 0) {
        fprintf(stderr,
            "[coins] tip check WARN: utxos max_height=%lld, "
            "coins_best_block hash not resolvable in blocks table "
            "(status>=3) — block index load may reconcile, continuing\n",
            (long long)max_height);
        return true;
    }
    if (max_height > tip_height) {
        fprintf(stderr,
            "[coins] DB_ERR_TIP_MISMATCH: utxos max_height=%lld > "
            "tip_height=%lld (UTXOs ahead of tip) — halt and "
            "investigate; do not auto-heal.\n",
            (long long)max_height, (long long)tip_height);
        return false;
    }
    printf("[coins] tip check OK: max_utxo_height=%lld tip_height=%lld\n",
           (long long)max_height, (long long)tip_height);
    return true;
}

/* ── Open / Close ──────────────────────────────────────────────── */

bool coins_view_sqlite_open(struct coins_view_sqlite *cvs, sqlite3 *db)
{
    if (!db)
        LOG_FAIL("coins_view", "open: db handle is NULL");
    memset(cvs, 0, sizeof(*cvs));
    cvs->view.vtable = &cvs_vtable;
    cvs->view.impl = cvs;
    pthread_mutex_init(&cvs->mutex, NULL);

    /* Use the shared database handle with SAVEPOINT nesting.
     * A dedicated connection causes WAL lock contention: when node_db
     * has an open transaction, the dedicated connection's BEGIN IMMEDIATE
     * gets SQLITE_BUSY (proven at heights 3061210-3061212).
     * SAVEPOINT nests cleanly inside any existing transaction. */
    cvs->db = db;
    cvs->owns_db = false;
    printf("coins_view_sqlite: using shared connection (SAVEPOINT mode)\n");

    /* Cap WAL size to 100MB to prevent unbounded growth.
     * A 6GB WAL was observed when flush failures accumulated. */
    sqlite3_exec(db, "PRAGMA journal_size_limit = 104857600", NULL, NULL, NULL);

    int rc;

    rc = sqlite3_prepare_v2(cvs->db,
        "SELECT vout, value, script, height, is_coinbase"
        " FROM utxos WHERE txid=? ORDER BY vout",
        -1, &cvs->stmt_get, NULL);
    if (rc != SQLITE_OK) goto fail;

    rc = sqlite3_prepare_v2(cvs->db,
        "SELECT 1 FROM utxos WHERE txid=? LIMIT 1",
        -1, &cvs->stmt_have, NULL);
    if (rc != SQLITE_OK) goto fail;

    rc = sqlite3_prepare_v2(cvs->db,
        "INSERT OR REPLACE INTO utxos"
        " (txid, vout, value, script, script_type, address_hash,"
        "  height, is_coinbase)"
        " VALUES(?,?,?,?,?,?,?,?)",
        -1, &cvs->stmt_insert, NULL);
    if (rc != SQLITE_OK) goto fail;

    rc = sqlite3_prepare_v2(cvs->db,
        "DELETE FROM utxos WHERE txid=?",
        -1, &cvs->stmt_delete_tx, NULL);
    if (rc != SQLITE_OK) goto fail;

    rc = sqlite3_prepare_v2(cvs->db,
        "SELECT value FROM node_state WHERE key='coins_best_block'",
        -1, &cvs->stmt_best_get, NULL);
    if (rc != SQLITE_OK) goto fail;

    rc = sqlite3_prepare_v2(cvs->db,
        "INSERT OR REPLACE INTO node_state(key,value)"
        " VALUES('coins_best_block',?)",
        -1, &cvs->stmt_best_set, NULL);
    if (rc != SQLITE_OK) goto fail;

    rc = sqlite3_prepare_v2(cvs->db,
        "SELECT value FROM node_state WHERE key='utxo_commitment'",
        -1, &cvs->stmt_commit_get, NULL);
    if (rc != SQLITE_OK) goto fail;

    rc = sqlite3_prepare_v2(cvs->db,
        "INSERT OR REPLACE INTO node_state(key,value)"
        " VALUES('utxo_commitment',?)",
        -1, &cvs->stmt_commit_set, NULL);
    if (rc != SQLITE_OK) goto fail;

    /* Boot-time integrity check — refuses to open on detected
     * UTXO/tip mismatch.  Caller decides how to halt. */
    if (!coins_view_sqlite_check_tip_consistency(cvs->db)) {
        coins_view_sqlite_close(cvs);
        return false;
    }

    return true;

fail:
    fprintf(stderr, "coins_view_sqlite_open: prepare failed: %s\n",
            sqlite3_errmsg(cvs->db));
    coins_view_sqlite_close(cvs);
    return false;
}

void coins_view_sqlite_close(struct coins_view_sqlite *cvs)
{
    pthread_mutex_lock(&cvs->mutex);
    if (cvs->stmt_get)        { sqlite3_finalize(cvs->stmt_get);        cvs->stmt_get = NULL; }
    if (cvs->stmt_have)       { sqlite3_finalize(cvs->stmt_have);       cvs->stmt_have = NULL; }
    if (cvs->stmt_insert)     { sqlite3_finalize(cvs->stmt_insert);     cvs->stmt_insert = NULL; }
    if (cvs->stmt_delete_tx)  { sqlite3_finalize(cvs->stmt_delete_tx);  cvs->stmt_delete_tx = NULL; }
    if (cvs->stmt_best_get)   { sqlite3_finalize(cvs->stmt_best_get);   cvs->stmt_best_get = NULL; }
    if (cvs->stmt_best_set)   { sqlite3_finalize(cvs->stmt_best_set);   cvs->stmt_best_set = NULL; }
    if (cvs->stmt_commit_get) { sqlite3_finalize(cvs->stmt_commit_get); cvs->stmt_commit_get = NULL; }
    if (cvs->stmt_commit_set) { sqlite3_finalize(cvs->stmt_commit_set); cvs->stmt_commit_set = NULL; }
    if (cvs->owns_db && cvs->db) {
        sqlite3_close(cvs->db);
    }
    cvs->db = NULL;
    cvs->owns_db = false;
    pthread_mutex_unlock(&cvs->mutex);
    pthread_mutex_destroy(&cvs->mutex);
}

/* ── get_coins: build struct coins from SQLite rows ────────────── */

bool coins_view_sqlite_get_coins(struct coins_view_sqlite *cvs,
                                  const struct uint256 *txid,
                                  struct coins *out)
{
    bool found = false;

    coins_init(out);
    if (!cvs || !cvs->db || !cvs->stmt_get || !txid)
        LOG_FAIL("coins_view", "get_coins: invalid arguments (cvs=%p txid=%p)",
                 (const void *)cvs, (const void *)txid);

    pthread_mutex_lock(&cvs->mutex);
    sqlite3_stmt *s = cvs->stmt_get;
    sqlite3_reset(s);
    sqlite3_bind_blob(s, 1, txid->data, 32, SQLITE_STATIC);

    /* Two-pass: first find max vout index, then allocate and fill.
     * This avoids realloc during iteration (which caused heap corruption). */
    uint32_t max_vout = 0;
    int nrows = 0;
    int height = 0;
    int is_coinbase = 0;

    /* Pass 1: find max vout and metadata */
    while (sqlite3_step(s) == SQLITE_ROW) {
        uint32_t vi = (uint32_t)sqlite3_column_int(s, 0);
        if (nrows == 0) {
            height = sqlite3_column_int(s, 3);
            is_coinbase = sqlite3_column_int(s, 4);
        }
        if (vi > max_vout) max_vout = vi;
        nrows++;
    }

    if (nrows == 0) {
        sqlite3_reset(s);
        pthread_mutex_unlock(&cvs->mutex);
        return false;
    }
    found = true;

    /* Allocate once */
    if (!coins_alloc(out, (size_t)(max_vout + 1))) {
        sqlite3_reset(s);
        pthread_mutex_unlock(&cvs->mutex);
        LOG_FAIL("coins_view", "get_coins: coins_alloc failed for %u vouts", max_vout + 1);
    }
    out->version = 1;
    out->height = height;
    out->is_coinbase = (is_coinbase != 0);

    /* Pass 2: fill in values */
    sqlite3_reset(s);
    sqlite3_bind_blob(s, 1, txid->data, 32, SQLITE_STATIC);

    while (sqlite3_step(s) == SQLITE_ROW) {
        uint32_t vi = (uint32_t)sqlite3_column_int(s, 0);
        if (vi >= out->num_vout) continue;

        out->vout[vi].value = sqlite3_column_int64(s, 1);
        const void *script = sqlite3_column_blob(s, 2);
        int script_len = sqlite3_column_bytes(s, 2);
        if (script && script_len > 0) {
            size_t slen = (size_t)script_len;
            if (slen > MAX_SCRIPT_SIZE) slen = MAX_SCRIPT_SIZE;
            memcpy(out->vout[vi].script_pub_key.data, script, slen);
            out->vout[vi].script_pub_key.size = slen;
        }
    }

    coins_cleanup(out);
    sqlite3_reset(s);
    pthread_mutex_unlock(&cvs->mutex);
    return found;
}

/* ── have_coins: existence check ───────────────────────────────── */

bool coins_view_sqlite_have_coins(struct coins_view_sqlite *cvs,
                                   const struct uint256 *txid)
{
    if (!cvs || !cvs->stmt_have || !txid)
        LOG_FAIL("coins_view", "have_coins: invalid arguments");
    pthread_mutex_lock(&cvs->mutex);
    sqlite3_stmt *s = cvs->stmt_have;
    sqlite3_reset(s);
    sqlite3_bind_blob(s, 1, txid->data, 32, SQLITE_STATIC);
    bool found = sqlite3_step(s) == SQLITE_ROW;
    sqlite3_reset(s);
    pthread_mutex_unlock(&cvs->mutex);
    return found;
}

/* ── get_best_block ────────────────────────────────────────────── */

bool coins_view_sqlite_get_best_block(struct coins_view_sqlite *cvs,
                                       struct uint256 *hash)
{
    bool found = false;

    if (!cvs || !cvs->db || !hash)
        LOG_FAIL("coins_view", "get_best_block: invalid arguments");
    if (!cvs->stmt_best_get) { uint256_set_null(hash);
        LOG_FAIL("coins_view", "get_best_block: stmt_best_get not prepared"); }
    pthread_mutex_lock(&cvs->mutex);
    sqlite3_stmt *s = cvs->stmt_best_get;
    sqlite3_reset(s);
    if (sqlite3_step(s) == SQLITE_ROW) {
        int len = sqlite3_column_bytes(s, 0);
        const void *data = sqlite3_column_blob(s, 0);
        if (data && len >= 32) {
            memcpy(hash->data, data, 32);
            found = true;
        }
    }
    sqlite3_reset(s);
    pthread_mutex_unlock(&cvs->mutex);
    if (found)
        return true;
    uint256_set_null(hash);
    return false;
}

/* ── batch_write: flush dirty coins_map to SQLite ──────────────── */

bool coins_view_sqlite_batch_write(struct coins_view_sqlite *cvs,
                                    struct coins_map *map_coins,
                                    const struct uint256 *hash_block)
{
    return coins_view_sqlite_batch_write_ex(cvs, map_coins, hash_block, NULL);
}

bool coins_view_sqlite_batch_write_ex(struct coins_view_sqlite *cvs,
                                       struct coins_map *map_coins,
                                       const struct uint256 *hash_block,
                                       const struct utxo_commitment *commit)
{
    if (!cvs->db)
        LOG_FAIL("coins_view", "batch_write: db handle is NULL");

    /* Transaction control: dedicated connection uses BEGIN IMMEDIATE.
     * Shared handle (in-memory or fallback) uses SAVEPOINT to nest
     * inside any existing transaction from node_db batch writes. */
    const char *txn_begin = cvs->owns_db ? "BEGIN IMMEDIATE"
                                          : "SAVEPOINT coins_flush";
    const char *txn_commit = cvs->owns_db ? "COMMIT"
                                           : "RELEASE coins_flush";
    const char *txn_rollback = cvs->owns_db ? "ROLLBACK"
                                : "ROLLBACK TO SAVEPOINT coins_flush";

    pthread_mutex_lock(&cvs->mutex);

    /* Reset ALL prepared statements BEFORE starting the transaction.
     * Without this, any reader thread that left a statement in
     * SQLITE_ROW state prevents SAVEPOINT from opening (rc=5:
     * "cannot open savepoint - SQL statements in progress").
     * This was the root cause of the 6GB WAL / 15GB RAM death loop. */
    {
        sqlite3_stmt *stmt = NULL;
        while ((stmt = sqlite3_next_stmt(cvs->db, stmt)) != NULL)
            sqlite3_reset(stmt);
    }

    /* Bump busy timeout for the flush — during IBD, WAL contention
     * from background readers/checkpointers makes SQLITE_BUSY common.
     * Without adequate timeout, flush failures cause unbounded memory
     * growth (71GB WAL observed in production). */
    sqlite3_busy_timeout(cvs->db, 30000); /* 30s for flush */
    {
        char *txn_err = NULL;
        int txn_rc = sqlite3_exec(cvs->db, txn_begin,
                                   NULL, NULL, &txn_err);
        if (txn_rc != SQLITE_OK) {
            fprintf(stderr, "coins_flush: %s failed rc=%d: %s\n",
                    txn_begin, txn_rc, txn_err ? txn_err : "unknown");
            if (txn_err) sqlite3_free(txn_err);
            sqlite3_busy_timeout(cvs->db, 10000); /* restore */
            pthread_mutex_unlock(&cvs->mutex);
            return false;
        }
    }

    int write_errors = 0;
    size_t entries_written = 0;
    size_t entries_deleted = 0;

    for (size_t i = 0; i < map_coins->num_buckets; i++) {
        struct coins_map_entry *e = &map_coins->buckets[i];
        if (!e->occupied || !(e->entry.flags & COINS_CACHE_DIRTY))
            continue;

        if (coins_is_pruned(&e->entry.coins)) {
            /* All outputs spent — remove from SQLite */
            sqlite3_reset(cvs->stmt_delete_tx);
            sqlite3_bind_blob(cvs->stmt_delete_tx, 1,
                              e->txid.data, 32, SQLITE_STATIC);
            int rc = sqlite3_step(cvs->stmt_delete_tx);
            if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
                fprintf(stderr, "coins_flush: DELETE failed rc=%d: %s\n",
                        rc, sqlite3_errmsg(cvs->db));
                write_errors++;
            } else {
                entries_deleted++;
            }
        } else {
            /* Delete stale rows first, then insert current outputs */
            sqlite3_reset(cvs->stmt_delete_tx);
            sqlite3_bind_blob(cvs->stmt_delete_tx, 1,
                              e->txid.data, 32, SQLITE_STATIC);
            int rc = sqlite3_step(cvs->stmt_delete_tx);
            if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
                fprintf(stderr, "coins_flush: pre-DELETE failed rc=%d: %s\n",
                        rc, sqlite3_errmsg(cvs->db));
                write_errors++;
            }

            const struct coins *cc = &e->entry.coins;
            for (size_t vi = 0; vi < cc->num_vout; vi++) {
                if (tx_out_is_null(&cc->vout[vi]))
                    continue;

                uint8_t addr_hash[20];
                bool has_addr = false;
                enum script_type stype = utxo_classify_script(
                    cc->vout[vi].script_pub_key.data,
                    cc->vout[vi].script_pub_key.size,
                    addr_hash, &has_addr);

                sqlite3_stmt *ins = cvs->stmt_insert;
                sqlite3_reset(ins);
                sqlite3_bind_blob(ins, 1, e->txid.data, 32, SQLITE_STATIC);
                sqlite3_bind_int(ins, 2, (int)vi);
                sqlite3_bind_int64(ins, 3, cc->vout[vi].value);
                sqlite3_bind_blob(ins, 4,
                    cc->vout[vi].script_pub_key.data,
                    (int)cc->vout[vi].script_pub_key.size,
                    SQLITE_STATIC);
                sqlite3_bind_int(ins, 5, stype);
                if (has_addr)
                    sqlite3_bind_blob(ins, 6, addr_hash, 20, SQLITE_STATIC);
                else
                    sqlite3_bind_null(ins, 6);
                sqlite3_bind_int(ins, 7, cc->height);
                sqlite3_bind_int(ins, 8, cc->is_coinbase ? 1 : 0);
                rc = sqlite3_step(ins);
                if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
                    fprintf(stderr, "coins_flush: INSERT failed rc=%d "
                            "h=%d vout=%zu: %s\n",
                            rc, cc->height, vi, sqlite3_errmsg(cvs->db));
                    write_errors++;
                } else {
                    entries_written++;
                }
            }
        }
    }

    if (write_errors > 0) {
        fprintf(stderr, "coins_flush: %d write errors! "
                "Rolling back to prevent UTXO loss "
                "(wrote=%zu deleted=%zu)\n",
                write_errors, entries_written, entries_deleted);
        event_emitf(EV_COINS_FLUSH_FAILED, 0,
                    "write_errors=%d wrote=%zu deleted=%zu",
                    write_errors, entries_written, entries_deleted);
        sqlite3_exec(cvs->db, txn_rollback, NULL, NULL, NULL);
        pthread_mutex_unlock(&cvs->mutex);
        return false;
    }

    /* Verify: at least one operation should have occurred if map had dirty
     * entries. Count actual dirty entries — map_coins->size includes clean
     * (read-only) entries that don't need flushing. */
    if (entries_written == 0 && entries_deleted == 0) {
        size_t dirty_count = 0;
        for (size_t di = 0; di < map_coins->num_buckets; di++) {
            struct coins_map_entry *de = &map_coins->buckets[di];
            if (de->occupied && (de->entry.flags & COINS_CACHE_DIRTY))
                dirty_count++;
        }
        if (dirty_count > 0) {
            fprintf(stderr, "coins_flush: WARNING zero operations with "
                    "%zu dirty entries (total=%zu) — possible silent failure\n",
                    dirty_count, map_coins->size);
        }
    }

    /* Update best block hash */
    if (hash_block && !uint256_is_null(hash_block)) {
        sqlite3_reset(cvs->stmt_best_set);
        sqlite3_bind_blob(cvs->stmt_best_set, 1,
                          hash_block->data, 32, SQLITE_STATIC);
        int rc = sqlite3_step(cvs->stmt_best_set);
        if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
            fprintf(stderr, "coins_flush: best_block UPDATE failed rc=%d: "
                    "%s\n", rc, sqlite3_errmsg(cvs->db));
            sqlite3_exec(cvs->db, txn_rollback, NULL, NULL, NULL);
            pthread_mutex_unlock(&cvs->mutex);
            return false;
        }
    }

    /* Write UTXO commitment inside the same transaction (atomic with
     * the coins flush). Previously this was a separate call after flush,
     * creating a window where concurrent readers could block the next
     * SAVEPOINT. */
    if (commit) {
        uint8_t buf[UTXO_COMMITMENT_SERIALIZED_SIZE];
        utxo_commitment_serialize(commit, buf);
        sqlite3_reset(cvs->stmt_commit_set);
        sqlite3_bind_blob(cvs->stmt_commit_set, 1,
                          buf, UTXO_COMMITMENT_SERIALIZED_SIZE, SQLITE_STATIC);
        int rc = sqlite3_step(cvs->stmt_commit_set);
        if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
            fprintf(stderr, "coins_flush: commitment UPDATE failed rc=%d: "
                    "%s\n", rc, sqlite3_errmsg(cvs->db));
            /* Non-fatal: commitment is optional, don't rollback coins */
        }
    }

    /* Reset all prepared statements before COMMIT/RELEASE. */
    {
        sqlite3_stmt *stmt = NULL;
        while ((stmt = sqlite3_next_stmt(cvs->db, stmt)) != NULL)
            sqlite3_reset(stmt);
    }

    {
        char *errmsg = NULL;
        int rc = sqlite3_exec(cvs->db, txn_commit, NULL, NULL, &errmsg);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "coins_flush: %s failed: %s\n",
                    txn_commit, errmsg ? errmsg : "unknown");
            if (errmsg) sqlite3_free(errmsg);
            sqlite3_exec(cvs->db, txn_rollback, NULL, NULL, NULL);
            sqlite3_busy_timeout(cvs->db, 10000); /* restore */
            pthread_mutex_unlock(&cvs->mutex);
            return false;
        }
    }
    sqlite3_busy_timeout(cvs->db, 10000); /* restore default */

    /* Post-commit observability: both sides of the invariant we
     * enforce at boot.  Cheap to compute (MAX + COUNT on a single
     * table) and invaluable when triaging an IBD divergence. */
    {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(cvs->db,
                "SELECT IFNULL(MAX(height), -1), COUNT(*) FROM utxos",
                -1, &s, NULL) == SQLITE_OK) {
            if (sqlite3_step(s) == SQLITE_ROW) {
                int64_t max_h = sqlite3_column_int64(s, 0);
                int64_t count = sqlite3_column_int64(s, 1);
                int tip_h = (hash_block && !uint256_is_null(hash_block))
                          ? 1 : 0; /* presence flag; hash itself not resolved here */
                printf("[coins] flush ok: max_height=%lld utxos=%lld "
                       "tip_written=%d wrote=%zu deleted=%zu\n",
                       (long long)max_h, (long long)count, tip_h,
                       entries_written, entries_deleted);
            }
            sqlite3_finalize(s);
        }
    }

    pthread_mutex_unlock(&cvs->mutex);
    return true;
}

/* ── UTXO commitment persistence ───────────────────────────────── */

bool coins_view_sqlite_write_commitment(struct coins_view_sqlite *cvs,
                                         const struct utxo_commitment *uc)
{
    if (!cvs || !cvs->db || !uc || !cvs->stmt_commit_set)
        LOG_FAIL("coins_view", "write_commitment: invalid arguments or stmt not prepared");
    pthread_mutex_lock(&cvs->mutex);
    uint8_t buf[UTXO_COMMITMENT_SERIALIZED_SIZE];
    utxo_commitment_serialize(uc, buf);

    sqlite3_reset(cvs->stmt_commit_set);
    sqlite3_bind_blob(cvs->stmt_commit_set, 1,
                      buf, UTXO_COMMITMENT_SERIALIZED_SIZE, SQLITE_STATIC);
    bool ok = sqlite3_step(cvs->stmt_commit_set) == SQLITE_DONE;
    sqlite3_reset(cvs->stmt_commit_set);
    pthread_mutex_unlock(&cvs->mutex);
    return ok;
}

bool coins_view_sqlite_read_commitment(struct coins_view_sqlite *cvs,
                                        struct utxo_commitment *uc)
{
    if (!cvs || !cvs->db || !uc) { if (uc) utxo_commitment_init(uc);
        LOG_FAIL("coins_view", "read_commitment: invalid arguments"); }
    if (!cvs->stmt_commit_get) { utxo_commitment_init(uc);
        LOG_FAIL("coins_view", "read_commitment: stmt_commit_get not prepared"); }
    pthread_mutex_lock(&cvs->mutex);
    sqlite3_reset(cvs->stmt_commit_get);
    if (sqlite3_step(cvs->stmt_commit_get) == SQLITE_ROW) {
        int len = sqlite3_column_bytes(cvs->stmt_commit_get, 0);
        const void *data = sqlite3_column_blob(cvs->stmt_commit_get, 0);
        sqlite3_reset(cvs->stmt_commit_get);
        pthread_mutex_unlock(&cvs->mutex);
        if (data && len >= UTXO_COMMITMENT_SERIALIZED_SIZE)
            return utxo_commitment_deserialize(uc, data, (size_t)len);
        utxo_commitment_init(uc);
        return false;
    }
    sqlite3_reset(cvs->stmt_commit_get);
    pthread_mutex_unlock(&cvs->mutex);
    utxo_commitment_init(uc);
    return false;
}
