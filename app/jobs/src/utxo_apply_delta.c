/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * utxo_apply_delta — implementation. See jobs/utxo_apply_delta.h.
 *
 * Durable disconnect support for the utxo_apply Job: this file records
 * per-block inverse deltas and replays them when a stage-side reorg rewinds
 * abandoned branch rows. */

#include "platform/time_compat.h"
#include "jobs/utxo_apply_delta.h"

#include "chain/chain.h"
#include "core/uint256.h"
#include "event/event.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "storage/utxo_projection.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "util/stage.h"
#include "validation/checkpoint.h"
#include "validation/main_constants.h"
#include "validation/main_state.h"

#include <limits.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define STAGE_NAME "utxo_apply"

static int64_t wall_now_s(void)
{
    return (int64_t)platform_time_wall_time_t();
}

/* ── Delta-array lifetime ──────────────────────────────────────────── */

void free_delta_arr(struct delta_entry *arr, size_t n)
{
    if (arr) {
        for (size_t i = 0; i < n; i++)
            free(arr[i].script_owned);
    }
    free(arr);
}

void free_delta(struct delta_summary *s)
{
    free_delta_arr(s->spent, s->spent_count);
    free_delta_arr(s->added, s->added_count);
    s->spent = NULL;
    s->added = NULL;
}

/* ── Block-delta construction ──────────────────────────────────────── */

#define MAX_MONEY_ZAT 2100000000000000LL

static void failure_detail_set(uint8_t out[36], const struct uint256 *txid,
                               uint32_t vout)
{
    memset(out, 0, 36);
    if (txid) memcpy(out, txid->data, 32);
    out[32] = (uint8_t)(vout & 0xff);
    out[33] = (uint8_t)((vout >> 8) & 0xff);
    out[34] = (uint8_t)((vout >> 16) & 0xff);
    out[35] = (uint8_t)((vout >> 24) & 0xff);
}

static bool lookup_added(const struct delta_entry *added, size_t n,
                         const struct uint256 *txid, uint32_t vout,
                         const struct delta_entry **out_entry)
{
    for (size_t i = 0; i < n; i++) {
        if (added[i].vout == vout && uint256_eq(&added[i].txid, txid)) {
            if (out_entry) *out_entry = &added[i];
            return true;
        }
    }
    return false;
}

static void delta_summary_init(struct delta_summary *s)
{
    memset(s, 0, sizeof(*s));
    s->ok = true;
    s->status = "verified";
}

static void delta_fail(struct delta_summary *s, const char *status,
                       const char *kind, const struct uint256 *txid,
                       uint32_t vout)
{
    s->ok = false;
    s->status = status;
    s->failure_kind = kind;
    failure_detail_set(s->failure_detail, txid, vout);
}

void utxo_apply_compute_block_delta(const struct block *blk,
                                    uint32_t block_height,
                                    utxo_apply_lookup_fn lookup,
                                    void *lookup_user,
                                    struct delta_summary *out)
{
    delta_summary_init(out);
    if (!blk) {
        out->ok = false;
        out->status = "internal_error";
        out->failure_kind = "missing_block";
        return;
    }

    size_t spend_cap = 0, add_cap = 0;
    for (size_t ti = 0; ti < blk->num_vtx; ti++) {
        const struct transaction *tx = &blk->vtx[ti];
        if (!transaction_is_coinbase(tx))
            spend_cap += tx->num_vin;
        add_cap += tx->num_vout;
    }

    struct delta_entry *spent = spend_cap
        ? zcl_calloc(spend_cap, sizeof(*spent), "utxo_apply_spent")
        : NULL;
    struct delta_entry *added = add_cap
        ? zcl_calloc(add_cap, sizeof(*added), "utxo_apply_added")
        : NULL;
    if ((spend_cap && !spent) || (add_cap && !added)) {
        out->ok = false;
        out->status = "internal_error";
        out->failure_kind = "alloc";
        free_delta_arr(spent, 0);
        free_delta_arr(added, 0);
        return;
    }
    /* Hand the arrays to `out` up front: every failure path below calls
     * free_delta(out), which frees the (possibly partly-filled) arrays
     * AND any owned restore-scripts up to the running counts. */
    out->spent = spent;
    out->added = added;

