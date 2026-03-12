/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "db/model_block.h"
#include "db/model_tx.h"
#include "db/model_utxo.h"
#include <string.h>
#include <stdio.h>

/* ── Callbacks ─────────────────────────────────────────────────── */

static struct ar_callbacks block_cbs;
static bool block_cbs_init = false;

struct ar_callbacks *db_block_callbacks(void)
{
    if (!block_cbs_init) {
        ar_callbacks_init(&block_cbs);
        block_cbs_init = true;
    }
    return &block_cbs;
}

/* ── Validation ────────────────────────────────────────────────── */

bool db_block_validate(const struct db_block *b, struct ar_errors *errors)
{
    ar_errors_clear(errors);
    validates_presence_of(errors, b, hash);
    validates_presence_of(errors, b, prev_hash);
    validates_presence_of(errors, b, merkle_root);
    validates_non_negative(errors, b, height);
    if (b->time == 0)
        ar_errors_add(errors, "time", "can't be zero");
    if (b->bits == 0)
        ar_errors_add(errors, "bits", "can't be zero");
    return !ar_errors_any(errors);
}

/* ── Save ──────────────────────────────────────────────────────── */

bool db_block_save(struct node_db *ndb, const struct db_block *b)
{
    if (!ndb->open) return false;

    /* Validate before save */
    struct ar_errors errors;
    if (!db_block_validate(b, &errors)) return false;

    /* Run before_save callbacks */
    struct ar_callbacks *cbs = db_block_callbacks();
    if (!ar_run_before_save(cbs, (void *)b)) return false;

    sqlite3_stmt *s = ndb->stmt_block_insert;
    sqlite3_reset(s);
    sqlite3_bind_blob(s, 1, b->hash, 32, SQLITE_STATIC);
    sqlite3_bind_int(s, 2, b->height);
    sqlite3_bind_blob(s, 3, b->prev_hash, 32, SQLITE_STATIC);
    sqlite3_bind_int(s, 4, b->version);
    sqlite3_bind_blob(s, 5, b->merkle_root, 32, SQLITE_STATIC);
    sqlite3_bind_int64(s, 6, b->time);
    sqlite3_bind_int64(s, 7, b->bits);
    sqlite3_bind_blob(s, 8, b->nonce, 32, SQLITE_STATIC);
    sqlite3_bind_blob(s, 9, b->solution,
                      (int)b->solution_len, SQLITE_STATIC);
    sqlite3_bind_blob(s, 10, b->chain_work, 32, SQLITE_STATIC);
    sqlite3_bind_int(s, 11, b->status);
    sqlite3_bind_int(s, 12, b->file_num);
    sqlite3_bind_int(s, 13, b->data_pos);
    sqlite3_bind_int(s, 14, b->undo_pos);
    sqlite3_bind_int(s, 15, b->num_tx);
    sqlite3_bind_blob(s, 16, b->sapling_root, 32, SQLITE_STATIC);
    sqlite3_bind_blob(s, 17, b->sprout_root, 32, SQLITE_STATIC);
    sqlite3_bind_int64(s, 18, b->sapling_value);
    sqlite3_bind_int64(s, 19, b->sprout_value);

    bool ok = sqlite3_step(s) == SQLITE_DONE;

    if (ok) ar_run_after_save(cbs, (void *)b);
    return ok;
}

