/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for the block_index_integrity service.
 *
 * Each test spins up a throw-away datadir, writes a mock block_index.bin
 * body, calls bii_write_sidecar to commit the bytes, and then drives
 * bii_verify through the seven verdicts the service can return. The
 * quarantine rename is checked too — important because an accidental
 * delete would lose operator forensic data.
 */

#include "test/test_helpers.h"

#include "services/block_index_integrity.h"
#include "models/database.h"
#include "event/event.h"

#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── Event counter for EV_BLOCK_INDEX_CORRUPT ──────────────── */

static _Atomic int g_bii_ev_corrupt;

static void bii_ev_observer(enum event_type type, uint32_t peer_id,
                             const void *payload, uint32_t payload_len,
                             void *ctx)
{
    (void)peer_id; (void)payload; (void)payload_len; (void)ctx;
    if (type == EV_BLOCK_INDEX_CORRUPT)
        atomic_fetch_add(&g_bii_ev_corrupt, 1);
}

static void bii_install_observer(void)
{
    event_clear_observers(EV_BLOCK_INDEX_CORRUPT);
    atomic_store(&g_bii_ev_corrupt, 0);
    event_observe(EV_BLOCK_INDEX_CORRUPT, bii_ev_observer, NULL);
}

#define BII_RUN(name, expr) do { \
    printf("%s... ", (name));    \
    bool _ok = (expr);           \
    if (_ok) printf("OK\n");     \
    else { printf("FAIL\n"); failures++; } \
} while (0)

/* ── Test datadir harness ───────────────────────────────────── */

static bool bii_make_tmpdir(char *out, size_t cap)
{
    /* Unique suffix via PID+counter so parallel tests don't collide. */
    static _Atomic int seq = 0;
    int s = atomic_fetch_add(&seq, 1);
    snprintf(out, cap, "/tmp/zcl23_bii_test_%d_%d", (int)getpid(), s);
    mkdir(out, 0700);
    return true;
}

static bool bii_write_body(const char *datadir,
                           const void *bytes, size_t len)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/block_index.bin", datadir);
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(bytes, 1, len, f) == len;
    fclose(f);
    return ok;
}

static bool bii_sidecar_exists(const char *datadir)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/block_index.bin.sha3", datadir);
    struct stat st;
    return stat(path, &st) == 0;
}

static void bii_tear_down(const char *datadir)
{
    test_cleanup_tmpdir(datadir);
}

/* ── 1. Happy path: write sidecar, verify passes ───────────── */

static int t_happy_path(void)
{
    int failures = 0;
    char dir[256];
    bii_make_tmpdir(dir, sizeof(dir));

    const char body[] = "ZCLI" "\x00\x00\x00\x00" "block_index_body_placeholder_payload";
    bool body_ok = bii_write_body(dir, body, sizeof(body) - 1);
    bool side_ok = bii_write_sidecar(dir);
    bool sidecar_present = bii_sidecar_exists(dir);

    char err[256];
    enum bii_verdict v = bii_verify(dir, NULL, NULL, err, sizeof(err));

    bool ok = body_ok && side_ok && sidecar_present && v == BII_OK;
    BII_RUN("bii: happy path write+verify returns OK", ok);

    bii_tear_down(dir);
    return failures;
}

/* ── 2. Missing sidecar returns SIDECAR_MISSING ──────────── */

static int t_sidecar_missing(void)
{
    int failures = 0;
    char dir[256];
    bii_make_tmpdir(dir, sizeof(dir));

    const char body[] = "body-no-sidecar";
    bii_write_body(dir, body, sizeof(body) - 1);

    char err[256];
    enum bii_verdict v = bii_verify(dir, NULL, NULL, err, sizeof(err));
    bool ok = v == BII_SIDECAR_MISSING;
    BII_RUN("bii: missing sidecar returns SIDECAR_MISSING", ok);

    bii_tear_down(dir);
    return failures;
}

