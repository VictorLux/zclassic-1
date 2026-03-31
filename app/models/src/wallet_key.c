/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "models/wallet_key.h"
#include "models/wallet_tx.h"
#include "support/cleanse.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* ── Callbacks ─────────────────────────────────────────────────── */

static struct ar_callbacks wkey_cbs, skey_cbs;
static bool wkey_cbs_init, skey_cbs_init;

struct ar_callbacks *db_wallet_key_callbacks(void)
{
    if (!wkey_cbs_init) { ar_callbacks_init(&wkey_cbs); wkey_cbs_init = true; }
    return &wkey_cbs;
}

struct ar_callbacks *db_sapling_key_callbacks(void)
{
    if (!skey_cbs_init) { ar_callbacks_init(&skey_cbs); skey_cbs_init = true; }
    return &skey_cbs;
}

/* ── Validation ────────────────────────────────────────────────── */

bool db_wallet_key_validate(const struct db_wallet_key *k,
                            struct ar_errors *errors)
{
    ar_errors_clear(errors);
    validates_presence_of(errors, k, pubkey_hash);
    validates_presence_of(errors, k, pubkey);
    validates_presence_of(errors, k, privkey);
    validates_custom(errors, k->pubkey_len == 33,
                     "pubkey_len", "must be 33 (compressed)");
    validates_custom(errors, !(k->pubkey_len == 33 && !k->compressed),
                     "compressed", "must be true for 33-byte pubkey");
    validates_non_negative(errors, k, created_at);
    return !ar_errors_any(errors);
}

/* ── CRUD ──────────────────────────────────────────────────────── */

bool db_wallet_key_save(struct node_db *ndb, const struct db_wallet_key *k)
{
    if (!ndb->open) return false;
    /* Auto-timestamp if caller didn't set created_at */
    if (k->created_at == 0)
        ((struct db_wallet_key *)k)->created_at = (int64_t)time(NULL);
    struct ar_errors errors;
    ar_errors_clear(&errors);
    if (!db_wallet_key_validate(k, &errors)) {
        AR_LOG_VALIDATION_FAILURE("wallet_key", &errors);
        return false;
    }
    if (!ar_run_before_save(&wkey_cbs, (void *)k)) return false;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "INSERT OR REPLACE INTO wallet_keys"
        "(pubkey_hash,pubkey,privkey,compressed,created_at)"
        " VALUES(?,?,?,?,?)",
        -1, &s, NULL);
    AR_BIND_BLOB(s, 1, k->pubkey_hash, 20);
    AR_BIND_BLOB(s, 2, k->pubkey, (int)k->pubkey_len);
    AR_BIND_BLOB(s, 3, k->privkey, 32);
    AR_BIND_INT(s, 4, k->compressed ? 1 : 0);
    AR_BIND_INT(s, 5, k->created_at);
    int rc = sqlite3_step(s);
    AR_FINALIZE(s);
    bool ok = rc == SQLITE_DONE;
    if (ok) ar_run_after_save(&wkey_cbs, (void *)k);
    return ok;
}

bool db_wallet_key_find(struct node_db *ndb, const uint8_t pubkey_hash[20],
                        struct db_wallet_key *out)
{
    if (!ndb->open) return false;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT pubkey,privkey,compressed,created_at"
        " FROM wallet_keys WHERE pubkey_hash=?",
        -1, &s, NULL);
    AR_BIND_BLOB(s, 1, pubkey_hash, 20);
    if (!AR_STEP_ROW(s)) {
        AR_FINALIZE(s);
        return false;
    }
    memset(out, 0, sizeof(*out));
    memcpy(out->pubkey_hash, pubkey_hash, 20);
    int pk_len = AR_COL_BYTES(s, 0);
    const void *pk = sqlite3_column_blob(s, 0);
    if (pk && pk_len <= 33) {
        memcpy(out->pubkey, pk, (size_t)pk_len);
        out->pubkey_len = (size_t)pk_len;
    }
    AR_READ_BLOB(s, 1, out->privkey, 32);
    out->compressed = AR_COL_INT(s, 2) != 0;
    out->created_at = AR_COL_INT(s, 3);
    AR_FINALIZE(s);
    return true;
}

