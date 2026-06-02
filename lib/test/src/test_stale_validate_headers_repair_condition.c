/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"

#include "framework/condition.h"
#include "jobs/stage_repair.h"
#include "storage/progress_store.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <sqlite3.h>
#include <stdint.h>
#include <string.h>

#define SVHR_CHECK(name, expr) do { \
    printf("stale_validate_headers_repair: %s... ", (name)); \
    if ((expr)) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

void register_stale_validate_headers_repair(void);
void stale_validate_headers_repair_test_reset(void);
int stale_validate_headers_repair_test_remedy_calls(void);

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
            "fail_reason TEXT, validated_at INTEGER NOT NULL)") &&
        exec_sql(db,
            "CREATE TABLE IF NOT EXISTS body_fetch_log ("
            "height INTEGER PRIMARY KEY, hash BLOB NOT NULL, source TEXT NOT NULL,"
            "bytes INTEGER NOT NULL DEFAULT 0, fetched_at INTEGER NOT NULL,"
            "ok INTEGER NOT NULL, fail_reason TEXT)") &&
        exec_sql(db,
            "CREATE TABLE IF NOT EXISTS body_persist_log ("
            "height INTEGER PRIMARY KEY, source TEXT, ok INTEGER,"
            "persisted_at INTEGER)") &&
        exec_sql(db,
            "CREATE TABLE IF NOT EXISTS script_validate_log ("
            "height INTEGER PRIMARY KEY, status TEXT, ok INTEGER)") &&
        exec_sql(db,
            "CREATE TABLE IF NOT EXISTS proof_validate_log ("
            "height INTEGER PRIMARY KEY, status TEXT, ok INTEGER)") &&
        exec_sql(db,
            "CREATE TABLE IF NOT EXISTS utxo_apply_log ("
            "height INTEGER PRIMARY KEY, status TEXT, ok INTEGER)") &&
        exec_sql(db,
            "CREATE TABLE IF NOT EXISTS utxo_apply_delta ("
            "height INTEGER PRIMARY KEY)") &&
        exec_sql(db,
            "CREATE TABLE IF NOT EXISTS tip_finalize_log ("
            "height INTEGER PRIMARY KEY, status TEXT, ok INTEGER)");
}

static bool seed_cursors(sqlite3 *db, int validate_cursor,
                         int downstream_cursor)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO stage_cursor(name,cursor,updated_at) "
            "VALUES(?,?,1)",
            -1, &st, NULL) != SQLITE_OK)
        return false;
    static const char *const names[] = {
        "validate_headers",
        "body_fetch",
        "body_persist",
        "script_validate",
        "proof_validate",
        "utxo_apply",
        "tip_finalize",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
        sqlite3_bind_text(st, 1, names[i], -1, SQLITE_STATIC);
        sqlite3_bind_int(st, 2, i == 0 ? validate_cursor : downstream_cursor);
        if (sqlite3_step(st) != SQLITE_DONE) {
            sqlite3_finalize(st);
            return false;
        }
    }
    sqlite3_finalize(st);
    return true;
}

static bool seed_poison_rows(sqlite3 *db, int height, const char *vh_reason,
                             int vh_ok)
{
    char sql[4096];
    snprintf(sql, sizeof(sql),
        "INSERT OR REPLACE INTO validate_headers_log"
        "(height,hash,ok,fail_reason,validated_at) "
        "VALUES(%d,zeroblob(32),%d,%s,1);"
        "INSERT OR REPLACE INTO body_fetch_log"
        "(height,hash,source,bytes,fetched_at,ok,fail_reason) "
        "VALUES(%d,zeroblob(32),'skipped_invalid',0,1,0,"
        "'header_validation_failed');"
        "INSERT OR REPLACE INTO body_persist_log"
        "(height,source,ok,persisted_at) "
        "VALUES(%d,'upstream_failed',0,1);"
        "INSERT OR REPLACE INTO script_validate_log"
        "(height,status,ok) VALUES(%d,'upstream_failed',0);"
        "INSERT OR REPLACE INTO proof_validate_log"
        "(height,status,ok) VALUES(%d,'upstream_failed',0);"
        "INSERT OR REPLACE INTO utxo_apply_log"
        "(height,status,ok) VALUES(%d,'upstream_failed',0);"
        "INSERT OR REPLACE INTO utxo_apply_delta(height) VALUES(%d);"
        "INSERT OR REPLACE INTO tip_finalize_log"
        "(height,status,ok) VALUES(%d,'upstream_failed',0);",
        height, vh_ok, vh_reason ? vh_reason : "NULL",
        height, height, height, height, height, height, height);
    return exec_sql(db, sql);
}

