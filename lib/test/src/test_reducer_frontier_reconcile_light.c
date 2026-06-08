/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"

#include "chain/chain.h"
#include "core/arith_uint256.h"
#include "jobs/reducer_frontier.h"
#include "jobs/stage_repair.h"
#include "storage/progress_store.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define RFRL_CHECK(name, expr) do { \
    printf("reducer_frontier_reconcile_light: %s... ", (name)); \
    if ((expr)) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

#define A REDUCER_FRONTIER_TRUSTED_ANCHOR

struct rfrl_fixture {
    char dir[256];
    struct main_state ms;
    struct uint256 hashes[4];
    struct block_index *idx[4];
};

static bool exec_sql(sqlite3 *db, const char *sql)
{
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        printf("SQL failed: %s\n", err ? err : "(no message)");
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}

static bool seed_schema(sqlite3 *db)
{
    return
        exec_sql(db,
            "CREATE TABLE IF NOT EXISTS validate_headers_log ("
            "height INTEGER PRIMARY KEY, hash BLOB NOT NULL, ok INTEGER NOT NULL,"
            "fail_reason TEXT, validated_at INTEGER)") &&
        exec_sql(db,
            "CREATE TABLE IF NOT EXISTS script_validate_log ("
            "height INTEGER PRIMARY KEY, status TEXT, ok INTEGER NOT NULL,"
            "block_hash BLOB)") &&
        exec_sql(db,
            "CREATE TABLE IF NOT EXISTS body_persist_log ("
            "height INTEGER PRIMARY KEY, source TEXT, ok INTEGER NOT NULL)") &&
        exec_sql(db,
            "CREATE TABLE IF NOT EXISTS proof_validate_log ("
            "height INTEGER PRIMARY KEY, status TEXT, ok INTEGER NOT NULL)") &&
        exec_sql(db,
            "CREATE TABLE IF NOT EXISTS utxo_apply_log ("
            "height INTEGER PRIMARY KEY, status TEXT, ok INTEGER NOT NULL)") &&
        exec_sql(db,
            "CREATE TABLE IF NOT EXISTS tip_finalize_log ("
            "height INTEGER PRIMARY KEY, status TEXT, ok INTEGER NOT NULL,"
            "tip_hash BLOB)");
}

static bool seed_cursor(sqlite3 *db, const char *name, int cursor)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO stage_cursor(name,cursor,updated_at) "
            "VALUES(?,?,1)",
            -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 2, cursor);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

static bool seed_all_cursors(sqlite3 *db, int cursor)
{
    return seed_cursor(db, "validate_headers", cursor) &&
           seed_cursor(db, "body_fetch", cursor) &&
           seed_cursor(db, "body_persist", cursor) &&
           seed_cursor(db, "script_validate", cursor) &&
           seed_cursor(db, "proof_validate", cursor) &&
           seed_cursor(db, "utxo_apply", cursor) &&
           seed_cursor(db, "tip_finalize", cursor);
}

static bool put_hash_log(sqlite3 *db, const char *table, const char *hash_col,
                         int height, int ok_flag, const struct uint256 *hash)
{
    char sql[192];
    if (strcmp(table, "validate_headers_log") == 0) {
        snprintf(sql, sizeof(sql),
                 "INSERT OR REPLACE INTO %s(height,ok,%s) VALUES(?,?,?)",
                 table, hash_col);
    } else {
        snprintf(sql, sizeof(sql),
                 "INSERT OR REPLACE INTO %s(height,status,ok,%s) "
                 "VALUES(?,'verified',?,?)",
                 table, hash_col);
    }

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, height);
    sqlite3_bind_int(st, 2, ok_flag);
    sqlite3_bind_blob(st, 3, hash->data, 32, SQLITE_STATIC);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