    for (size_t ti = 0; ti < blk->num_vtx; ti++) {
        const struct transaction *tx = &blk->vtx[ti];
        int64_t tx_input_value = 0;
        int64_t tx_output_value = 0;

        if (!transaction_is_coinbase(tx)) {
            for (size_t vi = 0; vi < tx->num_vin; vi++) {
                const struct outpoint *op = &tx->vin[vi].prevout;
                int64_t value = 0;
                /* Full restore pre-image for the spent coin. */
                uint32_t restore_height = 0;
                bool restore_coinbase = false;
                const uint8_t *restore_script = NULL;
                uint32_t restore_script_len = 0;
                const struct delta_entry *intra = NULL;
                bool found = lookup_added(added, out->added_count,
                                          &op->hash, op->n, &intra);
                if (found && intra) {
                    /* Created earlier in THIS block: its pre-image is the
                     * added entry we just recorded (height = this block). */
                    value = intra->value;
                    restore_height = block_height;
                    restore_coinbase = intra->is_coinbase;
                    restore_script = intra->script;
                    restore_script_len = intra->script_len;
                }
                if (!found) {
                    struct utxo_apply_lookup lk;
                    memset(&lk, 0, sizeof(lk));
                    if (lookup && !lookup(&op->hash, op->n, &lk, lookup_user)) {
                        out->ok = false;
                        out->status = "internal_error";
                        out->failure_kind = "lookup";
                        free_delta(out);
                        return;
                    }
                    found = lk.found;
                    value = lk.value;
                    restore_height = lk.height;
                    restore_coinbase = lk.is_coinbase;
                    restore_script = lk.script_len ? lk.script : NULL;
                    restore_script_len = lk.script_len;
                }
                if (!found) {
                    delta_fail(out, "spend_unknown_utxo",
                               "spend_unknown_utxo", &op->hash, op->n);
                    free_delta(out);
                    return;
                }
                if (value < 0 || value > MAX_MONEY_ZAT) {
                    delta_fail(out, "value_overflow",
                               "input_value", &op->hash, op->n);
                    free_delta(out);
                    return;
                }
                struct delta_entry *se = &spent[out->spent_count];
                se->txid = op->hash;
                se->vout = op->n;
                se->value = value;
                se->height = restore_height;
                se->is_coinbase = restore_coinbase;
                /* Own a copy of the restore script: the lookup buffer is
                 * stack-scoped and the intra-block add aliases the live
                 * block, both gone by disconnect time. */
                if (restore_script && restore_script_len) {
                    /* A UTXO scriptPubKey is consensus-bounded to
                     * MAX_SCRIPT_SIZE (== UTXO_APPLY_SCRIPT_MAX), the exact
                     * size of the stack lookup buffer restore_script points
                     * into. Reject rather than over-read that fixed buffer
                     * (or persist a truncated script that would corrupt the
                     * restored coin) if a lookup provider ever violates the
                     * contract. Unreachable with valid chain data. */
                    if (restore_script_len > UTXO_APPLY_SCRIPT_MAX) {
                        out->ok = false;
                        out->status = "internal_error";
                        out->failure_kind = "script_too_large";
                        free_delta(out);
                        return;
                    }
                    se->script_owned =
                        zcl_malloc(restore_script_len, "utxo_apply_restore_sc");
                    if (!se->script_owned) {
                        out->ok = false;
                        out->status = "internal_error";
                        out->failure_kind = "alloc";
                        free_delta(out);
                        return;
                    }
                    memcpy(se->script_owned, restore_script, restore_script_len);
                    se->script = se->script_owned;
                    se->script_len = restore_script_len;
                } else {
                    se->script = NULL;
                    se->script_len = 0;
                }
                out->spent_count++;
                out->total_value_delta -= value;
                tx_input_value += value;
            }
        }

        for (size_t vo = 0; vo < tx->num_vout; vo++) {
            const struct tx_out *txo = &tx->vout[vo];
            if (tx_out_is_null(txo))
                continue;
            if (txo->value < 0 || txo->value > MAX_MONEY_ZAT) {
                delta_fail(out, "value_overflow", "output_value",
                           &tx->hash, (uint32_t)vo);
                free_delta(out);
                return;
            }
            if (lookup_added(added, out->added_count, &tx->hash,
                             (uint32_t)vo, NULL)) {
                delta_fail(out, "utxo_collision", "duplicate_output",
                           &tx->hash, (uint32_t)vo);
                free_delta(out);
                return;
            }
            struct utxo_apply_lookup lk;
            memset(&lk, 0, sizeof(lk));
            if (lookup && !lookup(&tx->hash, (uint32_t)vo, &lk, lookup_user)) {
                out->ok = false;
                out->status = "internal_error";
                out->failure_kind = "lookup";
                free_delta(out);
                return;
            }
            if (lk.found) {
                delta_fail(out, "utxo_collision", "utxo_collision",
                           &tx->hash, (uint32_t)vo);
                free_delta(out);
                return;
            }
            added[out->added_count].txid = tx->hash;
            added[out->added_count].vout = (uint32_t)vo;
            added[out->added_count].value = txo->value;
            added[out->added_count].script = txo->script_pub_key.data;
            added[out->added_count].script_len =
                (uint32_t)txo->script_pub_key.size;
            added[out->added_count].is_coinbase = transaction_is_coinbase(tx);
            out->added_count++;
            out->total_value_delta += txo->value;
            tx_output_value += txo->value;
        }

        if (!transaction_is_coinbase(tx) && tx_output_value > tx_input_value) {
            delta_fail(out, "value_overflow", "outputs_exceed_inputs",
                       &tx->hash, 0);
            free_delta(out);
            return;
        }
    }

