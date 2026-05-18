/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * hodl_history_service — see header.
 *
 * The SQL for one snapshot at height H:
 *   total_zat   = SUM(o.value)               where o is "alive at H"
 *   older_1y_zat = SUM(o.value) where also b.time(o.block_height) <= T_H - 31557600
 *
 * "Alive at H" means o was created on a block ≤ H and not spent on a
 * block ≤ H. tx_inputs holds (prev_txid, prev_vout, block_height) for
 * every spend; we LEFT JOIN to find unspent.
 */

#include "services/hodl_history_service.h"

#include "util/ar_step_readonly.h"
#include "util/log_macros.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define JULIAN_YEAR_SECONDS  ((int64_t)31557600)

static int64_t fq_i64(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *s = NULL;
    int64_t v = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
        if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW)
            v = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
    }
    return v;
}

bool hodl_history_fill_one(sqlite3 *db, int64_t height)
{
    if (!db || height < 1)
        return false;

    /* Resolve block timestamp first. If we don't have the block,
     * the chain hasn't reached this height yet and we can't sample. */
    int64_t block_time = 0;
    {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db, "SELECT time FROM blocks WHERE height = ?",
                               -1, &s, NULL) != SQLITE_OK || !s)
            return false;
        sqlite3_bind_int64(s, 1, height);
        if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW)
            block_time = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
    }
    if (block_time <= 0)
        return false;

    int64_t cutoff_time = block_time - JULIAN_YEAR_SECONDS;

    /* Compute total + older-than-1y in a single pass.
     *   o "alive at H": LEFT JOIN tx_inputs filtered to spends ≤ H,
     *                   keep only rows where no such spend exists.
     *   "older than 1y": creation-block time ≤ block_time - 1y. */
    int64_t total = 0, older = 0;
    {
        const char *sql =
            "SELECT "
            "  COALESCE(SUM(o.value), 0) AS total_zat,"
            "  COALESCE(SUM(CASE WHEN b.time <= ?1 THEN o.value ELSE 0 END), 0) "
            "    AS older_zat "
            "FROM tx_outputs o "
            "JOIN blocks b ON b.height = o.block_height "
            "LEFT JOIN tx_inputs i "
            "  ON i.prev_txid = o.txid AND i.prev_vout = o.vout "
            "     AND i.block_height <= ?2 "
            "WHERE o.block_height <= ?2 AND i.txid IS NULL";
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK || !s) {
            LOG_FAIL("hodl_history",
                     "prepare snapshot SQL failed: %s", sqlite3_errmsg(db));
        }
        sqlite3_bind_int64(s, 1, cutoff_time);
        sqlite3_bind_int64(s, 2, height);
        if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
            total = sqlite3_column_int64(s, 0);
            older = sqlite3_column_int64(s, 1);
        }
        sqlite3_finalize(s);
    }

    /* Clamp older to total — same invariant the CHECK constraint
     * enforces. A small underflow from concurrent writes would only
     * happen during initial sync; pin to the valid range. */
    if (older > total) older = total;
    if (total < 0) total = 0;
    if (older < 0) older = 0;
    double pct = total > 0
        ? (double)older / (double)total * 100.0
        : 0.0;

    sqlite3_stmt *ins = NULL;
    const char *ins_sql =
        "INSERT OR REPLACE INTO hodl_history "
        "(height, time, total_zat, older_1y_zat, older_1y_pct) "
        "VALUES (?1, ?2, ?3, ?4, ?5)";
    if (sqlite3_prepare_v2(db, ins_sql, -1, &ins, NULL) != SQLITE_OK || !ins) {
        LOG_FAIL("hodl_history",
                 "prepare INSERT failed: %s", sqlite3_errmsg(db));
    }
    sqlite3_bind_int64(ins, 1, height);
    sqlite3_bind_int64(ins, 2, block_time);
    sqlite3_bind_int64(ins, 3, total);
    sqlite3_bind_int64(ins, 4, older);
    sqlite3_bind_double(ins, 5, pct);
    int rc = sqlite3_step(ins);  // raw-sql-ok:hodl-history-insert-rc-checked
    sqlite3_finalize(ins);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        LOG_FAIL("hodl_history",
                 "INSERT step rc=%d: %s", rc, sqlite3_errmsg(db));
    }
    return true;
}

int hodl_history_fill_pending(sqlite3 *db, int64_t chain_tip, int max_rows)
{
    if (!db || chain_tip < HODL_HISTORY_SAMPLE_STRIDE || max_rows <= 0)
        return 0;

    int64_t last_filled = fq_i64(db,
        "SELECT COALESCE(MAX(height), 0) FROM hodl_history");

    /* The most recent useful sample is (chain_tip - 1y_blocks) — beyond
     * that "older than 1y" can't be true. We do still sample within the
     * last year so the chart's right edge follows tip. */
    int64_t target = chain_tip - (chain_tip % HODL_HISTORY_SAMPLE_STRIDE);

    int filled = 0;
    int64_t next = last_filled > 0
        ? last_filled + HODL_HISTORY_SAMPLE_STRIDE
        : HODL_HISTORY_SAMPLE_STRIDE;
    while (filled < max_rows && next <= target) {
        if (hodl_history_fill_one(db, next))
            filled++;
        next += HODL_HISTORY_SAMPLE_STRIDE;
    }
    return filled;
}

int hodl_history_load_all(sqlite3 *db, struct hodl_history_row *out,
                          int max_rows)
{
    if (!db || !out || max_rows <= 0)
        return 0;
    const char *sql =
        "SELECT height, time, total_zat, older_1y_zat, older_1y_pct "
        "FROM hodl_history ORDER BY height ASC";
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK || !s)
        return 0;
    int n = 0;
    while (n < max_rows && AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
        out[n].height       = sqlite3_column_int64(s, 0);
        out[n].time         = sqlite3_column_int64(s, 1);
        out[n].total_zat    = sqlite3_column_int64(s, 2);
        out[n].older_1y_zat = sqlite3_column_int64(s, 3);
        out[n].older_1y_pct = sqlite3_column_double(s, 4);
        n++;
    }
    sqlite3_finalize(s);
    return n;
}