bool db_wallet_key_delete(struct node_db *ndb, const uint8_t pubkey_hash[20])
{
    if (!ndb->open) return false;

    struct ar_callbacks *cbs = db_wallet_key_callbacks();
    struct db_wallet_key k;
    memset(&k, 0, sizeof(k));
    memcpy(k.pubkey_hash, pubkey_hash, 20);
    if (!ar_run_before_destroy(cbs, &k)) return false;

    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "DELETE FROM wallet_keys WHERE pubkey_hash=?", -1, &s, NULL);
    sqlite3_bind_blob(s, 1, pubkey_hash, 20, SQLITE_STATIC);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);

    bool ok = rc == SQLITE_DONE;
    if (ok) ar_run_after_destroy(cbs, &k);
    return ok;
}

bool db_wallet_key_exists(struct node_db *ndb, const uint8_t pubkey_hash[20])
{
    if (!ndb->open) return false;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT 1 FROM wallet_keys WHERE pubkey_hash=?",
        -1, &s, NULL);
    sqlite3_bind_blob(s, 1, pubkey_hash, 20, SQLITE_STATIC);
    bool found = sqlite3_step(s) == SQLITE_ROW;
    sqlite3_finalize(s);
    return found;
}

int db_wallet_key_count(struct node_db *ndb)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT COUNT(*) FROM wallet_keys", -1, &s, NULL);
    int c = 0;
    if (sqlite3_step(s) == SQLITE_ROW)
        c = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return c;
}

int db_wallet_key_each(struct node_db *ndb, wallet_key_cb cb, void *ctx)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT pubkey_hash,pubkey,privkey,compressed,created_at"
        " FROM wallet_keys",
        -1, &s, NULL);
    int count = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        struct db_wallet_key k;
        memset(&k, 0, sizeof(k));
        const void *ph = sqlite3_column_blob(s, 0);
        if (ph) memcpy(k.pubkey_hash, ph, 20);
        int pk_len = sqlite3_column_bytes(s, 1);
        const void *pk = sqlite3_column_blob(s, 1);
        if (pk && pk_len <= 33) {
            memcpy(k.pubkey, pk, (size_t)pk_len);
            k.pubkey_len = (size_t)pk_len;
        }
        const void *sk = sqlite3_column_blob(s, 2);
        if (sk) memcpy(k.privkey, sk, 32);
        k.compressed = sqlite3_column_int(s, 3) != 0;
        k.created_at = sqlite3_column_int64(s, 4);
        cb(&k, ctx);
        memory_cleanse(k.privkey, 32);
        count++;
    }
    sqlite3_finalize(s);
    return count;
}

/* Sapling keys */

bool db_sapling_key_validate(const struct db_sapling_key *k,
                              struct ar_errors *errors)
{
    ar_errors_clear(errors);
    validates_presence_of(errors, k, ivk);
    validates_presence_of(errors, k, xsk);
    validates_presence_of(errors, k, xfvk);
    validates_presence_of(errors, k, diversifier);
    validates_presence_of(errors, k, pk_d);
    if (k->address[0] == '\0')
        ar_errors_add(errors, "address", "can't be blank");
    return !ar_errors_any(errors);
}