    if (out->total_value_delta > MAX_MONEY_ZAT ||
        out->total_value_delta < -MAX_MONEY_ZAT) {
        out->ok = false;
        out->status = "value_overflow";
        out->failure_kind = "total_value_delta";
        free_delta(out);
        return;
    }

    /* Success: `out->spent` / `out->added` already own the arrays; the
     * caller emits the events (if authoritative), persists the delta,
     * then frees via free_delta(out). */
}

/* ── Schema ────────────────────────────────────────────────────────── */

/* Per-block inverse-delta record. One row per applied height carries the OLD branch
 * hash (so a same-height fork is distinguishable) plus the spent[] and
 * added[] blobs needed to emit inverse EV_UTXO_* events on a disconnect:
 *   spent_blob: per restored coin  txid|vout|value|height|is_coinbase|
 *                                  script_len|script  (everything ADD needs)
 *   added_blob: per erased coin    txid|vout            (everything SPEND needs)
 * Pruned below the finality floor in step_apply so it stays bounded. */
bool utxo_apply_ensure_delta_schema(sqlite3 *db)
{
    static const char *const sql =
        "CREATE TABLE IF NOT EXISTS utxo_apply_delta ("
        "  height       INTEGER PRIMARY KEY,"
        "  branch_hash  BLOB    NOT NULL,"
        "  spent_blob   BLOB    NOT NULL,"
        "  added_blob   BLOB    NOT NULL"
        ")";
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        LOG_WARN("utxo_apply", "[utxo_apply] delta schema ensure failed: %s", err ? err : "(no message)");
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}

/* ── Inverse-delta (de)serialization ──────────────────────────────────
 * Little-endian, length-prefixed. Wire layout matches the load below; it
 * is internal to this process (progress.db is local) so no versioning. */

