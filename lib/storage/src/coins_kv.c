/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * coins_kv — implementation. See storage/coins_kv.h for the contract and the
 * durability rationale (docs/work/tip-durability-collapse.md).
 *
 * Raw sqlite3_step calls carry // raw-sql-ok:progress-kv-kernel-store, the
 * sanctioned hatch for the kernel store (same convention as progress_store.c /
 * utxo_projection.c). The coins set sits BELOW the AR lifecycle — it is reducer
 * state, not an AR model.
 */
#include "storage/coins_kv.h"

#include "coins/coins_view.h"
#include "crypto/sha3.h"
#include "primitives/transaction.h"
#include "script/script.h"

#include <sqlite3.h>
#include <string.h>

bool coins_kv_ensure_schema(sqlite3 *db)
{
    if (!db) return false;
    char *err = NULL;
    int rc = sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS coins ("
        "  txid        BLOB    NOT NULL,"
        "  vout        INTEGER NOT NULL,"
        "  value       INTEGER NOT NULL,"
        "  height      INTEGER NOT NULL,"
        "  is_coinbase INTEGER NOT NULL,"
        "  script      BLOB    NOT NULL,"
        "  PRIMARY KEY (txid, vout)"
        ") WITHOUT ROWID",
        NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}

bool coins_kv_add(sqlite3 *db, const uint8_t txid[32], uint32_t vout,
                  int64_t value, int32_t height, bool is_coinbase,
                  const uint8_t *script, size_t script_len)
{
    if (!db || !txid) return false;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO coins"
        "(txid,vout,value,height,is_coinbase,script) VALUES(?,?,?,?,?,?)",
        -1, &s, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_blob (s, 1, txid, 32, SQLITE_TRANSIENT);
    sqlite3_bind_int  (s, 2, (int)vout);
    sqlite3_bind_int64(s, 3, (sqlite3_int64)value);
    sqlite3_bind_int64(s, 4, (sqlite3_int64)height);
    sqlite3_bind_int  (s, 5, is_coinbase ? 1 : 0);
    if (script && script_len > 0)
        sqlite3_bind_blob(s, 6, script, (int)script_len, SQLITE_TRANSIENT);
    else
        sqlite3_bind_blob(s, 6, "", 0, SQLITE_STATIC);
    int rc = sqlite3_step(s);  // raw-sql-ok:progress-kv-kernel-store
    sqlite3_finalize(s);
    return rc == SQLITE_DONE;
}

bool coins_kv_spend(sqlite3 *db, const uint8_t txid[32], uint32_t vout)
{
    if (!db || !txid) return false;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
        "DELETE FROM coins WHERE txid=? AND vout=?",
        -1, &s, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_blob(s, 1, txid, 32, SQLITE_TRANSIENT);
    sqlite3_bind_int (s, 2, (int)vout);
    int rc = sqlite3_step(s);  // raw-sql-ok:progress-kv-kernel-store
    sqlite3_finalize(s);
    return rc == SQLITE_DONE;
}

bool coins_kv_exists(sqlite3 *db, const uint8_t txid[32], uint32_t vout)
{
    if (!db || !txid) return false;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT 1 FROM coins WHERE txid=? AND vout=?",
        -1, &s, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_blob(s, 1, txid, 32, SQLITE_TRANSIENT);
    sqlite3_bind_int (s, 2, (int)vout);
    bool found = sqlite3_step(s) == SQLITE_ROW;  // raw-sql-ok:progress-kv-kernel-store
    sqlite3_finalize(s);
    return found;
}

int64_t coins_kv_count(sqlite3 *db)
{
    if (!db) return -1;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM coins",
                           -1, &s, NULL) != SQLITE_OK)
        return -1;
    int64_t n = -1;
    if (sqlite3_step(s) == SQLITE_ROW)  // raw-sql-ok:progress-kv-kernel-store
        n = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return n;
}