static int cursor_for(sqlite3 *db, const char *name)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT cursor FROM stage_cursor WHERE name=?",
            -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    int out = -1;
    if (sqlite3_step(st) == SQLITE_ROW)
        out = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return out;
}

static bool row_exists(sqlite3 *db, const char *table, int height)
{
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT 1 FROM %s WHERE height=?", table);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, height);
    bool found = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return found;
}

static bool validate_ok_row_exists(sqlite3 *db, int height)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT ok FROM validate_headers_log WHERE height=?",
            -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, height);
    bool ok = false;
    if (sqlite3_step(st) == SQLITE_ROW)
        ok = sqlite3_column_int(st, 0) == 1;
    sqlite3_finalize(st);
    return ok;
}

static bool seed_repair_header(sqlite3 *db, int height)
{
    struct block_header h;
    block_header_init(&h);
    h.nVersion = 4;
    h.hashPrevBlock.data[0] = (uint8_t)(height - 1);
    h.hashPrevBlock.data[1] = 0xA7;
    h.hashMerkleRoot.data[0] = (uint8_t)height;
    h.hashMerkleRoot.data[1] = 0xB8;
    h.hashFinalSaplingRoot.data[0] = (uint8_t)height;
    h.hashFinalSaplingRoot.data[1] = 0xC9;
    h.nTime = 1700000000u + (uint32_t)height;
    h.nBits = 0x1f07ffff;
    h.nNonce.data[0] = (uint8_t)height;
    h.nNonce.data[1] = 0xDA;
    h.nSolutionSize = 32;
    for (size_t i = 0; i < h.nSolutionSize; i++)
        h.nSolution[i] = (uint8_t)(height + (int)i);

    struct uint256 hash;
    block_header_get_hash(&h, &hash);
    return stage_repair_header_solution_save(db, height, &hash, &h);
}

static void setup_main_state(struct main_state *ms,
                             struct block_index blocks[2],
                             struct uint256 hashes[2])
{
    main_state_init(ms);
    for (int i = 0; i < 2; i++) {
        block_index_init(&blocks[i]);
        memset(&hashes[i], 0, sizeof(hashes[i]));
        hashes[i].data[0] = (uint8_t)i;
        hashes[i].data[1] = 0xA7;
        blocks[i].phashBlock = &hashes[i];
        blocks[i].nHeight = i;
        blocks[i].nStatus = BLOCK_VALID_TREE;
        if (i > 0)
            blocks[i].pprev = &blocks[i - 1];
    }
    active_chain_move_window_tip(&ms->chain_active, &blocks[1]);
}

static bool setup_condition_case(const char *tag, char *dir, size_t dir_n,
                                 struct main_state *ms,
                                 struct block_index blocks[2],
                                 struct uint256 hashes[2])
{
    condition_engine_reset_for_testing();
    stale_validate_headers_repair_test_reset();
    test_make_tmpdir(dir, dir_n, "stale_vh_repair", tag);
    if (!progress_store_open(dir))
        return false;
    setup_main_state(ms, blocks, hashes);
    condition_engine_set_main_state(ms);
    register_stale_validate_headers_repair();
    return seed_schema(progress_store_db());
}

static void teardown_condition_case(const char *dir, struct main_state *ms)
{
    condition_engine_set_main_state(NULL);
    main_state_free(ms);
    progress_store_close();
    test_cleanup_tmpdir(dir);
    condition_engine_reset_for_testing();
}

