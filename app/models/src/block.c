/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ActiveRecord model: Block
 *
 * validates :hash, :prev_hash, :merkle_root, presence: true
 * validates :height, :file_num, :data_pos, :undo_pos, numericality: { >= 0 }
 * validates :height, maximum: 100_000_000
 * validates :time, :bits, not_zero: true
 * validates :num_tx, range: [0, 100_000]
 *
 * has_many :transactions
 * has_many :utxos, through: height
 * belongs_to :prev_block
 * has_one :next_block
 *
 * after_save -> emit EV_MODEL_SAVED */

#include "models/block.h"
#include "models/tx_index.h"
#include "models/utxo.h"
#include "event/event.h"
#include <string.h>
#include <stdio.h>

/* ── Callbacks ─────────────────────────────────────────────────── */

DEFINE_MODEL_CALLBACKS(block)

/* ── Validation ────────────────────────────────────────────────── */

bool db_block_validate(const struct db_block *b, struct ar_errors *errors)
{
    ar_errors_clear(errors);

    /* Required fields */
    validates_presence_of(errors, b, hash);
    validates_presence_of(errors, b, prev_hash);
    validates_presence_of(errors, b, merkle_root);

    /* Range checks */
    validates_non_negative(errors, b, height);
    validates_max(errors, b, height, 100000000);
    validates_not_zero(errors, b, time);
    validates_max(errors, b, time, 4294967295U);
    validates_not_zero(errors, b, bits);

    /* Transaction count */
    validates_non_negative(errors, b, num_tx);
    validates_max(errors, b, num_tx, 100000);

    /* File position */
    validates_non_negative(errors, b, file_num);
    validates_non_negative(errors, b, data_pos);
    validates_non_negative(errors, b, undo_pos);

    /* Equihash solution */
    validates_custom(errors,
        b->solution_len <= (size_t)INT32_MAX,
        "solution_len", "exceeds max size");
    validates_custom(errors,
        !(b->solution_len > 0 && !b->solution),
        "solution", "length set but pointer is null");

    return !ar_errors_any(errors);
}

/* ── Save ──────────────────────────────────────────────────────── */

bool db_block_save(struct node_db *ndb, const struct db_block *b)
{
    if (!ndb->open) return false;

    struct ar_callbacks *cbs = db_block_callbacks();
    AR_VALIDATE_RECORD(cbs, "block", b, db_block_validate);
    /* before_save callbacks */
    if (!ar_run_before_save(cbs, (void *)b)) return false;

    sqlite3_stmt *s = ndb->stmt_block_insert;
    sqlite3_reset(s);
    AR_BIND_BLOB(s, 1, b->hash, 32);
    AR_BIND_INT(s, 2, b->height);
    AR_BIND_BLOB(s, 3, b->prev_hash, 32);
    AR_BIND_INT(s, 4, b->version);
    AR_BIND_BLOB(s, 5, b->merkle_root, 32);
    AR_BIND_INT(s, 6, b->time);
    AR_BIND_INT(s, 7, b->bits);
    AR_BIND_BLOB(s, 8, b->nonce, 32);
    AR_BIND_BLOB(s, 9, b->solution, (int)b->solution_len);
    AR_BIND_BLOB(s, 10, b->chain_work, 32);
    AR_BIND_INT(s, 11, b->status);
    AR_BIND_INT(s, 12, b->file_num);
    AR_BIND_INT(s, 13, b->data_pos);
    AR_BIND_INT(s, 14, b->undo_pos);
    AR_BIND_INT(s, 15, b->num_tx);
    AR_BIND_BLOB(s, 16, b->sapling_root, 32);
    AR_BIND_BLOB(s, 17, b->sprout_root, 32);
    AR_BIND_INT(s, 18, b->sapling_value);
    AR_BIND_INT(s, 19, b->sprout_value);

    bool ok = AR_STEP_DONE(s);

    if (!ok) {
        static int lock_err_count = 0;
        int rc = sqlite3_errcode(ndb->db);
        if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
            lock_err_count++;
            if (lock_err_count <= 3 || (lock_err_count % 1000 == 0))
                fprintf(stderr, "db_block_save: locked at height %d "
                        "(%d total, transient during file sync)\n",
                        b->height, lock_err_count);
        } else {
            fprintf(stderr, "db_block_save: INSERT failed at height %d: "
                    "%s (rc=%d)\n", b->height, sqlite3_errmsg(ndb->db), rc);
        }
    }

    if (ok) {
        ar_run_after_save(cbs, (void *)b);
        event_emitf(EV_MODEL_SAVED, 0, "model=block height=%d ntx=%d",
                    b->height, b->num_tx);
    }
    return ok;
}

