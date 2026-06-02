/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "jobs/stage_repair.h"

#include "core/uint256.h"
#include "platform/time_compat.h"
#include "primitives/block.h"
#include "storage/progress_store.h"
#include "util/log_macros.h"
#include "util/stage.h"

#include <sqlite3.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SOLUTIONLESS_REASON "no-header-solution-backfill-required"

struct validate_row {
    bool found;
    int ok;
    char fail_reason[96];
};

static bool ensure_header_solution_schema(sqlite3 *db)
{
    static const char *const sql =
        "CREATE TABLE IF NOT EXISTS header_solution_repair ("
        "  height INTEGER PRIMARY KEY,"
        "  hash BLOB NOT NULL,"
        "  version INTEGER NOT NULL,"
        "  prev_hash BLOB NOT NULL,"
        "  merkle_root BLOB NOT NULL,"
        "  final_sapling_root BLOB NOT NULL,"
        "  n_time INTEGER NOT NULL,"
        "  n_bits INTEGER NOT NULL,"
        "  nonce BLOB NOT NULL,"
        "  solution BLOB NOT NULL,"
        "  solution_len INTEGER NOT NULL,"
        "  saved_at INTEGER NOT NULL"
        ")";
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] repair-header schema failed: %s",
                 err ? err : "(no message)");
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}

bool stage_repair_header_solution_save(sqlite3 *db, int height,
                                       const struct uint256 *hash,
                                       const struct block_header *header)
{
    if (!db || height < 0 || !hash || !header ||
        header->nSolutionSize == 0 ||
        header->nSolutionSize > sizeof(header->nSolution))
        LOG_FAIL("stage_repair", "header solution save invalid args");

    struct uint256 computed;
    block_header_get_hash(header, &computed);
    if (!uint256_eq(&computed, hash)) {
        LOG_WARN("stage_repair",
                 "[stage_repair] repair-header hash mismatch h=%d", height);
        return false;
    }

    progress_store_tx_lock();
    if (!ensure_header_solution_schema(db)) {
        progress_store_tx_unlock();
        return false;
    }

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO header_solution_repair "
            "(height,hash,version,prev_hash,merkle_root,final_sapling_root,"
            "n_time,n_bits,nonce,solution,solution_len,saved_at) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
            -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] repair-header prepare failed: %s",
                 sqlite3_errmsg(db));
        progress_store_tx_unlock();
        return false;
    }
    sqlite3_bind_int(st, 1, height);
    sqlite3_bind_blob(st, 2, hash->data, 32, SQLITE_STATIC);
    sqlite3_bind_int(st, 3, header->nVersion);
    sqlite3_bind_blob(st, 4, header->hashPrevBlock.data, 32, SQLITE_STATIC);
    sqlite3_bind_blob(st, 5, header->hashMerkleRoot.data, 32, SQLITE_STATIC);
    sqlite3_bind_blob(st, 6, header->hashFinalSaplingRoot.data, 32,
                      SQLITE_STATIC);
    sqlite3_bind_int64(st, 7, (sqlite3_int64)header->nTime);
    sqlite3_bind_int64(st, 8, (sqlite3_int64)header->nBits);
    sqlite3_bind_blob(st, 9, header->nNonce.data, 32, SQLITE_STATIC);
    sqlite3_bind_blob(st, 10, header->nSolution,
                      (int)header->nSolutionSize, SQLITE_STATIC);
    sqlite3_bind_int64(st, 11, (sqlite3_int64)header->nSolutionSize);
    sqlite3_bind_int64(st, 12,
                       (sqlite3_int64)platform_time_wall_unix());
    int rc = sqlite3_step(st);  // raw-sql-ok:stage-repair-kernel
    sqlite3_finalize(st);
    progress_store_tx_unlock();
    if (rc != SQLITE_DONE) {
        LOG_WARN("stage_repair",
                 "[stage_repair] repair-header step failed h=%d rc=%d: %s",
                 height, rc, sqlite3_errmsg(db));
        return false;
    }
    return true;
}