bool coins_kv_get_coins(sqlite3 *db, const uint8_t txid[32], struct coins *out)
{
    if (!out) return false;
    coins_init(out);
    if (!db || !txid) return false;

    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT vout, value, script, height, is_coinbase "
        "FROM coins WHERE txid=?",
        -1, &s, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_blob(s, 1, txid, 32, SQLITE_TRANSIENT);

    uint32_t max_vout = 0;
    int nrows = 0, height = 0, is_coinbase = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {  // raw-sql-ok:progress-kv-kernel-store
        uint32_t vi = (uint32_t)sqlite3_column_int(s, 0);
        if (nrows == 0) {
            height      = sqlite3_column_int(s, 3);
            is_coinbase = sqlite3_column_int(s, 4);
        }
        if (vi > max_vout) max_vout = vi;
        nrows++;
    }
    if (nrows == 0) {
        sqlite3_finalize(s);
        return false;
    }

    if (!coins_alloc(out, (size_t)(max_vout + 1))) {
        sqlite3_finalize(s);
        return false;
    }
    out->version     = 1;
    out->height      = height;
    out->is_coinbase = (is_coinbase != 0);

    sqlite3_reset(s);
    sqlite3_bind_blob(s, 1, txid, 32, SQLITE_TRANSIENT);
    while (sqlite3_step(s) == SQLITE_ROW) {  // raw-sql-ok:progress-kv-kernel-store
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
    sqlite3_finalize(s);
    return true;
}

bool coins_kv_setinfo(sqlite3 *db, int64_t *num_txs, int64_t *num_txouts,
                      int64_t *total_amount)
{
    if (!db) return false;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT COUNT(DISTINCT txid), COUNT(*), COALESCE(SUM(value),0) "
            "FROM coins", -1, &s, NULL) != SQLITE_OK)
        return false;
    bool ok = false;
    if (sqlite3_step(s) == SQLITE_ROW) {  // raw-sql-ok:progress-kv-kernel-store
        if (num_txs)      *num_txs      = sqlite3_column_int64(s, 0);
        if (num_txouts)   *num_txouts   = sqlite3_column_int64(s, 1);
        if (total_amount) *total_amount = sqlite3_column_int64(s, 2);
        ok = true;
    }
    sqlite3_finalize(s);
    return ok;
}

int coins_kv_commitment(sqlite3 *db, uint8_t out[32])
{
    if (!db || !out) return -1;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT txid, vout, value, script, height, is_coinbase "
            "FROM coins ORDER BY txid, vout", -1, &s, NULL) != SQLITE_OK)
        return -1;

    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);

    int rc;
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {  // raw-sql-ok:progress-kv-kernel-store
        const uint8_t *txid = (const uint8_t *)sqlite3_column_blob(s, 0);
        int txid_len = sqlite3_column_bytes(s, 0);
        if (!txid || txid_len < 32) continue;

        uint32_t vout   = (uint32_t)sqlite3_column_int(s, 1);
        int64_t  value  = sqlite3_column_int64(s, 2);
        const uint8_t *script = (const uint8_t *)sqlite3_column_blob(s, 3);
        int script_len = sqlite3_column_bytes(s, 3);
        int32_t  height = sqlite3_column_int(s, 4);
        int cb_int = sqlite3_column_int(s, 5);

        /* BYTE-IDENTICAL to utxo_projection_commitment: txid(32) || vout(LE4)
         * || value(LE8) || script_len(LE4) || script || height(LE4) || cb(1). */
        sha3_256_write(&ctx, txid, 32);

        uint8_t le4[4];
        le4[0] = (uint8_t)(vout);       le4[1] = (uint8_t)(vout >>  8);
        le4[2] = (uint8_t)(vout >> 16); le4[3] = (uint8_t)(vout >> 24);
        sha3_256_write(&ctx, le4, 4);

        uint8_t le8[8];
        uint64_t v = (uint64_t)value;
        for (int i = 0; i < 8; i++) le8[i] = (uint8_t)(v >> (8 * i));
        sha3_256_write(&ctx, le8, 8);

        uint32_t slen = (uint32_t)(script_len > 0 ? script_len : 0);
        le4[0] = (uint8_t)(slen);       le4[1] = (uint8_t)(slen >>  8);
        le4[2] = (uint8_t)(slen >> 16); le4[3] = (uint8_t)(slen >> 24);
        sha3_256_write(&ctx, le4, 4);
        if (script && script_len > 0)
            sha3_256_write(&ctx, script, (size_t)script_len);

        uint32_t ht = (uint32_t)height;
        le4[0] = (uint8_t)(ht);       le4[1] = (uint8_t)(ht >>  8);
        le4[2] = (uint8_t)(ht >> 16); le4[3] = (uint8_t)(ht >> 24);
        sha3_256_write(&ctx, le4, 4);

        uint8_t cb_byte = (uint8_t)(cb_int ? 1 : 0);
        sha3_256_write(&ctx, &cb_byte, 1);
    }
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE)
        return -1;

    sha3_256_finalize(&ctx, out);
    return 0;
}