static void blob_put_u32(uint8_t **p, uint32_t v)
{
    (*p)[0] = (uint8_t)(v & 0xff);
    (*p)[1] = (uint8_t)((v >> 8) & 0xff);
    (*p)[2] = (uint8_t)((v >> 16) & 0xff);
    (*p)[3] = (uint8_t)((v >> 24) & 0xff);
    *p += 4;
}

static void blob_put_i64(uint8_t **p, int64_t v)
{
    uint64_t u = (uint64_t)v;
    for (int i = 0; i < 8; i++) (*p)[i] = (uint8_t)((u >> (8 * i)) & 0xff);
    *p += 8;
}

static bool blob_get_u32(const uint8_t **p, const uint8_t *end, uint32_t *out)
{
    if (*p + 4 > end) return false;
    *out = (uint32_t)(*p)[0] | ((uint32_t)(*p)[1] << 8) |
           ((uint32_t)(*p)[2] << 16) | ((uint32_t)(*p)[3] << 24);
    *p += 4;
    return true;
}

static bool blob_get_i64(const uint8_t **p, const uint8_t *end, int64_t *out)
{
    if (*p + 8 > end) return false;
    uint64_t u = 0;
    for (int i = 0; i < 8; i++) u |= (uint64_t)(*p)[i] << (8 * i);
    *out = (int64_t)u;
    *p += 8;
    return true;
}

/* spent entry: 32(txid)+4(vout)+8(value)+4(height)+1(coinbase)+4(slen)+slen */
static size_t spent_entry_size(const struct delta_entry *e)
{
    return 32 + 4 + 8 + 4 + 1 + 4 + (size_t)e->script_len;
}

static uint8_t *serialize_spent(const struct delta_summary *s, size_t *out_len)
{
    size_t total = 0;
    for (size_t i = 0; i < s->spent_count; i++)
        total += spent_entry_size(&s->spent[i]);
    uint8_t *buf = zcl_malloc(total ? total : 1, "utxo_apply_spent_blob");
    if (!buf) { *out_len = 0; return NULL; }
    uint8_t *p = buf;
    for (size_t i = 0; i < s->spent_count; i++) {
        const struct delta_entry *e = &s->spent[i];
        memcpy(p, e->txid.data, 32); p += 32;
        blob_put_u32(&p, e->vout);
        blob_put_i64(&p, e->value);
        blob_put_u32(&p, e->height);
        *p++ = e->is_coinbase ? 1 : 0;
        blob_put_u32(&p, e->script_len);
        if (e->script_len) { memcpy(p, e->script, e->script_len); p += e->script_len; }
    }
    *out_len = total;
    return buf;
}

/* added entry: 32(txid)+4(vout) — SPEND needs only the outpoint. */
static uint8_t *serialize_added(const struct delta_summary *s, size_t *out_len)
{
    size_t total = s->added_count * (size_t)(32 + 4);
    uint8_t *buf = zcl_malloc(total ? total : 1, "utxo_apply_added_blob");
    if (!buf) { *out_len = 0; return NULL; }
    uint8_t *p = buf;
    for (size_t i = 0; i < s->added_count; i++) {
        memcpy(p, s->added[i].txid.data, 32); p += 32;
        blob_put_u32(&p, s->added[i].vout);
    }
    *out_len = total;
    return buf;
}