bool db_sapling_key_save(struct node_db *ndb, const struct db_sapling_key *k)
{
    if (!ndb->open) return false;
    struct ar_errors errors;
    ar_errors_clear(&errors);
    if (!db_sapling_key_validate(k, &errors)) {
        fprintf(stderr, "sapling_key save FAILED: %s\n", ar_errors_full(&errors));
        return false;
    }
    if (!ar_run_before_save(&skey_cbs, (void *)k)) return false;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "INSERT OR REPLACE INTO wallet_sapling_keys"
        "(ivk,xsk,xfvk,diversifier,pk_d,child_index,address)"
        " VALUES(?,?,?,?,?,?,?)",
        -1, &s, NULL);
    sqlite3_bind_blob(s, 1, k->ivk, 32, SQLITE_STATIC);
    sqlite3_bind_blob(s, 2, k->xsk, 169, SQLITE_STATIC);
    sqlite3_bind_blob(s, 3, k->xfvk, 169, SQLITE_STATIC);
    sqlite3_bind_blob(s, 4, k->diversifier, 11, SQLITE_STATIC);
    sqlite3_bind_blob(s, 5, k->pk_d, 32, SQLITE_STATIC);
    sqlite3_bind_int(s, 6, (int)k->child_index);
    sqlite3_bind_text(s, 7, k->address, -1, SQLITE_STATIC);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    bool ok = rc == SQLITE_DONE;
    if (ok) ar_run_after_save(&skey_cbs, (void *)k);
    return ok;
}

/* Read sapling_key columns: ivk,xsk,xfvk,diversifier,pk_d,child_index,address
 * starting at column offset `col`. */
static void db_sapling_key_read_row(sqlite3_stmt *s, int col,
                                     struct db_sapling_key *out)
{
    memset(out, 0, sizeof(*out));
    const void *ivk = sqlite3_column_blob(s, col);
    if (ivk) memcpy(out->ivk, ivk, 32);
    col++;
    const void *xsk = sqlite3_column_blob(s, col);
    if (xsk) memcpy(out->xsk, xsk, 169);
    col++;
    const void *xfvk = sqlite3_column_blob(s, col);
    if (xfvk) memcpy(out->xfvk, xfvk, 169);
    col++;
    const void *div = sqlite3_column_blob(s, col);
    if (div) memcpy(out->diversifier, div, 11);
    col++;
    const void *pkd = sqlite3_column_blob(s, col);
    if (pkd) memcpy(out->pk_d, pkd, 32);
    col++;
    out->child_index = (uint32_t)sqlite3_column_int(s, col++);
    const char *addr = (const char *)sqlite3_column_text(s, col);
    if (addr) {
        size_t len = strlen(addr);
        if (len >= sizeof(out->address)) len = sizeof(out->address) - 1;
        memcpy(out->address, addr, len);
        out->address[len] = '\0';
    }
}

bool db_sapling_key_find_by_ivk(struct node_db *ndb, const uint8_t ivk[32],
                                struct db_sapling_key *out)
{
    if (!ndb->open) return false;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT ivk,xsk,xfvk,diversifier,pk_d,child_index,address"
        " FROM wallet_sapling_keys WHERE ivk=?",
        -1, &s, NULL);
    if (!s) return false;
    sqlite3_bind_blob(s, 1, ivk, 32, SQLITE_STATIC);
    if (sqlite3_step(s) != SQLITE_ROW) {
        sqlite3_finalize(s);
        return false;
    }
    if (!out) {
        sqlite3_finalize(s);
        return true;
    }
    db_sapling_key_read_row(s, 0, out);
    sqlite3_finalize(s);
    return true;
}

bool db_sapling_key_find_by_address(struct node_db *ndb, const char *address,
                                    struct db_sapling_key *out)
{
    if (!ndb->open) return false;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT ivk,xsk,xfvk,diversifier,pk_d,child_index,address"
        " FROM wallet_sapling_keys WHERE address=?",
        -1, &s, NULL);
    sqlite3_bind_text(s, 1, address, -1, SQLITE_STATIC);
    if (sqlite3_step(s) != SQLITE_ROW) {
        sqlite3_finalize(s);
        return false;
    }
    db_sapling_key_read_row(s, 0, out);
    sqlite3_finalize(s);
    return true;
}

int db_sapling_key_count(struct node_db *ndb)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT COUNT(*) FROM wallet_sapling_keys", -1, &s, NULL);
    int c = 0;
    if (sqlite3_step(s) == SQLITE_ROW)
        c = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return c;
}

