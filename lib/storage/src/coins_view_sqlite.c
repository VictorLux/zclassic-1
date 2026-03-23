/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * SQLite-backed coins view: the canonical UTXO set lives in node.db.
 * Implements coins_view_vtable so coins_view_cache can flush here.
 * No LevelDB needed at runtime — LevelDB is import-only. */

#include "storage/coins_view_sqlite.h"
#include "coins/coins.h"
#include "coins/utxo_commitment.h"
#include "script/standard.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Script classification (same as sync_controller.c) ─────────── */

static int classify_script_for_coins(const uint8_t *script, size_t len,
                                     uint8_t addr_hash[20], bool *has_addr)
{
    *has_addr = false;

    /* P2PKH: OP_DUP OP_HASH160 <20> <hash> OP_EQUALVERIFY OP_CHECKSIG */
    if (len == 25 && script[0] == 0x76 && script[1] == 0xa9 &&
        script[2] == 0x14 && script[23] == 0x88 && script[24] == 0xac) {
        memcpy(addr_hash, script + 3, 20);
        *has_addr = true;
        return 1; /* SCRIPT_P2PKH */
    }

    /* P2SH: OP_HASH160 <20> <hash> OP_EQUAL */
    if (len == 23 && script[0] == 0xa9 && script[1] == 0x14 &&
        script[22] == 0x87) {
        memcpy(addr_hash, script + 2, 20);
        *has_addr = true;
        return 2; /* SCRIPT_P2SH */
    }

    /* OP_RETURN */
    if (len > 0 && script[0] == 0x6a)
        return 3; /* SCRIPT_OP_RETURN */

    return 0; /* SCRIPT_OTHER */
}

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

/* ── Open / Close ──────────────────────────────────────────────── */

bool coins_view_sqlite_open(struct coins_view_sqlite *cvs, sqlite3 *db)
{
    if (!db) return false;
    memset(cvs, 0, sizeof(*cvs));
    cvs->db = db;
    cvs->view.vtable = &cvs_vtable;
    cvs->view.impl = cvs;

    int rc;

    rc = sqlite3_prepare_v2(db,
        "SELECT vout, value, script, height, is_coinbase"
        " FROM utxos WHERE txid=? ORDER BY vout",
        -1, &cvs->stmt_get, NULL);
    if (rc != SQLITE_OK) goto fail;

    rc = sqlite3_prepare_v2(db,
        "SELECT 1 FROM utxos WHERE txid=? LIMIT 1",
        -1, &cvs->stmt_have, NULL);
    if (rc != SQLITE_OK) goto fail;

    rc = sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO utxos"
        " (txid, vout, value, script, script_type, address_hash,"
        "  height, is_coinbase)"
        " VALUES(?,?,?,?,?,?,?,?)",
        -1, &cvs->stmt_insert, NULL);
    if (rc != SQLITE_OK) goto fail;

    rc = sqlite3_prepare_v2(db,
        "DELETE FROM utxos WHERE txid=?",
        -1, &cvs->stmt_delete_tx, NULL);
    if (rc != SQLITE_OK) goto fail;

    rc = sqlite3_prepare_v2(db,
        "SELECT value FROM node_state WHERE key='coins_best_block'",
        -1, &cvs->stmt_best_get, NULL);
    if (rc != SQLITE_OK) goto fail;

    rc = sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO node_state(key,value)"
        " VALUES('coins_best_block',?)",
        -1, &cvs->stmt_best_set, NULL);
    if (rc != SQLITE_OK) goto fail;

    rc = sqlite3_prepare_v2(db,
        "SELECT value FROM node_state WHERE key='utxo_commitment'",
        -1, &cvs->stmt_commit_get, NULL);
    if (rc != SQLITE_OK) goto fail;

    rc = sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO node_state(key,value)"
        " VALUES('utxo_commitment',?)",
        -1, &cvs->stmt_commit_set, NULL);
    if (rc != SQLITE_OK) goto fail;

    return true;

fail:
    fprintf(stderr, "coins_view_sqlite_open: prepare failed: %s\n",
            sqlite3_errmsg(db));
    coins_view_sqlite_close(cvs);
    return false;
}