/* ── 3. Body missing returns BODY_MISSING ──────────────────── */

static int t_body_missing(void)
{
    int failures = 0;
    char dir[256];
    bii_make_tmpdir(dir, sizeof(dir));

    char err[256];
    enum bii_verdict v = bii_verify(dir, NULL, NULL, err, sizeof(err));
    bool ok = v == BII_BODY_MISSING;
    BII_RUN("bii: missing body returns BODY_MISSING", ok);

    bii_tear_down(dir);
    return failures;
}

/* ── 4. Stale sidecar (body size changed after write) ─────── */

static int t_sidecar_stale(void)
{
    int failures = 0;
    char dir[256];
    bii_make_tmpdir(dir, sizeof(dir));

    const char body1[] = "original-body-bytes";
    bii_write_body(dir, body1, sizeof(body1) - 1);
    bii_write_sidecar(dir);

    /* Grow the body; sidecar's body_size is now stale. */
    const char body2[] = "original-body-bytes-EXTRA-EXTRA";
    bii_write_body(dir, body2, sizeof(body2) - 1);

    char err[256];
    enum bii_verdict v = bii_verify(dir, NULL, NULL, err, sizeof(err));
    bool ok = v == BII_SIDECAR_STALE;
    BII_RUN("bii: size drift returns SIDECAR_STALE", ok);

    bii_tear_down(dir);
    return failures;
}

/* ── 5. Hash mismatch (same size, different bytes) ─────────── */

static int t_hash_mismatch(void)
{
    int failures = 0;
    char dir[256];
    bii_make_tmpdir(dir, sizeof(dir));

    const char body1[] = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    bii_write_body(dir, body1, sizeof(body1) - 1);
    bii_write_sidecar(dir);

    /* Same length, different bytes → size check passes but hash
     * check fires. */
    const char body2[] = "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBB";
    bii_write_body(dir, body2, sizeof(body2) - 1);

    char err[256];
    enum bii_verdict v = bii_verify(dir, NULL, NULL, err, sizeof(err));
    bool ok = v == BII_HASH_MISMATCH && strstr(err, "sha3") != NULL;
    BII_RUN("bii: equal-size corruption returns HASH_MISMATCH", ok);

    bii_tear_down(dir);
    return failures;
}

/* ── 6. Bad magic in sidecar ────────────────────────────────── */

static int t_sidecar_bad_magic(void)
{
    int failures = 0;
    char dir[256];
    bii_make_tmpdir(dir, sizeof(dir));

    const char body[] = "body-for-bad-magic";
    bii_write_body(dir, body, sizeof(body) - 1);
    bii_write_sidecar(dir);

    /* Corrupt the magic in-place. */
    char side_path[1024];
    snprintf(side_path, sizeof(side_path), "%s/block_index.bin.sha3", dir);
    FILE *f = fopen(side_path, "r+b");
    if (f) {
        fwrite("XXXX", 1, 4, f);
        fclose(f);
    }

    char err[256];
    enum bii_verdict v = bii_verify(dir, NULL, NULL, err, sizeof(err));
    bool ok = v == BII_SIDECAR_BAD_MAGIC;
    BII_RUN("bii: corrupt sidecar magic returns SIDECAR_BAD_MAGIC", ok);

    bii_tear_down(dir);
    return failures;
}

/* ── 7. SQLite cross-check — tip missing in SQL ────────────── */