int db_sapling_key_each(struct node_db *ndb, sapling_key_cb cb, void *ctx)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT ivk,xsk,xfvk,diversifier,pk_d,child_index,address"
        " FROM wallet_sapling_keys",
        -1, &s, NULL);
    int count = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        struct db_sapling_key k;
        db_sapling_key_read_row(s, 0, &k);
        cb(&k, ctx);
        count++;
    }
    sqlite3_finalize(s);
    return count;
}

/* Wallet seed */

bool db_wallet_seed_save(struct node_db *ndb, const uint8_t seed[32],
                         uint32_t next_child)
{
    if (!ndb->open) return false;
    static const uint8_t zero[32] = {0};
    if (memcmp(seed, zero, 32) == 0) return false;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "INSERT OR REPLACE INTO wallet_seed(id,seed,next_child)"
        " VALUES(1,?,?)",
        -1, &s, NULL);
    sqlite3_bind_blob(s, 1, seed, 32, SQLITE_STATIC);
    sqlite3_bind_int(s, 2, (int)next_child);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    return rc == SQLITE_DONE;
}

bool db_wallet_seed_load(struct node_db *ndb, uint8_t seed[32],
                         uint32_t *next_child)
{
    if (!ndb->open) return false;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT seed,next_child FROM wallet_seed WHERE id=1",
        -1, &s, NULL);
    if (sqlite3_step(s) != SQLITE_ROW) {
        sqlite3_finalize(s);
        return false;
    }
    const void *sd = sqlite3_column_blob(s, 0);
    if (sd) memcpy(seed, sd, 32);
    *next_child = (uint32_t)sqlite3_column_int(s, 1);
    sqlite3_finalize(s);
    return true;
}

/* Redeem scripts */

bool db_wallet_script_save(struct node_db *ndb, const struct db_wallet_script *sc)
{
    if (!ndb->open) return false;
    static const uint8_t zero[20] = {0};
    if (memcmp(sc->script_hash, zero, 20) == 0) return false;
    if (!sc->redeem_script || sc->script_len == 0) return false;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "INSERT OR REPLACE INTO wallet_scripts(script_hash,redeem_script)"
        " VALUES(?,?)",
        -1, &s, NULL);
    sqlite3_bind_blob(s, 1, sc->script_hash, 20, SQLITE_STATIC);
    sqlite3_bind_blob(s, 2, sc->redeem_script, (int)sc->script_len,
                      SQLITE_STATIC);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    return rc == SQLITE_DONE;
}

bool db_wallet_script_find(struct node_db *ndb, const uint8_t script_hash[20],
                           struct db_wallet_script *out)
{
    if (!ndb->open) return false;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT redeem_script FROM wallet_scripts WHERE script_hash=?",
        -1, &s, NULL);
    sqlite3_bind_blob(s, 1, script_hash, 20, SQLITE_STATIC);
    if (sqlite3_step(s) != SQLITE_ROW) {
        sqlite3_finalize(s);
        return false;
    }
    memset(out, 0, sizeof(*out));
    memcpy(out->script_hash, script_hash, 20);
    out->script_len = (size_t)sqlite3_column_bytes(s, 0);
    const void *rs = sqlite3_column_blob(s, 0);
    if (rs && out->script_len > 0) {
        out->redeem_script = malloc(out->script_len);
        if (out->redeem_script)
            memcpy(out->redeem_script, rs, out->script_len);
    }
    sqlite3_finalize(s);
    return true;
}

int db_wallet_script_each(struct node_db *ndb, wallet_script_cb cb, void *ctx)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT script_hash,redeem_script FROM wallet_scripts",
        -1, &s, NULL);
    int count = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        struct db_wallet_script sc;
        memset(&sc, 0, sizeof(sc));
        const void *sh = sqlite3_column_blob(s, 0);
        if (sh) memcpy(sc.script_hash, sh, 20);
        sc.script_len = (size_t)sqlite3_column_bytes(s, 1);
        const void *rs = sqlite3_column_blob(s, 1);
        if (rs && sc.script_len > 0) {
            sc.redeem_script = malloc(sc.script_len);
            if (sc.redeem_script)
                memcpy(sc.redeem_script, rs, sc.script_len);
        }
        cb(&sc, ctx);
        free(sc.redeem_script);
        count++;
    }
    sqlite3_finalize(s);
    return count;
}