void coins_view_sqlite_close(struct coins_view_sqlite *cvs)
{
    if (cvs->stmt_get)        { sqlite3_finalize(cvs->stmt_get);        cvs->stmt_get = NULL; }
    if (cvs->stmt_have)       { sqlite3_finalize(cvs->stmt_have);       cvs->stmt_have = NULL; }
    if (cvs->stmt_insert)     { sqlite3_finalize(cvs->stmt_insert);     cvs->stmt_insert = NULL; }
    if (cvs->stmt_delete_tx)  { sqlite3_finalize(cvs->stmt_delete_tx);  cvs->stmt_delete_tx = NULL; }
    if (cvs->stmt_best_get)   { sqlite3_finalize(cvs->stmt_best_get);   cvs->stmt_best_get = NULL; }
    if (cvs->stmt_best_set)   { sqlite3_finalize(cvs->stmt_best_set);   cvs->stmt_best_set = NULL; }
    if (cvs->stmt_commit_get) { sqlite3_finalize(cvs->stmt_commit_get); cvs->stmt_commit_get = NULL; }
    if (cvs->stmt_commit_set) { sqlite3_finalize(cvs->stmt_commit_set); cvs->stmt_commit_set = NULL; }
    cvs->db = NULL;
}

/* ── get_coins: build struct coins from SQLite rows ────────────── */

bool coins_view_sqlite_get_coins(struct coins_view_sqlite *cvs,
                                  const struct uint256 *txid,
                                  struct coins *out)
{
    coins_init(out);

    sqlite3_stmt *s = cvs->stmt_get;
    sqlite3_reset(s);
    sqlite3_bind_blob(s, 1, txid->data, 32, SQLITE_STATIC);

    /* First pass: collect rows to find max vout */
    struct {
        uint32_t vout;
        int64_t value;
        const void *script;
        int script_len;
        int height;
        int is_coinbase;
    } rows[4096];
    int nrows = 0;
    uint32_t max_vout = 0;

    while (sqlite3_step(s) == SQLITE_ROW && nrows < 4096) {
        rows[nrows].vout = (uint32_t)sqlite3_column_int(s, 0);
        rows[nrows].value = sqlite3_column_int64(s, 1);
        rows[nrows].script = sqlite3_column_blob(s, 2);
        rows[nrows].script_len = sqlite3_column_bytes(s, 2);
        rows[nrows].height = sqlite3_column_int(s, 3);
        rows[nrows].is_coinbase = sqlite3_column_int(s, 4);
        if (rows[nrows].vout > max_vout)
            max_vout = rows[nrows].vout;
        nrows++;
    }

    if (nrows == 0)
        return false;

    /* Allocate vout array (gaps will be null/spent) */
    coins_alloc(out, (size_t)(max_vout + 1));
    out->version = 1;
    out->height = rows[0].height;
    out->is_coinbase = (rows[0].is_coinbase != 0);

    /* Fill in available outputs */
    for (int i = 0; i < nrows; i++) {
        uint32_t vi = rows[i].vout;
        if (vi >= out->num_vout) continue;
        out->vout[vi].value = rows[i].value;
        if (rows[i].script && rows[i].script_len > 0) {
            size_t slen = (size_t)rows[i].script_len;
            if (slen > MAX_SCRIPT_SIZE) slen = MAX_SCRIPT_SIZE;
            memcpy(out->vout[vi].script_pub_key.data,
                   rows[i].script, slen);
            out->vout[vi].script_pub_key.size = slen;
        }
    }

    coins_cleanup(out);
    return true;
}

/* ── have_coins: existence check ───────────────────────────────── */

bool coins_view_sqlite_have_coins(struct coins_view_sqlite *cvs,
                                   const struct uint256 *txid)
{
    sqlite3_stmt *s = cvs->stmt_have;
    sqlite3_reset(s);
    sqlite3_bind_blob(s, 1, txid->data, 32, SQLITE_STATIC);
    return sqlite3_step(s) == SQLITE_ROW;
}

/* ── get_best_block ────────────────────────────────────────────── */

bool coins_view_sqlite_get_best_block(struct coins_view_sqlite *cvs,
                                       struct uint256 *hash)
{
    sqlite3_stmt *s = cvs->stmt_best_get;
    sqlite3_reset(s);
    if (sqlite3_step(s) == SQLITE_ROW) {
        int len = sqlite3_column_bytes(s, 0);
        const void *data = sqlite3_column_blob(s, 0);
        if (data && len >= 32) {
            memcpy(hash->data, data, 32);
            return true;
        }
    }
    uint256_set_null(hash);
    return false;
}

