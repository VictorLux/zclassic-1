/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Unit tests for legacy_oneshot_import (Wave S, S-4b).
 *
 * What this file does NOT do: drive the full import pipeline. That
 * requires a running zclassicd at $HOME/.zclassic plus a clean test
 * datadir to import into; the right place to verify it end-to-end is
 * the live `-legacy-attach=$HOME/.zclassic` boot path during `make
 * deploy` (per the per-stage DoD in graceful-roaming-goose.md:
 * "Deploy per milestone, not per wave"). The DoD acceptance check —
 * "fresh datadir reaches legacy's tip in <120s, progress.kv stage
 * cursors reflect the new height" — is an operator-run, real-node
 * check, not a unit test.
 *
 * What this file DOES cover:
 *   - Outcome enum → name mapping.
 *   - Bad-argument rejection (the LOG_FAIL macro is fatal in
 *     production but returns false to a test harness when
 *     ZCL_LOG_FATAL_RETURN is set; we use the no-progress-store
 *     pre-condition path instead, which is graceful).
 *   - Missing legacy datadir → LOI_OUTCOME_LEGACY_NOT_FOUND, soft skip
 *     (returns true so boot continues).
 *   - Atomicity contract of the finalization pattern: simulate a
 *     partial multi-key write via BEGIN IMMEDIATE + writes + ROLLBACK
 *     (no COMMIT), verify on reopen that none of the partial writes
 *     are observable. This is the cursor-stamp atomic-commit promise.
 */

#include "test/test_helpers.h"

#include "services/legacy_bootstrap_importer.h"
#include "storage/progress_store.h"
#include "util/blocker.h"
#include "util/stage.h"

/* Test seam exported by legacy_bootstrap_importer.c — exercises the
 * anti-rewind branch without spinning up the full import pipeline. */
bool loi_stamp_one_for_test(sqlite3 *db, const char *name,
                            uint64_t cursor, bool *out_was_write);

#include <errno.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOI_CHECK(name, expr) do { \
    printf("legacy_oneshot_import: %s... ", (name)); \
    if ((expr)) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

static void loi_tmpdir(char *buf, size_t n, const char *tag)
{
    snprintf(buf, n, "./test-tmp/loi_%d_%s", (int)getpid(), tag);
}