/* ── Relationships ─────────────────────────────────────────────── */

/* WalletKey has_many :wallet_utxos */
int db_wallet_key_utxos(struct node_db *ndb, const uint8_t pubkey_hash[20],
                        struct db_wallet_utxo *out, size_t max)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT txid,vout,value,script,height,is_coinbase"
        " FROM wallet_utxos WHERE address_hash=?"
        " AND spent_txid IS NULL ORDER BY value DESC LIMIT ?",
        -1, &s, NULL);
    sqlite3_bind_blob(s, 1, pubkey_hash, 20, SQLITE_STATIC);
    sqlite3_bind_int(s, 2, (int)max);
    int count = 0;
    while (sqlite3_step(s) == SQLITE_ROW && (size_t)count < max) {
        memset(&out[count], 0, sizeof(out[count]));
        const void *t = sqlite3_column_blob(s, 0);
        if (t) memcpy(out[count].txid, t, 32);
        out[count].vout = (uint32_t)sqlite3_column_int(s, 1);
        out[count].value = sqlite3_column_int64(s, 2);
        memcpy(out[count].address_hash, pubkey_hash, 20);
        out[count].script_len = (size_t)sqlite3_column_bytes(s, 3);
        out[count].script = NULL;
        out[count].height = sqlite3_column_int(s, 4);
        out[count].is_coinbase = sqlite3_column_int(s, 5) != 0;
        count++;
    }
    sqlite3_finalize(s);
    return count;
}

/* SaplingKey has_many :sapling_notes */
int db_sapling_key_notes(struct node_db *ndb, const uint8_t ivk[32],
                         struct db_sapling_note *out, size_t max)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT txid,output_index,value,rcm,memo,diversifier,pk_d,"
        "cm,nullifier,block_height"
        " FROM wallet_sapling_notes WHERE ivk=?"
        " AND spent_txid IS NULL ORDER BY value DESC LIMIT ?",
        -1, &s, NULL);
    sqlite3_bind_blob(s, 1, ivk, 32, SQLITE_STATIC);
    sqlite3_bind_int(s, 2, (int)max);
    int count = 0;
    while (sqlite3_step(s) == SQLITE_ROW && (size_t)count < max) {
        memset(&out[count], 0, sizeof(out[count]));
        const void *t = sqlite3_column_blob(s, 0);
        if (t) memcpy(out[count].txid, t, 32);
        out[count].output_index = (uint32_t)sqlite3_column_int(s, 1);
        out[count].value = sqlite3_column_int64(s, 2);
        const void *rcm = sqlite3_column_blob(s, 3);
        if (rcm) memcpy(out[count].rcm, rcm, 32);
        int memo_len = sqlite3_column_bytes(s, 4);
        const void *memo = sqlite3_column_blob(s, 4);
        if (memo && memo_len > 0) {
            size_t ml = (size_t)memo_len < 512 ? (size_t)memo_len : 512;
            memcpy(out[count].memo, memo, ml);
            out[count].memo_len = ml;
        }
        memcpy(out[count].ivk, ivk, 32);
        const void *div = sqlite3_column_blob(s, 5);
        if (div) memcpy(out[count].diversifier, div, 11);
        const void *pkd = sqlite3_column_blob(s, 6);
        if (pkd) memcpy(out[count].pk_d, pkd, 32);
        const void *cm = sqlite3_column_blob(s, 7);
        if (cm) memcpy(out[count].cm, cm, 32);
        const void *nf = sqlite3_column_blob(s, 8);
        if (nf) memcpy(out[count].nullifier, nf, 32);
        if (sqlite3_column_type(s, 9) != SQLITE_NULL)
            out[count].block_height = sqlite3_column_int(s, 9);
        count++;
    }
    sqlite3_finalize(s);
    return count;
}