bool utxo_apply_delta_persist(sqlite3 *db, int height,
                              const struct uint256 *branch_hash,
                              const struct delta_summary *s)
{
    size_t spent_len = 0, added_len = 0;
    uint8_t *spent_blob = serialize_spent(s, &spent_len);
    uint8_t *added_blob = serialize_added(s, &added_len);
    if (!spent_blob || !added_blob) {
        free(spent_blob); free(added_blob);
        LOG_WARN("utxo_apply", "[utxo_apply] delta serialize alloc failed h=%d", height);
        return false;
    }

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO utxo_apply_delta "
        "(height, branch_hash, spent_blob, added_blob) VALUES (?,?,?,?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        LOG_WARN("utxo_apply", "[utxo_apply] delta prepare failed: %s", sqlite3_errmsg(db));
        free(spent_blob); free(added_blob);
        return false;
    }
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)height);
    sqlite3_bind_blob (stmt, 2, branch_hash ? branch_hash->data : NULL,
                       branch_hash ? 32 : 0, SQLITE_STATIC);
    sqlite3_bind_blob (stmt, 3, spent_blob, (int)spent_len, SQLITE_STATIC);
    sqlite3_bind_blob (stmt, 4, added_blob, (int)added_len, SQLITE_STATIC);
    rc = sqlite3_step(stmt);  // raw-sql-ok:kernel-primitive
    sqlite3_finalize(stmt);
    free(spent_blob); free(added_blob);
    if (rc != SQLITE_DONE) {
        LOG_WARN("utxo_apply", "[utxo_apply] delta insert h=%d rc=%d", height, rc);
        return false;
    }
    return true;
}

/* ── Reorg unwind ──────────────────────────────────────────────────────
 *
 * The stage-side disconnect path. Structurally mirrors tip_finalize's
 * rewind_cursor_if_active_chain_reorged: detect that the active chain
 * has diverged from what we applied, walk DOWN to the fork point, emit
 * inverse UTXO events for the abandoned blocks (the exact inverse of the
 * former disconnect block path: restored spent coin -> ADD, erased created
 * coin -> SPEND), delete the now-invalid log+delta rows, and rewind the
 * stage cursor to the fork boundary so step_apply re-applies the winning
 * branch forward. Bounded by ZCL_FINALITY_DEPTH (legacy's floor). */

static uint64_t cursor_persisted(sqlite3 *db, const char *name)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT cursor FROM stage_cursor WHERE name = ?",
        -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("utxo_apply", "[utxo_apply] unwind cursor read prepare failed: %s", sqlite3_errmsg(db));
        return 0;
    }
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    uint64_t out = 0;
    if (sqlite3_step(st) == SQLITE_ROW)  // raw-sql-ok:kernel-primitive
        out = (uint64_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return out;
}

/* Load the persisted branch_hash for a delta row. Returns 1 if found
 * (out filled), 0 if absent, -1 on error. */
static int delta_branch_hash_at(sqlite3 *db, int height, struct uint256 *out)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT branch_hash FROM utxo_apply_delta WHERE height = ?",
        -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("utxo_apply", "[utxo_apply] delta branch_hash prepare failed: %s", sqlite3_errmsg(db));
        return -1;  // raw-return-ok:logged-above
    }
    sqlite3_bind_int(st, 1, height);
    int found = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {  // raw-sql-ok:kernel-primitive
        const void *blob = sqlite3_column_blob(st, 0);
        int n = sqlite3_column_bytes(st, 0);
        if (blob && n == 32) {
            memcpy(out->data, blob, 32);
            found = 1;
        }
    }
    sqlite3_finalize(st);
    return found;
}

/* Emit the inverse events for one persisted delta row, in disconnect
 * order: SPEND every created output FIRST, then ADD back every spent
 * coin (mirrors disconnect_block's per-tx reverse walk so even the
 * intermediate projection state matches legacy byte-for-byte). The
 * projection is a set, so final state is order-independent, but legacy
 * order is kept for auditability. Returns false on a malformed blob. */