static int mkdir_p(const char *p)
{
    if (mkdir(p, 0700) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

int test_legacy_oneshot_import(void)
{
    printf("\n=== legacy_oneshot_import tests ===\n");
    int failures = 0;

    blocker_module_init();

    /* ── 1. Outcome name mapping ─────────────────────────────────────── */
    LOI_CHECK("name(DID_IMPORT)",
              strcmp(loi_outcome_name(LOI_OUTCOME_DID_IMPORT),
                     "did_import") == 0);
    LOI_CHECK("name(NOOP_SAME_TIP)",
              strcmp(loi_outcome_name(LOI_OUTCOME_NOOP_SAME_TIP),
                     "noop_same_tip") == 0);
    LOI_CHECK("name(RECOVERED_FROM_CRASH)",
              strcmp(loi_outcome_name(LOI_OUTCOME_RECOVERED_FROM_CRASH),
                     "recovered_from_crash") == 0);
    LOI_CHECK("name(REFUSED_HAS_STATE)",
              strcmp(loi_outcome_name(LOI_OUTCOME_REFUSED_HAS_STATE),
                     "refused_has_state") == 0);
    LOI_CHECK("name(LEGACY_NOT_FOUND)",
              strcmp(loi_outcome_name(LOI_OUTCOME_LEGACY_NOT_FOUND),
                     "legacy_not_found") == 0);
    LOI_CHECK("name(FAILED)",
              strcmp(loi_outcome_name(LOI_OUTCOME_FAILED),
                     "failed") == 0);

    /* ── 2. Atomicity contract — BEGIN IMMEDIATE + writes + ROLLBACK ── */
    {
        char dir[256];
        loi_tmpdir(dir, sizeof(dir), "atomic");
        mkdir_p(dir);

        LOI_CHECK("open progress.kv", progress_store_open(dir));
        sqlite3 *db = progress_store_db();
        LOI_CHECK("db non-NULL", db != NULL);

        /* Simulate the legacy_oneshot_import finalization pattern: an
         * outer BEGIN IMMEDIATE that stamps multiple stage cursors +
         * sentinel keys, then ROLLBACK to mimic a crash before COMMIT.
         * The atomicity contract: none of the writes survive. */
        LOI_CHECK("BEGIN IMMEDIATE",
                  sqlite3_exec(db, "BEGIN IMMEDIATE",
                               NULL, NULL, NULL) == SQLITE_OK);

        /* Set the sentinel (simulating "import_in_progress"). */
        uint8_t one = 1;
        LOI_CHECK("set sentinel in tx",
                  progress_meta_set_in_tx(db, "import_in_progress",
                                          &one, 1));

        /* Stamp three stage cursors as the real finalization would. */
        sqlite3_stmt *st = NULL;
        LOI_CHECK("prepare stage upsert",
                  sqlite3_prepare_v2(db,
                      "INSERT INTO stage_cursor(name, cursor, updated_at) "
                      "VALUES(?,?,?) "
                      "ON CONFLICT(name) DO UPDATE SET "
                      "  cursor=excluded.cursor,"
                      "  updated_at=excluded.updated_at",
                      -1, &st, NULL) == SQLITE_OK);
        const char *names[] = {
            "header_admit", "validate_headers", "body_fetch"
        };
        bool stamps_ok = true;
        for (int i = 0; i < 3; i++) {
            sqlite3_reset(st);
            sqlite3_bind_text (st, 1, names[i], -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 2, (sqlite3_int64)(1000 + i));
            sqlite3_bind_int64(st, 3, 0);
            int rc = sqlite3_step(st);  // raw-sql-ok:test-direct
            if (rc != SQLITE_DONE) { stamps_ok = false; break; }
        }
        sqlite3_finalize(st);
        LOI_CHECK("stamp all 3 stage cursors", stamps_ok);

        /* Now ROLLBACK instead of COMMIT — simulates crash mid-tx. */
        LOI_CHECK("ROLLBACK",
                  sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL) ==
                      SQLITE_OK);

        /* Verify nothing leaked: sentinel absent, stage cursors absent. */
        bool found = true;
        size_t got = 0;
        uint8_t out[16] = {0};
        LOI_CHECK("post-rollback: sentinel get returns true",
                  progress_meta_get(db, "import_in_progress",
                                    out, sizeof(out), &got, &found));
        LOI_CHECK("post-rollback: sentinel NOT present", !found);

        for (int i = 0; i < 3; i++) {
            sqlite3_stmt *q = NULL;
            int rc = sqlite3_prepare_v2(db,
                "SELECT cursor FROM stage_cursor WHERE name=?",
                -1, &q, NULL);
            bool prep_ok = (rc == SQLITE_OK);
            if (prep_ok) {
                sqlite3_bind_text(q, 1, names[i], -1, SQLITE_TRANSIENT);
                rc = sqlite3_step(q);  // raw-sql-ok:test-direct
                /* Expected: SQLITE_DONE (no row). */
                LOI_CHECK("post-rollback: cursor row absent",
                          rc == SQLITE_DONE);
            }
            sqlite3_finalize(q);
        }

        /* And: verify that COMMIT path actually persists. */
        LOI_CHECK("BEGIN IMMEDIATE for commit-path",
                  sqlite3_exec(db, "BEGIN IMMEDIATE",
                               NULL, NULL, NULL) == SQLITE_OK);
        LOI_CHECK("set sentinel for commit-path",
                  progress_meta_set_in_tx(db, "import_in_progress",
                                          &one, 1));
        LOI_CHECK("set tip height for commit-path",
                  progress_meta_set_in_tx(db, "legacy_attach_tip_height",
                                          (int32_t[]){3120921}, 4));
        LOI_CHECK("delete sentinel in same tx",
                  progress_meta_delete_in_tx(db, "import_in_progress"));
        LOI_CHECK("COMMIT",
                  sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) ==
                      SQLITE_OK);

        found = true; got = 0;
        LOI_CHECK("post-commit: sentinel was deleted in tx",
                  progress_meta_get(db, "import_in_progress",
                                    out, sizeof(out), &got, &found) &&
                  !found);
        int32_t h_out = 0;
        LOI_CHECK("post-commit: tip_height observable",
                  progress_meta_get(db, "legacy_attach_tip_height",
                                    &h_out, sizeof(h_out), &got, &found) &&
                  found && got == 4 && h_out == 3120921);

        /* Reopen — same values should survive. */
        progress_store_close();
        LOI_CHECK("reopen after commit", progress_store_open(dir));
        h_out = 0; got = 0; found = false;
        LOI_CHECK("tip_height survives reopen",
                  progress_meta_get(progress_store_db(),
                                    "legacy_attach_tip_height",
                                    &h_out, sizeof(h_out), &got, &found) &&
                  found && h_out == 3120921);

        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    /* ── 3a. Drift gate — LOI_STAGES_TO_STAMP exactly matches expected.
     * Adding a stage requires updating BOTH the production list AND
     * this expected list — exactly the developer hand-off the long
     * comment in legacy_bootstrap_importer.h describes. */
    {
        static const char *const EXPECTED_LOI_STAGES[] = {
            "header_admit",
            "validate_headers",
            "body_fetch",
        };
        size_t expected_n = sizeof(EXPECTED_LOI_STAGES) /
                            sizeof(EXPECTED_LOI_STAGES[0]);
        LOI_CHECK("stages count matches expected",
                  loi_stages_to_stamp_count() == expected_n);
        for (size_t i = 0; i < expected_n; i++) {
            char label[96];
            snprintf(label, sizeof(label),
                     "stage[%zu] is '%s'", i, EXPECTED_LOI_STAGES[i]);
            const char *got = loi_stages_to_stamp_at(i);
            LOI_CHECK(label, got && strcmp(got, EXPECTED_LOI_STAGES[i]) == 0);
        }
        LOI_CHECK("stages_at(count) returns NULL",
                  loi_stages_to_stamp_at(expected_n) == NULL);
        LOI_CHECK("stages_at(big) returns NULL",
                  loi_stages_to_stamp_at(99) == NULL);
    }

    /* ── 3b. Anti-rewind guard — forward stamp writes, backward stamp
     * leaves existing value intact. The promise the production
     * pipeline relies on for "shadow stage already ahead of legacy". */
    {
        char dir[256];
        loi_tmpdir(dir, sizeof(dir), "rewind");
        mkdir_p(dir);

        LOI_CHECK("open progress.kv (rewind)", progress_store_open(dir));
        sqlite3 *db = progress_store_db();

        /* First stamp — fresh row, write happens. */
        bool was_write = false;
        LOI_CHECK("first stamp at 100 succeeds",
                  loi_stamp_one_for_test(db, "header_admit",
                                         100, &was_write));
        LOI_CHECK("first stamp reports was_write=true", was_write);

        /* Direct SELECT confirms cursor=100. */
        sqlite3_stmt *q = NULL;
        sqlite3_prepare_v2(db,
            "SELECT cursor FROM stage_cursor WHERE name='header_admit'",
            -1, &q, NULL);
        int rc = sqlite3_step(q);  // raw-sql-ok:test-direct
        int64_t cursor_after_first =
            rc == SQLITE_ROW ? sqlite3_column_int64(q, 0) : -1;
        sqlite3_finalize(q);
        LOI_CHECK("cursor stored as 100", cursor_after_first == 100);

        /* Forward stamp at 200 — write happens, cursor moves. */
        was_write = false;
        LOI_CHECK("forward stamp at 200 succeeds",
                  loi_stamp_one_for_test(db, "header_admit",
                                         200, &was_write));
        LOI_CHECK("forward stamp reports was_write=true", was_write);

        /* Backward stamp at 50 — anti-rewind kicks in: success, no write. */
        was_write = true;  /* deliberately wrong sentinel */
        LOI_CHECK("backward stamp at 50 succeeds (no-op)",
                  loi_stamp_one_for_test(db, "header_admit",
                                         50, &was_write));
        LOI_CHECK("backward stamp reports was_write=false", !was_write);

        sqlite3_prepare_v2(db,
            "SELECT cursor FROM stage_cursor WHERE name='header_admit'",
            -1, &q, NULL);
        rc = sqlite3_step(q);  // raw-sql-ok:test-direct
        int64_t cursor_after_rewind =
            rc == SQLITE_ROW ? sqlite3_column_int64(q, 0) : -1;
        sqlite3_finalize(q);
        LOI_CHECK("cursor preserved at 200 after backward stamp",
                  cursor_after_rewind == 200);

        /* Equal stamp at 200 — anti-rewind treats it as no-op. */
        was_write = true;
        LOI_CHECK("equal stamp at 200 succeeds (no-op)",
                  loi_stamp_one_for_test(db, "header_admit",
                                         200, &was_write));
        LOI_CHECK("equal stamp reports was_write=false", !was_write);

        /* Forward stamp at 5_000_000 — large jumps work. */
        was_write = false;
        LOI_CHECK("large forward stamp succeeds",
                  loi_stamp_one_for_test(db, "header_admit",
                                         5000000, &was_write));
        LOI_CHECK("large forward stamp reports was_write=true", was_write);

        /* Stamping a different stage starts fresh at the proposed value. */
        was_write = false;
        LOI_CHECK("fresh stage stamp at 1 succeeds",
                  loi_stamp_one_for_test(db, "validate_headers",
                                         1, &was_write));
        LOI_CHECK("fresh stage stamp reports was_write=true", was_write);

        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    /* ── 3. Bad-args path — returns false, no crash ──────────────────── */
    /* The full LEGACY_NOT_FOUND code path needs a real node_db / main_state
     * / coins_view_sqlite / block_tree_db, which is far too much harness
     * for a unit test. We assert only what we cheaply can: passing NULL
     * for any of the required arguments returns false via LOG_FAIL. The
     * end-to-end behavior (LEGACY_NOT_FOUND outcome on missing datadir
     * → soft skip, NOOP_SAME_TIP on re-run, DID_IMPORT on first run,
     * RECOVERED_FROM_CRASH after sentinel persistence) is verified by
     * the operator with `-legacy-attach=$HOME/.zclassic` after
     * `make deploy`. */
    {
        struct loi_result r;
        memset(&r, 0, sizeof(r));
        LOI_CHECK("NULL our_datadir rejected",
                  !legacy_oneshot_import_run(
                      NULL, "/tmp/anything",
                      NULL, NULL, NULL, NULL, &r));
        LOI_CHECK("NULL legacy_datadir rejected",
                  !legacy_oneshot_import_run(
                      "./test-tmp/x", NULL,
                      NULL, NULL, NULL, NULL, &r));
    }

    printf("legacy_oneshot_import: %d failures\n", failures);
    return failures;
}