bool stage_repair_header_solution_load(sqlite3 *db, int height,
                                       const struct uint256 *expected_hash,
                                       struct block_header *out)
{
    if (!db || height < 0 || !out)
        return false;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT hash,version,prev_hash,merkle_root,final_sapling_root,"
            "n_time,n_bits,nonce,solution,solution_len "
            "FROM header_solution_repair WHERE height=?",
            -1, &st, NULL) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int(st, 1, height);
    int rc = sqlite3_step(st);  // raw-sql-ok:stage-repair-kernel
    if (rc == SQLITE_DONE) {
        sqlite3_finalize(st);
        return false;
    }
    if (rc != SQLITE_ROW) {
        LOG_WARN("stage_repair",
                 "[stage_repair] repair-header load step failed h=%d rc=%d: %s",
                 height, rc, sqlite3_errmsg(db));
        sqlite3_finalize(st);
        return false;
    }

    const void *hash_blob = sqlite3_column_blob(st, 0);
    const void *prev_blob = sqlite3_column_blob(st, 2);
    const void *merkle_blob = sqlite3_column_blob(st, 3);
    const void *sapling_blob = sqlite3_column_blob(st, 4);
    const void *nonce_blob = sqlite3_column_blob(st, 7);
    const void *solution_blob = sqlite3_column_blob(st, 8);
    int solution_bytes = sqlite3_column_bytes(st, 8);
    int64_t solution_len = sqlite3_column_int64(st, 9);
    if (!hash_blob || !prev_blob || !merkle_blob || !sapling_blob ||
        !nonce_blob || !solution_blob ||
        sqlite3_column_bytes(st, 0) != 32 ||
        sqlite3_column_bytes(st, 2) != 32 ||
        sqlite3_column_bytes(st, 3) != 32 ||
        sqlite3_column_bytes(st, 4) != 32 ||
        sqlite3_column_bytes(st, 7) != 32 ||
        solution_len <= 0 || solution_len > MAX_SOLUTION_SIZE ||
        solution_bytes != (int)solution_len) {
        sqlite3_finalize(st);
        return false;
    }

    struct uint256 stored_hash;
    memcpy(stored_hash.data, hash_blob, 32);
    if (expected_hash && !uint256_eq(&stored_hash, expected_hash)) {
        sqlite3_finalize(st);
        return false;
    }

    block_header_init(out);
    out->nVersion = sqlite3_column_int(st, 1);
    memcpy(out->hashPrevBlock.data, prev_blob, 32);
    memcpy(out->hashMerkleRoot.data, merkle_blob, 32);
    memcpy(out->hashFinalSaplingRoot.data, sapling_blob, 32);
    out->nTime = (uint32_t)sqlite3_column_int64(st, 5);
    out->nBits = (uint32_t)sqlite3_column_int64(st, 6);
    memcpy(out->nNonce.data, nonce_blob, 32);
    memcpy(out->nSolution, solution_blob, (size_t)solution_len);
    out->nSolutionSize = (size_t)solution_len;
    sqlite3_finalize(st);

    struct uint256 computed;
    block_header_get_hash(out, &computed);
    return uint256_eq(&computed, &stored_hash);
}

bool stage_repair_header_solution_available(sqlite3 *db, int height)
{
    if (!db || height < 0)
        return false;
    progress_store_tx_lock();
    bool ok = ensure_header_solution_schema(db) &&
              stage_repair_header_solution_load(db, height, NULL,
                                                &(struct block_header){0});
    progress_store_tx_unlock();
    return ok;
}