static int t_tip_missing_in_sql(void)
{
    int failures = 0;
    char dir[256];
    bii_make_tmpdir(dir, sizeof(dir));

    const char body[] = "body-for-tip-check";
    bii_write_body(dir, body, sizeof(body) - 1);
    bii_write_sidecar(dir);

    /* Open an in-memory node_db (empty blocks table). Build a
     * declared_tip that points at a hash the DB doesn't know. */
    struct node_db ndb;
    memset(&ndb, 0, sizeof(ndb));
    node_db_open(&ndb, ":memory:");

    struct uint256 hash = {{0}};
    hash.data[0] = 0xAB; hash.data[1] = 0xCD;
    struct block_index tip = {0};
    tip.phashBlock = &hash;
    tip.nHeight = 12345;

    char err[256];
    enum bii_verdict v = bii_verify(dir, &ndb, &tip, err, sizeof(err));
    bool ok = v == BII_TIP_MISSING_IN_SQL;
    BII_RUN("bii: declared tip absent from SQLite returns TIP_MISSING_IN_SQL", ok);

    node_db_close(&ndb);
    bii_tear_down(dir);
    return failures;
}

/* ── 8. SQLite cross-check — tip present but height drifted ── */

static int t_tip_height_mismatch(void)
{
    int failures = 0;
    char dir[256];
    bii_make_tmpdir(dir, sizeof(dir));

    const char body[] = "body-for-height-check";
    bii_write_body(dir, body, sizeof(body) - 1);
    bii_write_sidecar(dir);

    struct node_db ndb;
    memset(&ndb, 0, sizeof(ndb));
    node_db_open(&ndb, ":memory:");

    /* Insert a blocks row with height=999 for a specific hash.
     * All NOT NULL columns must be supplied to satisfy the schema
     * at lib/app/models/src/database.c. */
    struct uint256 hash = {{0}};
    hash.data[0] = 0x11; hash.data[31] = 0xEE;
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(ndb.db,
        "INSERT INTO blocks("
        "hash,height,prev_hash,version,merkle_root,"
        "time,bits,nonce,solution,chain_work) "
        "VALUES(?,?,?,0,?,0,0,?,?,?)",
        -1, &st, NULL);
    if (rc == SQLITE_OK && st) {
        static const uint8_t zero32[32] = {0};
        static const uint8_t dummy_solution[4] = {0};
        sqlite3_bind_blob(st, 1, hash.data, 32, SQLITE_STATIC);
        sqlite3_bind_int64(st, 2, 999);
        sqlite3_bind_blob(st, 3, zero32, 32, SQLITE_STATIC);
        sqlite3_bind_blob(st, 4, zero32, 32, SQLITE_STATIC);  /* merkle_root */
        sqlite3_bind_blob(st, 5, zero32, 32, SQLITE_STATIC);  /* nonce */
        sqlite3_bind_blob(st, 6, dummy_solution, sizeof(dummy_solution), SQLITE_STATIC);
        sqlite3_bind_blob(st, 7, zero32, 32, SQLITE_STATIC);  /* chain_work */
        int step_rc = sqlite3_step(st);
        if (step_rc != SQLITE_DONE) {
            fprintf(stderr, "bii test: blocks insert failed: %s\n",
                    sqlite3_errmsg(ndb.db));
        }
        sqlite3_finalize(st);
    } else {
        fprintf(stderr, "bii test: blocks insert prepare failed: %s\n",
                sqlite3_errmsg(ndb.db));
    }

    struct block_index tip = {0};
    tip.phashBlock = &hash;
    tip.nHeight = 12345;  /* DISAGREES with SQLite */

    char err[256];
    enum bii_verdict v = bii_verify(dir, &ndb, &tip, err, sizeof(err));
    bool ok = v == BII_TIP_HEIGHT_MISMATCH;
    BII_RUN("bii: declared tip height ≠ SQLite height returns TIP_HEIGHT_MISMATCH", ok);

    node_db_close(&ndb);
    bii_tear_down(dir);
    return failures;
}

/* ── 9. Quarantine renames both files and emits event ──────── */

