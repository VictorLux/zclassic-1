/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * progress_store — implementation. See storage/progress_store.h.
 *
 * One handle, one path, opened at boot once. The handle lives behind
 * an atomic pointer so `progress_store_db()` is a relaxed-atomic load
 * with no mutex; opens and closes serialise on a one-shot init mutex.
 *
 * Direct sqlite3_exec / sqlite3_step calls here carry the kernel-
 * primitive marker because, like the stage primitive itself, this
 * module sits below the AR lifecycle — the stage_cursor row is not a
 * model. */

#include "storage/progress_store.h"

#include "json/json.h"
#include "util/log_macros.h"
#include "util/stage.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define PROGRESS_STORE_FILENAME  "progress.kv"
#define PROGRESS_STORE_PATH_MAX  1024

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic(sqlite3 *) g_db = NULL;
static char g_path[PROGRESS_STORE_PATH_MAX];
static int64_t g_opened_at;

static int64_t wall_now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec;
}

/* Apply WAL + reasonable durability/recovery pragmas. Errors here are
 * fatal for the open (caller will close & fail). */
static bool apply_pragmas(sqlite3 *db)
{
    static const char *const pragmas[] = {
        "PRAGMA journal_mode=WAL",
        "PRAGMA synchronous=NORMAL",
        "PRAGMA foreign_keys=ON",
        "PRAGMA busy_timeout=5000",
        NULL,
    };
    for (size_t i = 0; pragmas[i]; i++) {
        char *err = NULL;
        if (sqlite3_exec(db, pragmas[i], NULL, NULL, &err) != SQLITE_OK) {
            fprintf(stderr,  // obs-ok:progress-store-open-failure
                    "[progress_store] pragma failed (%s): %s\n",
                    pragmas[i], err ? err : "(no message)");
            if (err) sqlite3_free(err);
            return false;
        }
    }
    return true;
}

bool progress_store_open(const char *datadir)
{
    if (!datadir || !datadir[0]) LOG_FAIL("progress_store",
        "open: empty datadir");

    char path[PROGRESS_STORE_PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/%s",
                     datadir, PROGRESS_STORE_FILENAME);
    if (n <= 0 || (size_t)n >= sizeof(path))
        LOG_FAIL("progress_store", "open: datadir path too long");

    pthread_mutex_lock(&g_lock);

    /* Already open with this path → idempotent success. */
    if (atomic_load_explicit(&g_db, memory_order_relaxed) != NULL) {
        bool same = (strcmp(g_path, path) == 0);
        pthread_mutex_unlock(&g_lock);
        if (!same)
            LOG_FAIL("progress_store",
                "open: already opened at a different path (%s vs %s)",
                g_path, path);
        return true;
    }

    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(path, &db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr,  // obs-ok:progress-store-open-failure
                "[progress_store] sqlite3_open_v2(%s) failed: %s\n",
                path, db ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
        if (db) sqlite3_close(db);
        pthread_mutex_unlock(&g_lock);
        return false;
    }

    if (!apply_pragmas(db) || !stage_table_ensure(db)) {
        sqlite3_close(db);
        pthread_mutex_unlock(&g_lock);
        return false;
    }

    snprintf(g_path, sizeof(g_path), "%s", path);
    g_opened_at = wall_now_s();
    atomic_store_explicit(&g_db, db, memory_order_release);

    pthread_mutex_unlock(&g_lock);

    fprintf(stderr,  // obs-ok:progress-store-lifecycle
            "[progress_store] opened %s (WAL)\n", path);
    return true;
}

sqlite3 *progress_store_db(void)
{
    return atomic_load_explicit(&g_db, memory_order_acquire);
}

void progress_store_close(void)
{
    pthread_mutex_lock(&g_lock);
    sqlite3 *db = atomic_exchange_explicit(&g_db, NULL,
                                            memory_order_acq_rel);
    if (!db) {
        pthread_mutex_unlock(&g_lock);
        return;
    }

    /* Best-effort checkpoint so the WAL truncates cleanly. Failures
     * here are not fatal — sqlite3_close still runs. */
    char *err = NULL;
    if (sqlite3_exec(db, "PRAGMA wal_checkpoint(TRUNCATE)",
                     NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr,  // obs-ok:progress-store-lifecycle
                "[progress_store] checkpoint on close: %s\n",
                err ? err : "(no message)");
    }
    if (err) sqlite3_free(err);

    int rc = sqlite3_close(db);
    if (rc != SQLITE_OK) {
        fprintf(stderr,  // obs-ok:progress-store-lifecycle
                "[progress_store] sqlite3_close: rc=%d (%s)\n",
                rc, sqlite3_errstr(rc));
    } else {
        fprintf(stderr,  // obs-ok:progress-store-lifecycle
                "[progress_store] closed %s\n", g_path);
    }

    g_path[0] = '\0';
    g_opened_at = 0;
    pthread_mutex_unlock(&g_lock);
}

static int64_t stage_cursor_count(sqlite3 *db)
{
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM stage_cursor",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    int64_t n = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW)  // raw-sql-ok:kernel-primitive
        n = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return n;
}

bool progress_store_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out) return false;
    json_set_object(out);

    sqlite3 *db = progress_store_db();
    json_push_kv_bool(out, "open", db != NULL);
    json_push_kv_str (out, "path", g_path);
    json_push_kv_int (out, "opened_at", g_opened_at);

    if (db) {
        json_push_kv_int(out, "stage_cursor_rows",
                         stage_cursor_count(db));
    } else {
        json_push_kv_int(out, "stage_cursor_rows", 0);
    }
    return true;
}