int test_stale_validate_headers_repair_condition(void)
{
    printf("\n=== stale_validate_headers_repair condition tests ===\n");
    int failures = 0;

    {
        char dir[256];
        struct main_state ms;
        struct block_index blocks[2];
        struct uint256 hashes[2];
        bool ok = setup_condition_case("downstream", dir, sizeof(dir),
                                       &ms, blocks, hashes);
        sqlite3 *db = progress_store_db();
        ok = ok && seed_cursors(db, 5, 5);
        ok = ok && seed_poison_rows(db, 2, "NULL", 1);

        condition_engine_tick();

        ok = ok && stale_validate_headers_repair_test_remedy_calls() == 1;
        ok = ok && condition_engine_get_active_count() == 0;
        ok = ok && cursor_for(db, "validate_headers") == 5;
        ok = ok && cursor_for(db, "body_fetch") == 2;
        ok = ok && validate_ok_row_exists(db, 2);
        ok = ok && !row_exists(db, "body_fetch_log", 2);
        /* tip_finalize_log rows MUST survive a downstream rewind — doctrine
         * forbids deleting them. The cursor is rewound; the (ok=0) row stays. */
        ok = ok && row_exists(db, "tip_finalize_log", 2);
        SVHR_CHECK("stale downstream poison rewinds downstream, preserves "
                   "tip_finalize_log", ok);
        teardown_condition_case(dir, &ms);
    }

    {
        char dir[256];
        struct main_state ms;
        struct block_index blocks[2];
        struct uint256 hashes[2];
        bool ok = setup_condition_case("solutionless_no_header", dir, sizeof(dir),
                                       &ms, blocks, hashes);
        sqlite3 *db = progress_store_db();
        ok = ok && seed_cursors(db, 5, 5);
        ok = ok && seed_poison_rows(
            db, 2, "'no-header-solution-backfill-required'", 0);

        condition_engine_tick();

        ok = ok && stale_validate_headers_repair_test_remedy_calls() == 1;
        ok = ok && condition_engine_get_active_count() == 1;
        ok = ok && cursor_for(db, "validate_headers") == 5;
        ok = ok && cursor_for(db, "body_fetch") == 5;
        ok = ok && row_exists(db, "validate_headers_log", 2);
        ok = ok && row_exists(db, "body_fetch_log", 2);
        SVHR_CHECK("solutionless poison without repair header stays active", ok);
        teardown_condition_case(dir, &ms);
    }

    {
        char dir[256];
        struct main_state ms;
        struct block_index blocks[2];
        struct uint256 hashes[2];
        bool ok = setup_condition_case("solutionless_with_header", dir,
                                       sizeof(dir),
                                       &ms, blocks, hashes);
        sqlite3 *db = progress_store_db();
        ok = ok && seed_cursors(db, 5, 5);
        ok = ok && seed_poison_rows(
            db, 2, "'no-header-solution-backfill-required'", 0);
        ok = ok && seed_repair_header(db, 2);

        condition_engine_tick();

        ok = ok && stale_validate_headers_repair_test_remedy_calls() == 1;
        ok = ok && condition_engine_get_active_count() == 0;
        ok = ok && cursor_for(db, "validate_headers") == 2;
        ok = ok && cursor_for(db, "body_fetch") == 2;
        ok = ok && !row_exists(db, "validate_headers_log", 2);
        ok = ok && !row_exists(db, "body_fetch_log", 2);
        /* The validate-frontier rewind also must NOT delete tip_finalize_log
         * rows; the seeded (ok=0) row survives the repair. */
        ok = ok && row_exists(db, "tip_finalize_log", 2);
        SVHR_CHECK("solutionless validate poison rewinds validate frontier, "
                   "preserves tip_finalize_log", ok);
        teardown_condition_case(dir, &ms);
    }

    {
        char dir[256];
        struct main_state ms;
        struct block_index blocks[2];
        struct uint256 hashes[2];
        bool ok = setup_condition_case("invalid", dir, sizeof(dir),
                                       &ms, blocks, hashes);
        sqlite3 *db = progress_store_db();
        ok = ok && seed_cursors(db, 5, 5);
        ok = ok && seed_poison_rows(db, 2, "'invalid-solution'", 0);

        condition_engine_tick();

        ok = ok && stale_validate_headers_repair_test_remedy_calls() == 0;
        ok = ok && cursor_for(db, "validate_headers") == 5;
        ok = ok && cursor_for(db, "body_fetch") == 5;
        ok = ok && row_exists(db, "body_fetch_log", 2);
        SVHR_CHECK("consensus-invalid skip is not repaired", ok);
        teardown_condition_case(dir, &ms);
    }

    return failures;
}