static bool put_simple_log(sqlite3 *db, const char *table, int height,
                           int ok_flag)
{
    char sql[160];
    if (strcmp(table, "body_persist_log") == 0) {
        snprintf(sql, sizeof(sql),
                 "INSERT OR REPLACE INTO %s(height,source,ok) "
                 "VALUES(?,'fixture',?)",
                 table);
    } else {
        snprintf(sql, sizeof(sql),
                 "INSERT OR REPLACE INTO %s(height,status,ok) "
                 "VALUES(?,'verified',?)",
                 table);
    }

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, height);
    sqlite3_bind_int(st, 2, ok_flag);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

static bool put_tip_log(sqlite3 *db, int height, int ok_flag,
                        const struct uint256 *hash)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO tip_finalize_log"
            "(height,status,ok,tip_hash) VALUES(?,'finalized',?,?)",
            -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, height);
    sqlite3_bind_int(st, 2, ok_flag);
    sqlite3_bind_blob(st, 3, hash->data, 32, SQLITE_STATIC);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

static bool put_upstream_ok(sqlite3 *db, int height,
                            const struct uint256 *hash)
{
    return put_hash_log(db, "validate_headers_log", "hash", height, 1, hash) &&
           put_hash_log(db, "script_validate_log", "block_hash", height, 1,
                        hash) &&
           put_simple_log(db, "body_persist_log", height, 1) &&
           put_simple_log(db, "proof_validate_log", height, 1) &&
           put_simple_log(db, "utxo_apply_log", height, 1);
}

static bool seed_coins_applied(sqlite3 *db, int64_t height)
{
    uint8_t blob[8];
    for (int i = 0; i < 8; i++)
        blob[i] = (uint8_t)((uint64_t)height >> (8 * i));

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO progress_meta(key,value) VALUES(?,?)",
            -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, "coins_applied_height", -1, SQLITE_STATIC);
    sqlite3_bind_blob(st, 2, blob, sizeof(blob), SQLITE_STATIC);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

static struct block_index *insert_index(struct main_state *ms,
                                        struct uint256 *hash,
                                        int height,
                                        struct block_index *prev,
                                        unsigned status)
{
    memset(hash, 0, sizeof(*hash));
    hash->data[0] = (uint8_t)(height & 0xff);
    hash->data[1] = (uint8_t)((height >> 8) & 0xff);
    hash->data[2] = (uint8_t)((height >> 16) & 0xff);
    hash->data[31] = 0x7b;

    struct block_index *bi =
        chainstate_insert_block_index((struct chainstate *)ms, hash);
    if (!bi)
        return NULL;
    bi->nHeight = height;
    bi->pprev = prev;
    bi->nStatus = status;
    bi->nFile = -1;
    bi->nDataPos = 0;
    bi->nTx = 1;
    bi->nChainTx = prev ? prev->nChainTx + 1 : 1;
    arith_uint256_set_u64(&bi->nChainWork, (uint64_t)(height - A + 1));
    return bi;
}

static bool setup_fixture(struct rfrl_fixture *fx, const char *tag)
{
    memset(fx, 0, sizeof(*fx));
    test_make_tmpdir(fx->dir, sizeof(fx->dir),
                     "reducer_frontier_reconcile_light", tag);
    if (!progress_store_open(fx->dir))
        return false;
    if (!seed_schema(progress_store_db()))
        return false;
    if (!seed_all_cursors(progress_store_db(), A + 4))
        return false;

    main_state_init(&fx->ms);
    fx->idx[1] = insert_index(&fx->ms, &fx->hashes[1], A + 1, NULL,
                              BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA);
    fx->idx[2] = insert_index(&fx->ms, &fx->hashes[2], A + 2,
                              fx->idx[1], BLOCK_HAVE_DATA);
    fx->idx[3] = insert_index(&fx->ms, &fx->hashes[3], A + 3,
                              fx->idx[2],
                              BLOCK_VALID_TREE | BLOCK_HAVE_DATA |
                              BLOCK_FAILED_VALID);
    if (!fx->idx[1] || !fx->idx[2] || !fx->idx[3])
        return false;

    if (!put_upstream_ok(progress_store_db(), A + 1, &fx->hashes[1]) ||
        !put_upstream_ok(progress_store_db(), A + 2, &fx->hashes[2]) ||
        !put_upstream_ok(progress_store_db(), A + 3, &fx->hashes[3]))
        return false;
    if (!put_tip_log(progress_store_db(), A + 1, 1, &fx->hashes[1]))
        return false;
    if (!seed_coins_applied(progress_store_db(), A + 1))
        return false;
    return true;
}

