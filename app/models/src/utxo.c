/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "models/utxo.h"
#include "models/tx_index.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ── Script Classification (single shared implementation) ──────── */

enum script_type utxo_classify_script(const uint8_t *script, size_t len,
                                       uint8_t addr_hash[20], bool *has_addr)
{
    *has_addr = false;

    /* P2PKH: OP_DUP OP_HASH160 <20> <hash> OP_EQUALVERIFY OP_CHECKSIG */
    if (len == 25 &&
        script[0] == 0x76 && script[1] == 0xa9 &&
        script[2] == 0x14 &&
        script[23] == 0x88 && script[24] == 0xac) {
        memcpy(addr_hash, script + 3, 20);
        *has_addr = true;
        return SCRIPT_P2PKH;
    }

    /* P2SH: OP_HASH160 <20> <hash> OP_EQUAL */
    if (len == 23 &&
        script[0] == 0xa9 && script[1] == 0x14 &&
        script[22] == 0x87) {
        memcpy(addr_hash, script + 2, 20);
        *has_addr = true;
        return SCRIPT_P2SH;
    }

    /* OP_RETURN */
    if (len > 0 && script[0] == 0x6a)
        return SCRIPT_OP_RETURN;

    return SCRIPT_OTHER;
}

/* ── Callbacks ─────────────────────────────────────────────────── */

static struct ar_callbacks utxo_cbs;
static bool utxo_cbs_init = false;

struct ar_callbacks *db_utxo_callbacks(void)
{
    if (!utxo_cbs_init) {
        ar_callbacks_init(&utxo_cbs);
        utxo_cbs_init = true;
    }
    return &utxo_cbs;
}

/* ── Validation ────────────────────────────────────────────────── */

bool db_utxo_validate(const struct db_utxo *u, struct ar_errors *errors)
{
    ar_errors_clear(errors);
    validates_presence_of(errors, u, txid);
    validates_non_negative(errors, u, value);
    if (u->value > 2100000000000000LL)
        ar_errors_add(errors, "value", "exceeds MAX_MONEY");
    validates_non_negative(errors, u, height);
    if (u->script_len > 10000)
        ar_errors_add(errors, "script_len", "exceeds max script size");
    if (u->script_len > 0 && !u->script)
        ar_errors_add(errors, "script", "null pointer with nonzero length");
    static const enum script_type valid_types[] = {
        SCRIPT_P2PKH, SCRIPT_P2SH, SCRIPT_OP_RETURN,
        SCRIPT_MULTISIG, SCRIPT_OTHER
    };
    validates_inclusion_of(errors, u, script_type, valid_types, 5);
    if (u->has_address) {
        static const uint8_t z[20] = {0};
        if (memcmp(u->address_hash, z, 20) == 0)
            ar_errors_add(errors, "address_hash",
                          "can't be blank when has_address");
    }
    return !ar_errors_any(errors);
}

/* ── CRUD ──────────────────────────────────────────────────────── */

bool db_utxo_save(struct node_db *ndb, const struct db_utxo *u)
{
    if (!ndb->open) return false;

    struct ar_errors errors;
    if (!db_utxo_validate(u, &errors)) return false;

    struct ar_callbacks *cbs = db_utxo_callbacks();
    if (!ar_run_before_save(cbs, (void *)u)) return false;

    sqlite3_stmt *s = ndb->stmt_utxo_insert;
    sqlite3_reset(s);
    sqlite3_bind_blob(s, 1, u->txid, 32, SQLITE_STATIC);
    sqlite3_bind_int(s, 2, (int)u->vout);
    sqlite3_bind_int64(s, 3, u->value);
    sqlite3_bind_blob(s, 4, u->script, (int)u->script_len, SQLITE_STATIC);
    sqlite3_bind_int(s, 5, (int)u->script_type);
    if (u->has_address)
        sqlite3_bind_blob(s, 6, u->address_hash, 20, SQLITE_STATIC);
    else
        sqlite3_bind_null(s, 6);
    sqlite3_bind_int(s, 7, u->height);
    sqlite3_bind_int(s, 8, u->is_coinbase ? 1 : 0);

    bool ok = sqlite3_step(s) == SQLITE_DONE;
    if (ok) ar_run_after_save(cbs, (void *)u);
    return ok;
}