static int t_quarantine_renames(void)
{
    int failures = 0;
    bii_install_observer();

    char dir[256];
    bii_make_tmpdir(dir, sizeof(dir));

    const char body[] = "to-be-quarantined";
    bii_write_body(dir, body, sizeof(body) - 1);
    bii_write_sidecar(dir);

    bii_quarantine_corrupt(dir, BII_HASH_MISMATCH);

    /* Neither original file should exist anymore. */
    char body_path[1024], side_path[1024];
    snprintf(body_path, sizeof(body_path), "%s/block_index.bin", dir);
    snprintf(side_path, sizeof(side_path), "%s/block_index.bin.sha3", dir);
    struct stat st;
    bool body_gone = stat(body_path, &st) != 0;
    bool side_gone = stat(side_path, &st) != 0;

    /* But a .corrupt.<ts> sibling should now exist for each. */
    DIR *d = opendir(dir);
    int corrupt_count = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (strstr(e->d_name, ".corrupt.")) corrupt_count++;
        }
        closedir(d);
    }

    bool ok = body_gone && side_gone && corrupt_count == 2 &&
              atomic_load(&g_bii_ev_corrupt) == 1;
    BII_RUN("bii: quarantine renames both files and emits event", ok);

    bii_tear_down(dir);
    event_clear_observers(EV_BLOCK_INDEX_CORRUPT);
    return failures;
}

/* ── 10. Quarantine is idempotent and safe when files absent ── */

static int t_quarantine_missing_is_noop(void)
{
    int failures = 0;
    bii_install_observer();

    char dir[256];
    bii_make_tmpdir(dir, sizeof(dir));

    /* No files written. Quarantine must not crash. */
    bii_quarantine_corrupt(dir, BII_SIDECAR_STALE);

    /* Still emits the event — the caller decided there was
     * corruption and the event is their audit trail. */
    bool ok = atomic_load(&g_bii_ev_corrupt) == 1;
    BII_RUN("bii: quarantine on empty dir is a safe no-op + event", ok);

    bii_tear_down(dir);
    event_clear_observers(EV_BLOCK_INDEX_CORRUPT);
    return failures;
}

/* ── 11. Verdict names cover every value ───────────────────── */

static int t_verdict_names(void)
{
    int failures = 0;
    bool ok = strcmp(bii_verdict_name(BII_OK), "ok") == 0 &&
              strcmp(bii_verdict_name(BII_SIDECAR_MISSING), "sidecar_missing") == 0 &&
              strcmp(bii_verdict_name(BII_SIDECAR_STALE), "sidecar_stale") == 0 &&
              strcmp(bii_verdict_name(BII_HASH_MISMATCH), "hash_mismatch") == 0 &&
              strcmp(bii_verdict_name(BII_TIP_HEIGHT_MISMATCH), "tip_height_mismatch") == 0 &&
              strcmp(bii_verdict_name(BII_TIP_MISSING_IN_SQL), "tip_missing_in_sql") == 0 &&
              strcmp(bii_verdict_name(BII_BODY_MISSING), "body_missing") == 0 &&
              strcmp(bii_verdict_name(BII_BODY_UNREADABLE), "body_unreadable") == 0 &&
              strcmp(bii_verdict_name(BII_SIDECAR_BAD_MAGIC), "sidecar_bad_magic") == 0 &&
              strcmp(bii_verdict_name(BII_SIDECAR_UNSUPPORTED), "sidecar_unsupported") == 0;
    BII_RUN("bii: verdict names are stable strings", ok);
    return failures;
}

/* ── Aggregator ─────────────────────────────────────────────── */

int test_block_index_integrity(void)
{
    printf("\n=== block_index_integrity tests ===\n");
    int failures = 0;
    failures += t_happy_path();
    failures += t_sidecar_missing();
    failures += t_body_missing();
    failures += t_sidecar_stale();
    failures += t_hash_mismatch();
    failures += t_sidecar_bad_magic();
    failures += t_tip_missing_in_sql();
    failures += t_tip_height_mismatch();
    failures += t_quarantine_renames();
    failures += t_quarantine_missing_is_noop();
    failures += t_verdict_names();
    return failures;
}