static bool emit_inverse_delta(sqlite3 *db, int height)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT spent_blob, added_blob FROM utxo_apply_delta WHERE height = ?",
        -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("utxo_apply", "[utxo_apply] inverse delta prepare failed: %s", sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int(st, 1, height);
    if (sqlite3_step(st) != SQLITE_ROW) {  // raw-sql-ok:kernel-primitive
        /* No delta row for a height we believe we applied: corruption or
         * a failure row (which we never persist a delta for). Either way
         * there is nothing to invert at this height. */
        sqlite3_finalize(st);
        return true;
    }
    const uint8_t *spent = sqlite3_column_blob(st, 0);
    int spent_n = sqlite3_column_bytes(st, 0);
    const uint8_t *added = sqlite3_column_blob(st, 1);
    int added_n = sqlite3_column_bytes(st, 1);

    bool ok = true;

    /* Inverse of the forward ADDs: SPEND each created outpoint. */
    {
        const uint8_t *p = added;
        const uint8_t *end = added + (added_n > 0 ? added_n : 0);
        while (p && p + 36 <= end) {
            uint8_t txid[32];
            memcpy(txid, p, 32); p += 32;
            uint32_t vout = 0;
            if (!blob_get_u32(&p, end, &vout)) { ok = false; break; }
            utxo_projection_emit_spend(txid, vout);
        }
    }

    /* Inverse of the forward SPENDs: re-ADD each restored coin with its
     * ORIGINAL value/height/is_coinbase/script (the pre-image we captured
     * at apply time) — exactly disconnect_block's restore→ADD. */
    {
        const uint8_t *p = spent;
        const uint8_t *end = spent + (spent_n > 0 ? spent_n : 0);
        while (p && p < end) {
            if (p + 32 > end) { ok = false; break; }
            uint8_t txid[32];
            memcpy(txid, p, 32); p += 32;
            uint32_t vout = 0, ch = 0, slen = 0;
            int64_t value = 0;
            if (!blob_get_u32(&p, end, &vout)) { ok = false; break; }
            if (!blob_get_i64(&p, end, &value)) { ok = false; break; }
            if (!blob_get_u32(&p, end, &ch)) { ok = false; break; }
            if (p + 1 > end) { ok = false; break; }
            bool is_cb = (*p++ != 0);
            if (!blob_get_u32(&p, end, &slen)) { ok = false; break; }
            const uint8_t *script = NULL;
            if (slen) {
                if (p + slen > end) { ok = false; break; }
                script = p;
                p += slen;
            }
            utxo_projection_emit_add(txid, vout, value, ch, is_cb,
                                     script, slen);
        }
    }

    sqlite3_finalize(st);
    if (!ok)
        LOG_WARN("utxo_apply", "[utxo_apply] malformed inverse delta blob h=%d", height);
    return ok;
}

/* Delete the log + delta rows for heights in [fork_plus1 .. last_h]. */
static bool delete_rows_above(sqlite3 *db, int fork_plus1, int last_h)
{
    sqlite3_stmt *st = NULL;
    for (int pass = 0; pass < 2; pass++) {
        const char *sql = (pass == 0)
            ? "DELETE FROM utxo_apply_log WHERE height >= ? AND height <= ?"
            : "DELETE FROM utxo_apply_delta WHERE height >= ? AND height <= ?";
        if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
            LOG_WARN("utxo_apply", "[utxo_apply] unwind delete prepare failed: %s", sqlite3_errmsg(db));
            return false;
        }
        sqlite3_bind_int(st, 1, fork_plus1);
        sqlite3_bind_int(st, 2, last_h);
        int rc = sqlite3_step(st);  // raw-sql-ok:kernel-primitive
        sqlite3_finalize(st);
        st = NULL;
        if (rc != SQLITE_DONE) {
            LOG_WARN("utxo_apply", "[utxo_apply] unwind delete rc=%d", rc);
            return false;
        }
    }
    return true;
}

/* UPSERT the stage cursor row inside the caller's transaction (mirrors
 * stage.c's cursor_write_locked, which is static). */