static bool read_validate_row(sqlite3 *db, int height,
                              struct validate_row *out)
{
    memset(out, 0, sizeof(*out));
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT ok, COALESCE(fail_reason,'') "
            "FROM validate_headers_log WHERE height = ?",
            -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] validate row prepare failed: %s",
                 sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int(st, 1, height);
    int rc = sqlite3_step(st);  // raw-sql-ok:stage-repair-kernel
    if (rc == SQLITE_ROW) {
        out->found = true;
        out->ok = sqlite3_column_int(st, 0);
        const unsigned char *txt = sqlite3_column_text(st, 1);
        if (txt)
            snprintf(out->fail_reason, sizeof(out->fail_reason),
                     "%s", (const char *)txt);
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("stage_repair",
                 "[stage_repair] validate row step failed rc=%d: %s",
                 rc, sqlite3_errmsg(db));
        sqlite3_finalize(st);
        return false;
    }
    sqlite3_finalize(st);
    return true;
}

static bool cursor_at_unlocked(sqlite3 *db, const char *name, int *out)
{
    *out = -1;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT cursor FROM stage_cursor WHERE name = ?",
            -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] cursor read prepare failed stage=%s: %s",
                 name, sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);  // raw-sql-ok:stage-repair-kernel
    if (rc == SQLITE_ROW) {
        *out = sqlite3_column_int(st, 0);
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("stage_repair",
                 "[stage_repair] cursor read step failed stage=%s rc=%d: %s",
                 name, rc, sqlite3_errmsg(db));
        sqlite3_finalize(st);
        return false;
    }
    sqlite3_finalize(st);
    return true;
}

static bool body_fetch_row_observed_unlocked(sqlite3 *db, int height,
                                             bool *found, bool *observed)
{
    *found = false;
    *observed = false;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT ok FROM body_fetch_log WHERE height = ?",
            -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] body_fetch observed prepare failed: %s",
                 sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int(st, 1, height);
    int rc = sqlite3_step(st);  // raw-sql-ok:stage-repair-kernel
    if (rc == SQLITE_ROW) {
        *found = true;
        *observed = sqlite3_column_int(st, 0) == 1;
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("stage_repair",
                 "[stage_repair] body_fetch observed step failed rc=%d: %s",
                 rc, sqlite3_errmsg(db));
        sqlite3_finalize(st);
        return false;
    }
    sqlite3_finalize(st);
    return true;
}

bool stage_repair_body_fetch_observed(sqlite3 *db, int height)
{
    if (!db || height < 0)
        return false;
    progress_store_tx_lock();
    bool found = false;
    bool observed = false;
    bool ok = body_fetch_row_observed_unlocked(db, height, &found,
                                               &observed);
    progress_store_tx_unlock();
    return ok && found && observed;
}

bool stage_repair_body_fetch_missing_have_data_candidate(
    sqlite3 *db,
    int height,
    struct stage_repair_body_fetch_gap *out)
{
    if (out)
        memset(out, 0, sizeof(*out));
    if (!db || height < 0)
        return false;

    progress_store_tx_lock();
    int validate_cursor = -1;
    int body_fetch_cursor = -1;
    bool ok = cursor_at_unlocked(db, "validate_headers", &validate_cursor) &&
              cursor_at_unlocked(db, "body_fetch", &body_fetch_cursor);
    if (!ok) {
        progress_store_tx_unlock();
        return false;
    }

    struct validate_row vh;
    if (!read_validate_row(db, height, &vh)) {
        progress_store_tx_unlock();
        return false;
    }

    bool body_row_found = false;
    bool body_observed = false;
    if (!body_fetch_row_observed_unlocked(db, height, &body_row_found,
                                          &body_observed)) {
        progress_store_tx_unlock();
        return false;
    }
    progress_store_tx_unlock();

    bool ready = body_fetch_cursor == height &&
                 validate_cursor > height &&
                 vh.found && vh.ok == 1 &&
                 !body_row_found;
    if (out) {
        out->ready = ready;
        out->body_observed = body_row_found && body_observed;
        out->target_height = height;
        out->validate_cursor = validate_cursor;
        out->body_fetch_cursor = body_fetch_cursor;
    }
    return ready;
}

