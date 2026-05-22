/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Unit tests for the progress_store singleton (lib/storage/src/progress_store.c).
 *
 * Coverage:
 *   - open creates <datadir>/progress.kv with WAL + stage_cursor table
 *   - second open on the same datadir is idempotent (no error, same handle)
 *   - second open on a *different* datadir is rejected (one process, one store)
 *   - close releases the singleton; reopen on a fresh path succeeds
 *   - data persisted via the F-2 stage primitive survives close + reopen
 *   - dump_state_json reports open status, path, and stage_cursor row count */

#include "test/test_helpers.h"

#include "json/json.h"
#include "storage/progress_store.h"
#include "util/blocker.h"
#include "util/stage.h"

#include <errno.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PS_CHECK(name, expr) do { \
    printf("progress_store: %s... ", (name)); \
    if ((expr)) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

/* cwd-relative tmpdir to comply with the "no /tmp" project convention. */
static void ps_tmpdir(char *buf, size_t n, const char *tag)
{
    snprintf(buf, n, "./test-tmp/progress_store_%d_%s", (int)getpid(), tag);
}

static int mkdir_p(const char *p)
{
    if (mkdir(p, 0700) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

/* Tiny stage step that advances the cursor by one each time. */
static stage_result_t step_advance_by_one(struct stage_step_ctx *c)
{
    c->cursor_out = c->cursor_in + 1;
    return STAGE_ADVANCED;
}

int test_progress_store(void)
{
    printf("\n=== progress_store tests ===\n");
    int failures = 0;

    blocker_module_init();

    /* ── open creates file + table, idempotent on same path ────────── */
    {
        char dir[256];
        ps_tmpdir(dir, sizeof(dir), "open");
        mkdir_p(dir);

        PS_CHECK("first open succeeds", progress_store_open(dir));
        PS_CHECK("handle is non-NULL", progress_store_db() != NULL);

        char fpath[512];
        snprintf(fpath, sizeof(fpath), "%s/progress.kv", dir);
        struct stat st;
        PS_CHECK("progress.kv file exists",
                 stat(fpath, &st) == 0 && S_ISREG(st.st_mode));

        sqlite3 *db1 = progress_store_db();
        PS_CHECK("second open same dir is idempotent",
                 progress_store_open(dir));
        PS_CHECK("handle unchanged after idempotent open",
                 progress_store_db() == db1);

        /* Verify the stage_cursor schema is queryable. */
        sqlite3_stmt *st_check = NULL;
        int rc = sqlite3_prepare_v2(progress_store_db(),
            "SELECT COUNT(*) FROM stage_cursor",
            -1, &st_check, NULL);
        PS_CHECK("stage_cursor table query prepares",
                 rc == SQLITE_OK);
        sqlite3_finalize(st_check);

        /* Different dir is rejected (one process, one store). */
        char dir2[256];
        ps_tmpdir(dir2, sizeof(dir2), "open_other");
        mkdir_p(dir2);
        PS_CHECK("second open with different dir is rejected",
                 !progress_store_open(dir2));

        progress_store_close();
        PS_CHECK("handle NULL after close",
                 progress_store_db() == NULL);

        test_cleanup_tmpdir(dir);
        test_cleanup_tmpdir(dir2);
    }

    /* ── cursor persistence: stage cursor survives close + reopen ──── */
    {
        char dir[256];
        ps_tmpdir(dir, sizeof(dir), "persist");
        mkdir_p(dir);

        PS_CHECK("open #1 OK", progress_store_open(dir));
        stage_t *s1 = stage_create("test-advance",
                                    step_advance_by_one, NULL);
        PS_CHECK("stage create OK", s1 != NULL);

        sqlite3 *db = progress_store_db();
        for (int i = 0; i < 5; i++) {
            PS_CHECK("advance step OK",
                     stage_run_once(s1, db) == STAGE_ADVANCED);
        }
        PS_CHECK("cursor == 5 after 5 advances",
                 stage_cursor(s1) == 5);

        stage_destroy(s1);
        progress_store_close();

        /* Reopen and verify the cursor is still 5. */
        PS_CHECK("open #2 OK (reopen)", progress_store_open(dir));
        stage_t *s2 = stage_create("test-advance",
                                    step_advance_by_one, NULL);
        /* stage_run_once will restore cursor from DB on first invocation. */
        PS_CHECK("first step after reopen advances from 5 to 6",
                 stage_run_once(s2, progress_store_db()) == STAGE_ADVANCED);
        PS_CHECK("cursor == 6 after reopen + 1 step",
                 stage_cursor(s2) == 6);

        stage_destroy(s2);
        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    /* ── dump_state_json shape ─────────────────────────────────────── */
    {
        char dir[256];
        ps_tmpdir(dir, sizeof(dir), "dump");
        mkdir_p(dir);

        char buf[1024];

        /* Closed state. */
        struct json_value v_closed;
        json_init(&v_closed);
        PS_CHECK("dump_state_json works when closed",
                 progress_store_dump_state_json(&v_closed, NULL));
        size_t n = json_write(&v_closed, buf, sizeof(buf));
        PS_CHECK("closed dump serializes", n > 0 && n < sizeof(buf));
        PS_CHECK("closed dump has open=false",
                 strstr(buf, "\"open\":false") != NULL);
        json_free(&v_closed);

        /* Open state. */
        PS_CHECK("open for dump", progress_store_open(dir));
        struct json_value v_open;
        json_init(&v_open);
        PS_CHECK("dump_state_json works when open",
                 progress_store_dump_state_json(&v_open, NULL));
        n = json_write(&v_open, buf, sizeof(buf));
        PS_CHECK("open dump serializes", n > 0 && n < sizeof(buf));
        PS_CHECK("open dump has open=true",
                 strstr(buf, "\"open\":true") != NULL);
        PS_CHECK("open dump reports stage_cursor_rows",
                 strstr(buf, "\"stage_cursor_rows\"") != NULL);
        PS_CHECK("open dump reports path",
                 strstr(buf, "progress.kv") != NULL);
        json_free(&v_open);

        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    /* ── input validation ──────────────────────────────────────────── */
    {
        PS_CHECK("open(NULL) rejected", !progress_store_open(NULL));
        PS_CHECK("open(\"\") rejected", !progress_store_open(""));
        PS_CHECK("dump(NULL) rejected",
                 !progress_store_dump_state_json(NULL, NULL));
    }

    printf("progress_store: %d failures\n", failures);
    return failures;
}