static bool unwind_write_cursor(sqlite3 *db, uint64_t value)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "INSERT INTO stage_cursor (name, cursor, updated_at) "
        "VALUES (?1, ?2, ?3) "
        "ON CONFLICT(name) DO UPDATE SET "
        "  cursor = excluded.cursor, updated_at = excluded.updated_at",
        -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("utxo_apply", "[utxo_apply] unwind cursor prepare failed: %s", sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_text (st, 1, STAGE_NAME, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)value);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)wall_now_s());
    int rc = sqlite3_step(st);  // raw-sql-ok:kernel-primitive
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        LOG_WARN("utxo_apply", "[utxo_apply] unwind cursor step rc=%d", rc);
        return false;
    }
    return true;
}

bool utxo_apply_reorg_unwind_if_needed(sqlite3 *db,
                                       struct stage *stage,
                                       struct main_state *ms,
                                       _Atomic uint64_t *unwound_counter,
                                       _Atomic int64_t *last_blocked_unix)
{
    if (!stage || !ms)
        return true;

    uint64_t cursor = cursor_persisted(db, STAGE_NAME);
    if (cursor == 0)
        return true;
    if (cursor > (uint64_t)INT32_MAX) {
        LOG_WARN("utxo_apply", "[utxo_apply] reorg cursor too large: %llu", (unsigned long long)cursor);
        return false;
    }
    int C = (int)cursor;  /* next height to apply; [0, C) already applied */

    /* DRIVER vs FOLLOWER. Under UTXO_AUTHOR_STAGE the stage is itself the
     * tip authority (tip_finalize sets the in-mem chain[] — design step 1),
     * so the active chain at C-1 reflects the stage's OWN tip, not a tip the
     * old engine already swapped ahead of us. Under the default
     * UTXO_AUTHOR_LEGACY the stage is a FOLLOWER: it re-converges onto a
     * chain[] that an external authority drove, and must WAIT (no-op) until
     * that authority has populated C-1. The driver flag selects which
     * discipline applies; the divergence detection + fork walk below are
     * identical in both. */

    /* Compare the OLD branch hash recorded for the highest applied height
     * (C-1) against the block now occupying that height on the active
     * chain. A mismatch is the divergence signal: the winning branch now
     * occupies C-1 (set by the stage itself in driver mode, or by the live
     * driver in follower mode); our delta rows recorded the losing branch. */
    struct uint256 recorded;
    int have = delta_branch_hash_at(db, C - 1, &recorded);
    if (have < 0)
        return false;
    if (have == 0)
        return true;  /* no delta at C-1 (e.g. all-failure tail) → nothing to do */

    struct block_index *active = active_chain_at(&ms->chain_active, C - 1);
    if (!active || !active->phashBlock) {
        /* Chain shorter than our cursor at C-1. In FOLLOWER mode this means
         * legacy has not yet driven the tip to C-1: wait, don't unwind. In
         * DRIVER mode the stage owns the tip, so there is simply no fork to
         * disconnect here yet — also a no-op. Either way: nothing to do. */
        return true;
    }
    if (uint256_eq(&recorded, active->phashBlock))
        return true;  /* no divergence */

    /* Walk DOWN to the fork point F: the highest height where our recorded
     * branch_hash still matches the active chain. Disconnect (F, C-1]. */
    int fork = -1;
    for (int h = C - 2; h >= 0; h--) {
        struct uint256 rec_h;
        int hv = delta_branch_hash_at(db, h, &rec_h);
        if (hv < 0) return false;
        struct block_index *act_h = active_chain_at(&ms->chain_active, h);
        if (hv == 1 && act_h && act_h->phashBlock &&
            uint256_eq(&rec_h, act_h->phashBlock)) {
            fork = h;
            break;
        }
    }
    int fork_plus1 = fork + 1;  /* first height to disconnect (== 0 if F<0) */

    /* Finality-depth floor: never unwind below tip - ZCL_FINALITY_DEPTH,
     * exactly as the single-engine path refused. The deepest
     * disconnected block is at fork_plus1, whose fork point is `fork`;
     * the reorg depth is (C-1) - fork. This reorg_is_allowed(C-1, fork)
     * check is the SOLE gate on whether the unwind proceeds.
     *
     * Why (C-1), the stage cursor, is the correct depth reference:
     *
     *   DRIVER mode (UTXO_AUTHOR_STAGE): the stage drives the tip, so C-1
     *   IS the authoritative tip height and reorg_is_allowed(C-1, fork) is
     *   the exact finality check — the same one legacy applies at
     *   tip->nHeight. The unwind is gated purely on this; there is no
     *   "wait for legacy to reach C-1" precondition (the stage cannot wait
     *   on an engine it has replaced).
     *
     *   FOLLOWER mode (UTXO_AUTHOR_LEGACY, the default): the unwind only
     *   re-converges the stage's OWN applied range [fork+1, C-1] onto a
     *   chain[] that the prior path already reorged and finality-gated
     *   (reorg_is_allowed at tip->nHeight), so the
     *   active chain never presents a reorg deeper than ZCL_FINALITY_DEPTH
     *   for the stage to follow. Measuring from C-1 is a defensive backstop
     *   equivalent to legacy's check; using the global active tip instead
     *   would WEDGE a lagging stage on a side branch. */
    {
        const char *reason = NULL;
        if (!reorg_is_allowed(C - 1, fork, &reason)) {
            LOG_WARN("utxo_apply",
                "[utxo_apply] reorg unwind refused below finality floor "
                "tip=%d fork=%d depth=%d reason=%s (ZCL_FINALITY_DEPTH=%d)",
                C - 1, fork, (C - 1) - fork, reason ? reason : "(null)",
                ZCL_FINALITY_DEPTH);
            event_emitf(EV_BLOCK_REJECTED, 0,
                        "utxo_apply unwind_below_finality tip=%d fork=%d depth=%d",
                        C - 1, fork, (C - 1) - fork);
            return false;
        }
    }

    /* Emit inverse events for h = C-1 down to fork_plus1 (REVERSE, the
     * disconnect order). Only the configured projection author may unwind
     * the projection. */
    if (utxo_projection_get_author() == UTXO_AUTHOR_STAGE) {
        for (int h = C - 1; h >= fork_plus1; h--) {
            if (!emit_inverse_delta(db, h))
                return false;
        }
    }

    /* Atomically drop the abandoned rows and rewind the cursor to
     * fork_plus1 (the first height step_apply re-applies on the winner).
     * The cursor row write + the row deletes share ONE txn so a crash
     * cannot leave a stale cursor pointing past deleted delta rows. The
     * in-memory s->cursor is reloaded from this DB row at the top of the
     * next stage_run_once (cursor_read), so we don't touch it here. */
    char *err = NULL;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK) {
        LOG_WARN("utxo_apply", "[utxo_apply] unwind BEGIN failed: %s", err ? err : "(no message)");
        if (err) sqlite3_free(err);
        return false;
    }
    if (!delete_rows_above(db, fork_plus1, C - 1)) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return false;
    }
    if (!unwind_write_cursor(db, (uint64_t)fork_plus1)) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return false;
    }
    if (sqlite3_exec(db, "COMMIT", NULL, NULL, &err) != SQLITE_OK) {
        LOG_WARN("utxo_apply", "[utxo_apply] unwind COMMIT failed: %s", err ? err : "(no message)");
        if (err) sqlite3_free(err);
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return false;
    }

    atomic_fetch_add(unwound_counter, 1);
    atomic_store(last_blocked_unix, wall_now_s());
    event_emitf(EV_BLOCK_REJECTED, 0,
                "utxo_apply reorg_unwind from=%d to=%d depth=%d",
                C, fork_plus1, (C - 1) - fork);
    LOG_INFO("utxo_apply", "[utxo_apply] reorg unwind from=%d to=%d depth=%d",
             C, fork_plus1, (C - 1) - fork);
    return true;
}