bool db_utxo_find(struct node_db *ndb, const uint8_t txid[32], uint32_t vout,
                  struct db_utxo *out)
{
    if (!ndb->open) return false;
    sqlite3_stmt *s = ndb->stmt_utxo_find;
    sqlite3_reset(s);
    sqlite3_bind_blob(s, 1, txid, 32, SQLITE_STATIC);
    sqlite3_bind_int(s, 2, (int)vout);
    if (sqlite3_step(s) != SQLITE_ROW) return false;
    memset(out, 0, sizeof(*out));
    memcpy(out->txid, txid, 32);
    out->vout = vout;
    out->value = sqlite3_column_int64(s, 0);
    out->script_len = (size_t)sqlite3_column_bytes(s, 1);
    const void *sc = sqlite3_column_blob(s, 1);
    if (sc && out->script_len > 0) {
        out->script = malloc(out->script_len);
        if (out->script)
            memcpy(out->script, sc, out->script_len);
    } else {
        out->script = NULL;
    }
    out->script_type = (enum script_type)sqlite3_column_int(s, 2);
    const void *ah = sqlite3_column_blob(s, 3);
    if (ah && sqlite3_column_bytes(s, 3) >= 20) {
        memcpy(out->address_hash, ah, 20);
        out->has_address = true;
    }
    out->height = sqlite3_column_int(s, 4);
    out->is_coinbase = sqlite3_column_int(s, 5) != 0;
    return true;
}

bool db_utxo_exists(struct node_db *ndb, const uint8_t txid[32], uint32_t vout)
{
    if (!ndb->open) return false;
    sqlite3_stmt *s = ndb->stmt_utxo_find;
    sqlite3_reset(s);
    sqlite3_bind_blob(s, 1, txid, 32, SQLITE_STATIC);
    sqlite3_bind_int(s, 2, (int)vout);
    bool found = sqlite3_step(s) == SQLITE_ROW;
    sqlite3_reset(s);
    return found;
}

bool db_utxo_delete(struct node_db *ndb, const uint8_t txid[32], uint32_t vout)
{
    if (!ndb->open) return false;

    struct ar_callbacks *cbs = db_utxo_callbacks();
    struct db_utxo u;
    memset(&u, 0, sizeof(u));
    memcpy(u.txid, txid, 32);
    u.vout = vout;
    if (!ar_run_before_destroy(cbs, &u)) return false;

    sqlite3_stmt *s = ndb->stmt_utxo_delete;
    sqlite3_reset(s);
    sqlite3_bind_blob(s, 1, txid, 32, SQLITE_STATIC);
    sqlite3_bind_int(s, 2, (int)vout);

    bool ok = sqlite3_step(s) == SQLITE_DONE;
    if (ok) ar_run_after_destroy(cbs, &u);
    return ok;
}

int64_t db_utxo_balance_for_address(struct node_db *ndb,
                                     const uint8_t address_hash[20])
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT COALESCE(SUM(value),0) FROM utxos WHERE address_hash=?",
        -1, &s, NULL);
    sqlite3_bind_blob(s, 1, address_hash, 20, SQLITE_STATIC);
    int64_t bal = 0;
    if (sqlite3_step(s) == SQLITE_ROW)
        bal = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return bal;
}

int db_utxo_list_for_address(struct node_db *ndb,
                             const uint8_t address_hash[20],
                             struct db_utxo *out, size_t max)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT txid,vout,value,script_type,height,is_coinbase"
        " FROM utxos WHERE address_hash=? ORDER BY height",
        -1, &s, NULL);
    sqlite3_bind_blob(s, 1, address_hash, 20, SQLITE_STATIC);
    int count = 0;
    while (sqlite3_step(s) == SQLITE_ROW && (size_t)count < max) {
        memset(&out[count], 0, sizeof(out[count]));
        const void *t = sqlite3_column_blob(s, 0);
        if (t && sqlite3_column_bytes(s, 0) >= 32)
            memcpy(out[count].txid, t, 32);
        out[count].vout = (uint32_t)sqlite3_column_int(s, 1);
        out[count].value = sqlite3_column_int64(s, 2);
        out[count].script_type = (enum script_type)sqlite3_column_int(s, 3);
        memcpy(out[count].address_hash, address_hash, 20);
        out[count].has_address = true;
        out[count].height = sqlite3_column_int(s, 4);
        out[count].is_coinbase = sqlite3_column_int(s, 5) != 0;
        count++;
    }
    sqlite3_finalize(s);
    return count;
}

int64_t db_utxo_count(struct node_db *ndb)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db, "SELECT COUNT(*) FROM utxos", -1, &s, NULL);
    int64_t c = 0;
    if (sqlite3_step(s) == SQLITE_ROW)
        c = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return c;
}

void db_utxo_free(struct db_utxo *u)
{
    if (!u) return;
    free(u->script);
    u->script = NULL;
    u->script_len = 0;
}

/* ── Relationships ─────────────────────────────────────────────── */

/* belongs_to :transaction */
bool db_utxo_transaction(struct node_db *ndb, const struct db_utxo *u,
                         struct db_tx_index *out)
{
    return db_tx_find(ndb, u->txid, out);
}
