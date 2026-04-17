/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Regression tests for coins_view_sqlite boot-time tip integrity check.
 *
 * The invariant: on open, MAX(utxos.height) must not exceed the height
 * of the block referenced by node_state.coins_best_block.  A violation
 * means the UTXO set drifted ahead of the anchor — possible after a
 * SIGKILL landed the UTXO writes but not the tip update, or after an
 * operator bungled a manual recovery.  The check halts instead of
 * silently "self-healing" (never wipe above tip; memory rule).
 *
 * These tests construct the mismatch conditions directly rather than
 * forking a live node, so they exercise the detection logic
 * deterministically without needing a running chain.
 */

#include "test/test_helpers.h"
#include "storage/coins_view_sqlite.h"
#include "models/database.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int mkdir_p(const char *p)
{
    if (mkdir(p, 0700) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

static void cva_path(char *buf, size_t n, const char *tag)
{
    snprintf(buf, n, "./test-tmp/cva_%d_%s", (int)getpid(), tag);
}

/* Build a minimal DB with just the tables we need — avoids the cost
 * (and migration coupling) of running node_db_open. */
static bool build_min_db(sqlite3 **out, const char *dbpath)
{
    if (sqlite3_open(dbpath, out) != SQLITE_OK) return false;
    char *err = NULL;
    int rc = sqlite3_exec(*out,
        "CREATE TABLE IF NOT EXISTS utxos("
        " txid BLOB, vout INTEGER, value INTEGER,"
        " script BLOB, script_type INTEGER, address_hash BLOB,"
        " height INTEGER, is_coinbase INTEGER,"
        " PRIMARY KEY(txid,vout));"
        "CREATE TABLE IF NOT EXISTS node_state("
        " key TEXT PRIMARY KEY, value BLOB);"
        "CREATE TABLE IF NOT EXISTS blocks("
        " hash BLOB PRIMARY KEY, height INTEGER, status INTEGER);",
        NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "build_min_db exec err: %s\n", err ? err : "?");
        sqlite3_free(err);
        return false;
    }
    return true;
}

static void seed_utxo(sqlite3 *db, int height, uint8_t txid_byte)
{
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(db,
        "INSERT INTO utxos(txid,vout,value,script,script_type,"
        " address_hash,height,is_coinbase) VALUES(?,0,0,NULL,0,NULL,?,0)",
        -1, &s, NULL);
    uint8_t txid[32];
    memset(txid, txid_byte, 32);
    sqlite3_bind_blob(s, 1, txid, 32, SQLITE_STATIC);
    sqlite3_bind_int(s, 2, height);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

static void set_tip(sqlite3 *db, const uint8_t hash[32])
{
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO node_state(key,value) "
        "VALUES('coins_best_block',?)", -1, &s, NULL);
    sqlite3_bind_blob(s, 1, hash, 32, SQLITE_STATIC);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

static void seed_block(sqlite3 *db, const uint8_t hash[32], int height)
{
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(db,
        "INSERT INTO blocks(hash,height,status) VALUES(?,?,?)",
        -1, &s, NULL);
    sqlite3_bind_blob(s, 1, hash, 32, SQLITE_STATIC);
    sqlite3_bind_int(s, 2, height);
    sqlite3_bind_int(s, 3, 3); /* status>=3 = accepted */
    sqlite3_step(s);
    sqlite3_finalize(s);
}

static int t_fresh_db_opens(void)
{
    int failures = 0;
    char dir[256]; cva_path(dir, sizeof(dir), "fresh"); mkdir_p(dir);
    char dbpath[512]; snprintf(dbpath, sizeof(dbpath), "%s/node.db", dir);

    TEST("cva: fresh DB (no utxos, no tip) opens cleanly") {
        sqlite3 *db = NULL;
        ASSERT(build_min_db(&db, dbpath));
        struct coins_view_sqlite cvs;
        ASSERT(coins_view_sqlite_open(&cvs, db));
        coins_view_sqlite_close(&cvs);
        sqlite3_close(db);
        PASS();
    } _test_next:;
    test_cleanup_tmpdir(dir);
    return failures;
}

static int t_utxos_without_tip_rejected(void)
{
    int failures = 0;
    char dir[256]; cva_path(dir, sizeof(dir), "orphan_utxo"); mkdir_p(dir);
    char dbpath[512]; snprintf(dbpath, sizeof(dbpath), "%s/node.db", dir);

    TEST("cva: UTXOs without coins_best_block → open refused") {
        sqlite3 *db = NULL;
        ASSERT(build_min_db(&db, dbpath));
        seed_utxo(db, 100, 0x11);
        seed_utxo(db, 101, 0x12);
        /* no set_tip() — simulate crash-between-flush-and-anchor */

        struct coins_view_sqlite cvs;
        bool opened = coins_view_sqlite_open(&cvs, db);
        ASSERT(!opened);  /* refused */
        sqlite3_close(db);
        PASS();
    } _test_next:;
    test_cleanup_tmpdir(dir);
    return failures;
}

static int t_matching_tip_accepted(void)
{
    int failures = 0;
    char dir[256]; cva_path(dir, sizeof(dir), "match"); mkdir_p(dir);
    char dbpath[512]; snprintf(dbpath, sizeof(dbpath), "%s/node.db", dir);

    TEST("cva: UTXOs with matching tip height → open accepts") {
        sqlite3 *db = NULL;
        ASSERT(build_min_db(&db, dbpath));
        seed_utxo(db, 100, 0x11);
        uint8_t tip[32]; memset(tip, 0xAA, 32);
        seed_block(db, tip, 100);
        set_tip(db, tip);

        struct coins_view_sqlite cvs;
        ASSERT(coins_view_sqlite_open(&cvs, db));
        coins_view_sqlite_close(&cvs);
        sqlite3_close(db);
        PASS();
    } _test_next:;
    test_cleanup_tmpdir(dir);
    return failures;
}

static int t_utxos_above_tip_rejected(void)
{
    int failures = 0;
    char dir[256]; cva_path(dir, sizeof(dir), "ahead"); mkdir_p(dir);
    char dbpath[512]; snprintf(dbpath, sizeof(dbpath), "%s/node.db", dir);

    TEST("cva: UTXOs strictly ahead of tip → open refused") {
        sqlite3 *db = NULL;
        ASSERT(build_min_db(&db, dbpath));
        seed_utxo(db, 200, 0x33);   /* UTXO at height 200 */
        uint8_t tip[32]; memset(tip, 0xBB, 32);
        seed_block(db, tip, 100);   /* but tip is at height 100 */
        set_tip(db, tip);

        struct coins_view_sqlite cvs;
        bool opened = coins_view_sqlite_open(&cvs, db);
        ASSERT(!opened);
        sqlite3_close(db);
        PASS();
    } _test_next:;
    test_cleanup_tmpdir(dir);
    return failures;
}

int test_coins_view_atomicity(void);

int test_coins_view_atomicity(void)
{
    printf("\n=== coins_view_atomicity tests ===\n");
    int failures = 0;
    mkdir_p("./test-tmp");
    failures += t_fresh_db_opens();
    failures += t_utxos_without_tip_rejected();
    failures += t_matching_tip_accepted();
    failures += t_utxos_above_tip_rejected();
    return failures;
}