bool stage_repair_body_fetch_missing_have_data_frontier_candidate(
    sqlite3 *db,
    struct stage_repair_body_fetch_gap *out)
{
    if (out)
        memset(out, 0, sizeof(*out));
    if (!db)
        return false;

    progress_store_tx_lock();
    int body_fetch_cursor = -1;
    bool ok = cursor_at_unlocked(db, "body_fetch", &body_fetch_cursor);
    progress_store_tx_unlock();
    if (!ok || body_fetch_cursor < 0)
        return false;

    return stage_repair_body_fetch_missing_have_data_candidate(
        db, body_fetch_cursor, out);
}

static bool body_fetch_skipped_invalid(sqlite3 *db, int height, bool *out)
{
    *out = false;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT source, ok, COALESCE(fail_reason,'') "
            "FROM body_fetch_log WHERE height = ?",
            -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] body_fetch row prepare failed: %s",
                 sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int(st, 1, height);
    int rc = sqlite3_step(st);  // raw-sql-ok:stage-repair-kernel
    if (rc == SQLITE_ROW) {
        const unsigned char *source = sqlite3_column_text(st, 0);
        int ok = sqlite3_column_int(st, 1);
        const unsigned char *reason = sqlite3_column_text(st, 2);
        *out = ok == 0 &&
               source && strcmp((const char *)source, "skipped_invalid") == 0 &&
               reason && strcmp((const char *)reason,
                                "header_validation_failed") == 0;
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("stage_repair",
                 "[stage_repair] body_fetch row step failed rc=%d: %s",
                 rc, sqlite3_errmsg(db));
        sqlite3_finalize(st);
        return false;
    }
    sqlite3_finalize(st);
    return true;
}

static enum stage_repair_header_solution_poison
poison_mode_unlocked(sqlite3 *db, int height)
{
    if (!db || height < 0)
        return STAGE_REPAIR_POISON_NONE;

    struct validate_row vh;
    if (!read_validate_row(db, height, &vh))
        return STAGE_REPAIR_POISON_NONE;

    if (vh.found && vh.ok == 0 &&
        strcmp(vh.fail_reason, SOLUTIONLESS_REASON) == 0)
        return STAGE_REPAIR_POISON_VALIDATE_SOLUTIONLESS;

    bool skipped = false;
    if (!body_fetch_skipped_invalid(db, height, &skipped))
        return STAGE_REPAIR_POISON_NONE;

    if (!skipped)
        return STAGE_REPAIR_POISON_NONE;
    if (vh.found && vh.ok == 1)
        return STAGE_REPAIR_POISON_DOWNSTREAM_STALE;
    if (vh.found && vh.ok == 0 &&
        strcmp(vh.fail_reason, SOLUTIONLESS_REASON) == 0)
        return STAGE_REPAIR_POISON_VALIDATE_SOLUTIONLESS;
    return STAGE_REPAIR_POISON_NONE;
}

enum stage_repair_header_solution_poison
stage_repair_header_solution_poison_mode(sqlite3 *db, int height)
{
    progress_store_tx_lock();
    enum stage_repair_header_solution_poison mode =
        poison_mode_unlocked(db, height);
    progress_store_tx_unlock();
    return mode;
}

bool stage_repair_header_solution_poison_present(sqlite3 *db, int height)
{
    return stage_repair_header_solution_poison_mode(db, height) !=
           STAGE_REPAIR_POISON_NONE;
}

static int delete_from_table(sqlite3 *db, const char *table, int height)
{
    char sql[160];
    snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE height >= ?", table);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] delete prepare failed table=%s: %s",
                 table, sqlite3_errmsg(db));
        return -1;
    }
    sqlite3_bind_int(st, 1, height);
    int rc = sqlite3_step(st);  // raw-sql-ok:stage-repair-kernel
    int changed = sqlite3_changes(db);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        LOG_WARN("stage_repair",
                 "[stage_repair] delete step failed table=%s rc=%d: %s",
                 table, rc, sqlite3_errmsg(db));
        return -1;
    }
    return changed;
}