/* ── Read helpers ──────────────────────────────────────────────── */

static void read_block_cols(sqlite3_stmt *s, int col,
                            struct db_block *out)
{
    AR_READ_BLOB(s, col, out->prev_hash, 32);     col++;
    out->version = (int)AR_COL_INT(s, col++);
    AR_READ_BLOB(s, col, out->merkle_root, 32);    col++;
    out->time = (uint32_t)AR_COL_INT(s, col++);
    out->bits = (uint32_t)AR_COL_INT(s, col++);
    AR_READ_BLOB(s, col, out->nonce, 32);          col++;
    out->solution_len = (size_t)AR_COL_BYTES(s, col);
    out->solution = NULL;                           col++;
    AR_READ_BLOB(s, col, out->chain_work, 32);     col++;
    out->status = (int)AR_COL_INT(s, col++);
    out->file_num = (int)AR_COL_INT(s, col++);
    out->data_pos = (int)AR_COL_INT(s, col++);
    out->undo_pos = (int)AR_COL_INT(s, col++);
    out->num_tx = (int)AR_COL_INT(s, col++);
    AR_READ_BLOB(s, col, out->sapling_root, 32);   col++;
    AR_READ_BLOB(s, col, out->sprout_root, 32);    col++;
    out->sapling_value = AR_COL_INT(s, col++);
    out->sprout_value = AR_COL_INT(s, col++);
}

/* ── Find ──────────────────────────────────────────────────────── */

bool db_block_find_by_hash(struct node_db *ndb,
                           const uint8_t hash[32],
                           struct db_block *out)
{
    if (!ndb->open) return false;
    sqlite3_stmt *s = ndb->stmt_block_by_hash;
    sqlite3_reset(s);
    AR_BIND_BLOB(s, 1, hash, 32);
    if (!AR_STEP_ROW(s)) return false;
    memset(out, 0, sizeof(*out));
    memcpy(out->hash, hash, 32);
    out->height = (int)AR_COL_INT(s, 0);
    read_block_cols(s, 1, out);
    return true;
}

bool db_block_find_by_height(struct node_db *ndb, int height,
                             struct db_block *out)
{
    if (!ndb->open) return false;
    sqlite3_stmt *s = ndb->stmt_block_by_height;
    sqlite3_reset(s);
    AR_BIND_INT(s, 1, height);
    if (!AR_STEP_ROW(s)) return false;
    memset(out, 0, sizeof(*out));
    out->height = height;
    AR_READ_BLOB(s, 0, out->hash, 32);
    read_block_cols(s, 1, out);
    return true;
}

/* ── Delete ────────────────────────────────────────────────────── */

bool db_block_delete(struct node_db *ndb, const uint8_t hash[32])
{
    if (!ndb->open) return false;

    struct ar_callbacks *cbs = db_block_callbacks();
    struct db_block blk;
    memset(&blk, 0, sizeof(blk));
    memcpy(blk.hash, hash, 32);
    if (!ar_run_before_destroy(cbs, &blk)) return false;

    /* dependent: :destroy — delete child transactions */
    sqlite3_stmt *dt = NULL;
    if (sqlite3_prepare_v2(ndb->db,
            "DELETE FROM transactions WHERE block_hash=?",
            -1, &dt, NULL) == SQLITE_OK && dt) {
        AR_BIND_BLOB(dt, 1, hash, 32);
        (void)AR_STEP_DONE(dt);
        AR_FINALIZE(dt);
    }

    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(ndb->db,
            "DELETE FROM blocks WHERE hash=?",
            -1, &s, NULL) != SQLITE_OK || !s)
        return false;
    AR_BIND_BLOB(s, 1, hash, 32);
    bool ok = AR_STEP_DONE(s);
    AR_FINALIZE(s);
    if (ok) {
        ar_run_after_destroy(cbs, &blk);
        event_emitf(EV_MODEL_DESTROYED, 0, "model=block");
    }
    return ok;
}