/* ── batch_write: flush dirty coins_map to SQLite ──────────────── */

bool coins_view_sqlite_batch_write(struct coins_view_sqlite *cvs,
                                    struct coins_map *map_coins,
                                    const struct uint256 *hash_block)
{
    if (!cvs->db) return false;

    /* Only start a transaction if one isn't already active.
     * sync_controller may have an open batch transaction on
     * the same sqlite3 handle. */
    bool own_txn = (sqlite3_get_autocommit(cvs->db) != 0);
    if (own_txn)
        sqlite3_exec(cvs->db, "BEGIN", NULL, NULL, NULL);

    for (size_t i = 0; i < map_coins->num_buckets; i++) {
        struct coins_map_entry *e = &map_coins->buckets[i];
        if (!e->occupied || !(e->entry.flags & COINS_CACHE_DIRTY))
            continue;

        if (coins_is_pruned(&e->entry.coins)) {
            /* All outputs spent — remove from SQLite */
            sqlite3_reset(cvs->stmt_delete_tx);
            sqlite3_bind_blob(cvs->stmt_delete_tx, 1,
                              e->txid.data, 32, SQLITE_STATIC);
            sqlite3_step(cvs->stmt_delete_tx);
        } else {
            /* Delete stale rows first, then insert current outputs */
            sqlite3_reset(cvs->stmt_delete_tx);
            sqlite3_bind_blob(cvs->stmt_delete_tx, 1,
                              e->txid.data, 32, SQLITE_STATIC);
            sqlite3_step(cvs->stmt_delete_tx);

            const struct coins *cc = &e->entry.coins;
            for (size_t vi = 0; vi < cc->num_vout; vi++) {
                if (tx_out_is_null(&cc->vout[vi]))
                    continue;

                uint8_t addr_hash[20];
                bool has_addr = false;
                int stype = classify_script_for_coins(
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
                sqlite3_step(ins);
            }
        }
    }

    /* Update best block hash */
    if (hash_block && !uint256_is_null(hash_block)) {
        sqlite3_reset(cvs->stmt_best_set);
        sqlite3_bind_blob(cvs->stmt_best_set, 1,
                          hash_block->data, 32, SQLITE_STATIC);
        sqlite3_step(cvs->stmt_best_set);
    }

    if (own_txn) {
        char *errmsg = NULL;
        sqlite3_exec(cvs->db, "COMMIT", NULL, NULL, &errmsg);
        if (errmsg) {
            fprintf(stderr, "coins_view_sqlite batch_write COMMIT: %s\n",
                    errmsg);
            sqlite3_free(errmsg);
            return false;
        }
    }
    return true;
}

/* ── UTXO commitment persistence ───────────────────────────────── */

bool coins_view_sqlite_write_commitment(struct coins_view_sqlite *cvs,
                                         const struct utxo_commitment *uc)
{
    uint8_t buf[UTXO_COMMITMENT_SERIALIZED_SIZE];
    utxo_commitment_serialize(uc, buf);

    sqlite3_reset(cvs->stmt_commit_set);
    sqlite3_bind_blob(cvs->stmt_commit_set, 1,
                      buf, UTXO_COMMITMENT_SERIALIZED_SIZE, SQLITE_STATIC);
    return sqlite3_step(cvs->stmt_commit_set) == SQLITE_DONE;
}

bool coins_view_sqlite_read_commitment(struct coins_view_sqlite *cvs,
                                        struct utxo_commitment *uc)
{
    sqlite3_reset(cvs->stmt_commit_get);
    if (sqlite3_step(cvs->stmt_commit_get) == SQLITE_ROW) {
        int len = sqlite3_column_bytes(cvs->stmt_commit_get, 0);
        const void *data = sqlite3_column_blob(cvs->stmt_commit_get, 0);
        if (data && len >= UTXO_COMMITMENT_SERIALIZED_SIZE)
            return utxo_commitment_deserialize(uc, data, (size_t)len);
    }
    utxo_commitment_init(uc);
    return false;
}