static bool force_stage_cursor(sqlite3 *db, const char *name, int height)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO stage_cursor(name, cursor, updated_at) "
            "VALUES(?,?,?) "
            "ON CONFLICT(name) DO UPDATE SET "
            "cursor=excluded.cursor, updated_at=excluded.updated_at",
            -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] cursor prepare failed stage=%s: %s",
                 name, sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)height);
    sqlite3_bind_int64(st, 3,
                       (sqlite3_int64)platform_time_wall_unix());
    int rc = sqlite3_step(st);  // raw-sql-ok:stage-repair-kernel
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        LOG_WARN("stage_repair",
                 "[stage_repair] cursor step failed stage=%s rc=%d: %s",
                 name, rc, sqlite3_errmsg(db));
        return false;
    }
    return true;
}

static bool table_has_success_at_or_above(sqlite3 *db, const char *table,
                                          int height, bool *out)
{
    *out = false;
    char sql[160];
    snprintf(sql, sizeof(sql),
             "SELECT 1 FROM %s WHERE height >= ? AND ok = 1 LIMIT 1", table);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] success-check prepare failed table=%s: %s",
                 table, sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int(st, 1, height);
    int rc = sqlite3_step(st);  // raw-sql-ok:stage-repair-kernel
    if (rc == SQLITE_ROW) {
        *out = true;
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("stage_repair",
                 "[stage_repair] success-check step failed table=%s rc=%d: %s",
                 table, rc, sqlite3_errmsg(db));
        sqlite3_finalize(st);
        return false;
    }
    sqlite3_finalize(st);
    return true;
}

bool stage_repair_header_solution_poison_rewind(
    sqlite3 *db,
    int height,
    int active_tip_height,
    struct stage_repair_header_solution_result *out)
{
    if (out)
        memset(out, 0, sizeof(*out));
    if (!db || height < 0 || active_tip_height < -1)
        LOG_FAIL("stage_repair", "header poison rewind invalid args");
    if (height != active_tip_height + 1) {
        LOG_WARN("stage_repair",
                 "[stage_repair] reject non-frontier repair h=%d active_tip=%d",
                 height, active_tip_height);
        return false;
    }
    if (!stage_table_ensure(db))
        return false;

    static const char *const downstream_logs[] = {
        "body_fetch_log",
        "body_persist_log",
        "script_validate_log",
        "proof_validate_log",
        "utxo_apply_log",
        "utxo_apply_delta",
        "tip_finalize_log",
    };
    static const char *const downstream_stages[] = {
        "body_fetch",
        "body_persist",
        "script_validate",
        "proof_validate",
        "utxo_apply",
        "tip_finalize",
    };
    static const char *const success_checked_logs[] = {
        "body_fetch_log",
        "body_persist_log",
        "script_validate_log",
        "proof_validate_log",
        "utxo_apply_log",
        "tip_finalize_log",
    };