/* ── Queries ───────────────────────────────────────────────────── */

int db_block_max_height(struct node_db *ndb)
{
    if (!ndb->open) return -1;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT MAX(height) FROM blocks WHERE status>=3",
        -1, &s, NULL);
    int h = -1;
    if (AR_STEP_ROW(s))
        h = (int)AR_COL_INT(s, 0);
    AR_FINALIZE(s);
    return h;
}

int db_block_count(struct node_db *ndb)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT COUNT(*) FROM blocks",
        -1, &s, NULL);
    int c = 0;
    if (AR_STEP_ROW(s))
        c = (int)AR_COL_INT(s, 0);
    AR_FINALIZE(s);
    return c;
}

bool db_block_save_batch(struct node_db *ndb,
                         const struct db_block *blocks,
                         size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (!db_block_save(ndb, &blocks[i]))
            return false;
    }
    return true;
}

/* ── Relationships ─────────────────────────────────────────────── */

/* has_many :transactions */
int db_block_transactions(struct node_db *ndb, const uint8_t hash[32],
                          struct db_tx_index *out, size_t max)
{
    return db_tx_find_by_block(ndb, hash, out, max);
}

/* has_many :utxos (created at this height) */
int db_block_utxos(struct node_db *ndb, int height,
                   struct db_utxo *out, size_t max)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT txid,vout,value,script,script_type,"
        "address_hash,is_coinbase FROM utxos WHERE height=?"
        " LIMIT ?",
        -1, &s, NULL);
    AR_BIND_INT(s, 1, height);
    AR_BIND_INT(s, 2, (int)max);
    int count = 0;
    while (AR_STEP_ROW(s) && count < (int)max) {
        memset(&out[count], 0, sizeof(out[count]));
        AR_READ_BLOB(s, 0, out[count].txid, 32);
        out[count].vout = (uint32_t)AR_COL_INT(s, 1);
        out[count].value = AR_COL_INT(s, 2);
        out[count].script_len = (size_t)AR_COL_BYTES(s, 3);
        out[count].script = NULL;
        out[count].script_type = (int)AR_COL_INT(s, 4);
        AR_READ_BLOB(s, 5, out[count].address_hash, 20);
        if (sqlite3_column_blob(s, 5))
            out[count].has_address = true;
        out[count].is_coinbase = AR_COL_INT(s, 6) != 0;
        out[count].height = height;
        count++;
    }
    AR_FINALIZE(s);
    return count;
}

/* belongs_to :prev_block */
bool db_block_prev(struct node_db *ndb, const struct db_block *b,
                   struct db_block *out)
{
    return db_block_find_by_hash(ndb, b->prev_hash, out);
}

/* has_one :next_block */
bool db_block_next(struct node_db *ndb, const struct db_block *b,
                   struct db_block *out)
{
    return db_block_find_by_height(ndb, b->height + 1, out);
}

/* ── Scope: hashes in height range ────────────────────────────── */

int db_block_hashes_in_range(struct node_db *ndb,
                             int start_height, int end_height,
                             uint8_t (*hashes_out)[32], size_t max)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT hash FROM blocks WHERE height >= ? "
        "AND height <= ? ORDER BY height ASC",
        -1, &s, NULL);
    if (!s) return 0;
    AR_BIND_INT(s, 1, start_height);
    AR_BIND_INT(s, 2, end_height);
    int count = 0;
    while (AR_STEP_ROW(s) && (size_t)count < max) {
        AR_READ_BLOB(s, 0, hashes_out[count], 32);
        count++;
    }
    AR_FINALIZE(s);
    return count;
}