/* Read columns starting at col_start */
static void read_block_cols(sqlite3_stmt *s, int col,
                            struct db_block *out)
{
    const void *ph = sqlite3_column_blob(s, col);
    if (ph) memcpy(out->prev_hash, ph, 32);
    col++;
    out->version = sqlite3_column_int(s, col++);
    const void *mr = sqlite3_column_blob(s, col);
    if (mr) memcpy(out->merkle_root, mr, 32);
    col++;
    out->time = (uint32_t)sqlite3_column_int64(s, col++);
    out->bits = (uint32_t)sqlite3_column_int64(s, col++);
    const void *nc = sqlite3_column_blob(s, col);
    if (nc) memcpy(out->nonce, nc, 32);
    col++;
    out->solution_len = (size_t)sqlite3_column_bytes(s, col);
    out->solution = NULL;
    col++;
    const void *cw = sqlite3_column_blob(s, col);
    if (cw) memcpy(out->chain_work, cw, 32);
    col++;
    out->status = sqlite3_column_int(s, col++);
    out->file_num = sqlite3_column_int(s, col++);
    out->data_pos = sqlite3_column_int(s, col++);
    out->undo_pos = sqlite3_column_int(s, col++);
    out->num_tx = sqlite3_column_int(s, col++);
    const void *sr = sqlite3_column_blob(s, col);
    if (sr) memcpy(out->sapling_root, sr, 32);
    col++;
    const void *spr = sqlite3_column_blob(s, col);
    if (spr) memcpy(out->sprout_root, spr, 32);
    col++;
    out->sapling_value = sqlite3_column_int64(s, col++);
    out->sprout_value = sqlite3_column_int64(s, col++);
}

bool db_block_find_by_hash(struct node_db *ndb,
                           const uint8_t hash[32],
                           struct db_block *out)
{
    if (!ndb->open) return false;
    sqlite3_stmt *s = ndb->stmt_block_by_hash;
    sqlite3_reset(s);
    sqlite3_bind_blob(s, 1, hash, 32, SQLITE_STATIC);
    if (sqlite3_step(s) != SQLITE_ROW) return false;
    memset(out, 0, sizeof(*out));
    memcpy(out->hash, hash, 32);
    out->height = sqlite3_column_int(s, 0);
    read_block_cols(s, 1, out);
    return true;
}

bool db_block_find_by_height(struct node_db *ndb, int height,
                             struct db_block *out)
{
    if (!ndb->open) return false;
    sqlite3_stmt *s = ndb->stmt_block_by_height;
    sqlite3_reset(s);
    sqlite3_bind_int(s, 1, height);
    if (sqlite3_step(s) != SQLITE_ROW) return false;
    memset(out, 0, sizeof(*out));
    out->height = height;
    const void *h = sqlite3_column_blob(s, 0);
    if (h) memcpy(out->hash, h, 32);
    read_block_cols(s, 1, out);
    return true;
}

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
    sqlite3_prepare_v2(ndb->db,
        "DELETE FROM transactions WHERE block_hash=?", -1, &dt, NULL);
    sqlite3_bind_blob(dt, 1, hash, 32, SQLITE_STATIC);
    sqlite3_step(dt);
    sqlite3_finalize(dt);

    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "DELETE FROM blocks WHERE hash=?",
        -1, &s, NULL);
    sqlite3_bind_blob(s, 1, hash, 32, SQLITE_STATIC);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);

    bool ok = rc == SQLITE_DONE;
    if (ok) ar_run_after_destroy(cbs, &blk);
    return ok;
}

int db_block_max_height(struct node_db *ndb)
{
    if (!ndb->open) return -1;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT MAX(height) FROM blocks WHERE status>=3",
        -1, &s, NULL);
    int h = -1;
    if (sqlite3_step(s) == SQLITE_ROW)
        h = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
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
    if (sqlite3_step(s) == SQLITE_ROW)
        c = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
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
    sqlite3_bind_int(s, 1, height);
    sqlite3_bind_int(s, 2, (int)max);
    int count = 0;
    while (sqlite3_step(s) == SQLITE_ROW && count < (int)max) {
        memset(&out[count], 0, sizeof(out[count]));
        const void *t = sqlite3_column_blob(s, 0);
        if (t) memcpy(out[count].txid, t, 32);
        out[count].vout = (uint32_t)sqlite3_column_int(s, 1);
        out[count].value = sqlite3_column_int64(s, 2);
        out[count].script_len = (size_t)sqlite3_column_bytes(s, 3);
        out[count].script = NULL;
        out[count].script_type = sqlite3_column_int(s, 4);
        const void *ah = sqlite3_column_blob(s, 5);
        if (ah) {
            memcpy(out[count].address_hash, ah, 20);
            out[count].has_address = true;
        }
        out[count].is_coinbase = sqlite3_column_int(s, 6) != 0;
        out[count].height = height;
        count++;
    }
    sqlite3_finalize(s);
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