static void teardown_fixture(struct rfrl_fixture *fx)
{
    main_state_free(&fx->ms);
    progress_store_close();
    test_cleanup_tmpdir(fx->dir);
}

static int cursor_value(sqlite3 *db, const char *name)
{
    sqlite3_stmt *st = NULL;
    int value = -1;
    if (sqlite3_prepare_v2(db,
            "SELECT cursor FROM stage_cursor WHERE name=?",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW)
            value = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return value;
}

int test_reducer_frontier_reconcile_light(void);
int test_reducer_frontier_reconcile_light(void)
{
    printf("\n=== reducer_frontier_reconcile_light tests ===\n");
    int failures = 0;

    {
        struct rfrl_fixture fx;
        RFRL_CHECK("setup dry-run/apply fixture",
                   setup_fixture(&fx, "apply"));

        sqlite3 *db = progress_store_db();
        unsigned before2 = fx.idx[2]->nStatus;
        unsigned before3 = fx.idx[3]->nStatus;

        struct stage_reducer_frontier_reconcile_result dry;
        RFRL_CHECK("dry-run succeeds",
                   stage_reducer_frontier_reconcile_light_needed(
                       db, &fx.ms, &dry));
        RFRL_CHECK("dry-run reports repair",
                   dry.repaired && dry.hstar == A + 1 &&
                   dry.sweep_top == A + 3 &&
                   dry.lowest_have_data_cleared == A + 2 &&
                   dry.body_fetch_cursor_before == A + 4 &&
                   dry.body_fetch_cursor_after == A + 2 &&
                   dry.clamped_body_fetch &&
                   dry.tip_finalize_cursor_after == A + 2);
        RFRL_CHECK("dry-run does not mutate",
                   fx.idx[2]->nStatus == before2 &&
                   fx.idx[3]->nStatus == before3 &&
                   cursor_value(db, "body_fetch") == A + 4 &&
                   cursor_value(db, "tip_finalize") == A + 4);

        struct stage_reducer_frontier_reconcile_result applied;
        RFRL_CHECK("apply succeeds",
                   stage_reducer_frontier_reconcile_light(
                       db, &fx.ms, &applied));
        RFRL_CHECK("apply clamps body_fetch and tip_finalize",
                   cursor_value(db, "tip_finalize") == A + 2 &&
                   cursor_value(db, "body_fetch") == A + 2 &&
                   cursor_value(db, "utxo_apply") == A + 4 &&
                   applied.clamped_body_fetch &&
                   applied.clamped_tip_finalize);
        RFRL_CHECK("script bits restored",
                   (fx.idx[2]->nStatus & BLOCK_VALID_MASK) ==
                       BLOCK_VALID_SCRIPTS &&
                   (fx.idx[3]->nStatus & BLOCK_VALID_MASK) ==
                       BLOCK_VALID_SCRIPTS &&
                   applied.scripts_set == 2);
        RFRL_CHECK("unreadable HAVE_DATA cleared",
                   (fx.idx[2]->nStatus & BLOCK_HAVE_DATA) == 0 &&
                   (fx.idx[3]->nStatus & BLOCK_HAVE_DATA) == 0 &&
                   applied.have_data_cleared == 2);
        RFRL_CHECK("proved stale failure mask cleared",
                   (fx.idx[3]->nStatus & BLOCK_FAILED_MASK) == 0 &&
                   applied.failed_mask_cleared == 1);

        teardown_fixture(&fx);
    }

    {
        struct rfrl_fixture fx;
        RFRL_CHECK("setup coin-tear fixture",
                   setup_fixture(&fx, "cointear"));
        sqlite3 *db = progress_store_db();
        RFRL_CHECK("seed coins_applied above hstar",
                   seed_coins_applied(db, A + 3));

        struct stage_reducer_frontier_reconcile_result rr;
        RFRL_CHECK("coin-tear detect call succeeds",
                   stage_reducer_frontier_reconcile_light(
                       db, &fx.ms, &rr));
        RFRL_CHECK("coin-tear refused without mutation",
                   rr.refused_coin_tear &&
                   cursor_value(db, "tip_finalize") == A + 4 &&
                   (fx.idx[2]->nStatus & BLOCK_VALID_MASK) == 0);

        teardown_fixture(&fx);
    }

    {
        struct rfrl_fixture fx;
        RFRL_CHECK("setup served-floor fixture",
                   setup_fixture(&fx, "served_floor"));
        sqlite3 *db = progress_store_db();
        RFRL_CHECK("seed served floor above hstar without contiguous prefix",
                   put_tip_log(db, A + 3, 1, &fx.hashes[3]));

        struct stage_reducer_frontier_reconcile_result rr;
        RFRL_CHECK("served-floor apply succeeds",
                   stage_reducer_frontier_reconcile_light(
                       db, &fx.ms, &rr));
        RFRL_CHECK("served-floor cannot override coins cap",
                   rr.hstar == A + 1 &&
                   rr.served_floor == A + 3 &&
                   rr.coins_applied_height == A + 1 &&
                   rr.tip_finalize_cursor_after == A + 2 &&
                   cursor_value(db, "tip_finalize") == A + 2 &&
                   rr.clamped_tip_finalize);

        teardown_fixture(&fx);
    }

    {
        struct rfrl_fixture fx;
        RFRL_CHECK("setup coin-lag fixture",
                   setup_fixture(&fx, "coin_lag"));
        sqlite3 *db = progress_store_db();
        RFRL_CHECK("seed contiguous hstar above coins_applied",
                   put_tip_log(db, A + 2, 1, &fx.hashes[2]) &&
                   put_tip_log(db, A + 3, 1, &fx.hashes[3]) &&
                   seed_coins_applied(db, A + 2));

        struct stage_reducer_frontier_reconcile_result rr;
        RFRL_CHECK("coin-lag apply succeeds",
                   stage_reducer_frontier_reconcile_light(
                       db, &fx.ms, &rr));
        RFRL_CHECK("coin-lag caps tip_finalize at coins_applied + 1",
                   rr.hstar == A + 3 &&
                   rr.coins_applied_height == A + 2 &&
                   rr.tip_finalize_cursor_after == A + 3 &&
                   cursor_value(db, "tip_finalize") == A + 3 &&
                   rr.clamped_tip_finalize);

        teardown_fixture(&fx);
    }

    {
        struct rfrl_fixture fx;
        RFRL_CHECK("setup unknown-coin fixture",
                   setup_fixture(&fx, "unknown"));
        sqlite3 *db = progress_store_db();
        RFRL_CHECK("delete coins_applied frontier",
                   exec_sql(db, "DELETE FROM progress_meta "
                                "WHERE key='coins_applied_height'"));

        struct stage_reducer_frontier_reconcile_result rr;
        RFRL_CHECK("unknown-coin call succeeds",
                   stage_reducer_frontier_reconcile_light(
                       db, &fx.ms, &rr));
        RFRL_CHECK("unknown-coin refused without mutation",
                   rr.refused_coin_unknown &&
                   cursor_value(db, "tip_finalize") == A + 4 &&
                   (fx.idx[2]->nStatus & BLOCK_VALID_MASK) == 0);

        teardown_fixture(&fx);
    }

    printf("reducer_frontier_reconcile_light: %d failures\n", failures);
    return failures;
}