    progress_store_tx_lock();
    char *err = NULL;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err) !=
        SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] BEGIN failed: %s",
                 err ? err : "(no message)");
        if (err) sqlite3_free(err);
        progress_store_tx_unlock();
        return false;
    }

    enum stage_repair_header_solution_poison mode =
        poison_mode_unlocked(db, height);
    if (out) {
        out->target_height = height;
        out->mode = mode;
    }
    if (mode == STAGE_REPAIR_POISON_NONE) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        progress_store_tx_unlock();
        return true;
    }

    for (size_t i = 0;
         i < sizeof(success_checked_logs) / sizeof(success_checked_logs[0]);
         i++) {
        bool has_success = false;
        if (!table_has_success_at_or_above(db, success_checked_logs[i],
                                           height, &has_success))
            goto rollback;
        if (has_success) {
            LOG_WARN("stage_repair",
                     "[stage_repair] reject repair h=%d: %s has successful "
                     "rows at/above frontier",
                     height, success_checked_logs[i]);
            goto rollback;
        }
    }

    int deleted = 0;
    int rewound = 0;
    if (mode == STAGE_REPAIR_POISON_VALIDATE_SOLUTIONLESS) {
        int n = delete_from_table(db, "validate_headers_log", height);
        if (n < 0) goto rollback;
        deleted += n;
        if (!force_stage_cursor(db, "validate_headers", height))
            goto rollback;
        rewound++;
    }

    for (size_t i = 0; i < sizeof(downstream_logs) / sizeof(downstream_logs[0]);
         i++) {
        int n = delete_from_table(db, downstream_logs[i], height);
        if (n < 0) goto rollback;
        deleted += n;
    }
    for (size_t i = 0;
         i < sizeof(downstream_stages) / sizeof(downstream_stages[0]); i++) {
        if (!force_stage_cursor(db, downstream_stages[i], height))
            goto rollback;
        rewound++;
    }

    if (sqlite3_exec(db, "COMMIT", NULL, NULL, &err) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] COMMIT failed: %s",
                 err ? err : "(no message)");
        if (err) sqlite3_free(err);
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        progress_store_tx_unlock();
        return false;
    }
    progress_store_tx_unlock();

    if (out) {
        out->repaired = true;
        out->deleted_rows = deleted;
        out->rewound_cursors = rewound;
    }
    LOG_WARN("stage_repair",
             "[stage_repair] header-solution poison repaired h=%d mode=%d "
             "deleted=%d rewound=%d",
             height, mode, deleted, rewound);
    return true;

rollback:
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    progress_store_tx_unlock();
    return false;
}

bool stage_reconcile_clamp_tip_finalize_to_floor(
    sqlite3 *db, int coins_best, struct stage_reconcile_result *out)
{
    if (out)
        memset(out, 0, sizeof(*out));
    if (!db)
        return false;

    /* No durable applied anchor → nothing to floor on; leave the chain alone. */
    if (coins_best < 0)
        return true;

    int floor = coins_best + 1;
    if (out)
        out->floor = floor;

    if (!stage_table_ensure(db))
        return false;

    progress_store_tx_lock();

    int cur = -1;
    if (!cursor_at_unlocked(db, "tip_finalize", &cur)) {
        progress_store_tx_unlock();
        return false;
    }

    /* Only act on the wedge: tip_finalize cursor strictly AHEAD of the applied
     * tip. A cursor at or below the floor is healthy (or behind, which is a
     * different concern) — leave it untouched so we never disturb a node that
     * is finalizing normally. */
    if (cur <= floor) {
        progress_store_tx_unlock();
        return true;   /* no-op, clamped=false */
    }

    char *err = NULL;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] tip_finalize clamp BEGIN failed: %s",
                 err ? err : "(no message)");
        if (err) sqlite3_free(err);
        progress_store_tx_unlock();
        return false;
    }

    /* Clamp ONLY the tip_finalize cursor. No log deletions, no upstream cursor
     * changes — the upstream evidence stays intact so the re-finalize replays
     * it forward, and the surviving tip_finalize_log rows keep the Tier-2
     * public-tip floor at coins_best. */
    if (!force_stage_cursor(db, "tip_finalize", floor)) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        progress_store_tx_unlock();
        return false;
    }

    if (sqlite3_exec(db, "COMMIT", NULL, NULL, &err) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] tip_finalize clamp COMMIT failed: %s",
                 err ? err : "(no message)");
        if (err) sqlite3_free(err);
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        progress_store_tx_unlock();
        return false;
    }
    progress_store_tx_unlock();

    if (out)
        out->clamped = true;
    LOG_WARN("stage_repair",
             "[stage_repair] reducer cursor/coins desync: clamped tip_finalize "
             "cursor %d -> %d (coins_best=%d) so the reducer re-finalizes "
             "forward; no logs deleted, public tip floored at coins_best",
             cur, floor, coins_best);
    return true;
}
