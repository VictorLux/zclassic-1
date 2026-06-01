/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Stage primitive — implementation. See util/stage.h for design notes.
 *
 * Persistence is a single SQLite table:
 *
 *   CREATE TABLE IF NOT EXISTS stage_cursor (
 *       name TEXT PRIMARY KEY,
 *       cursor INTEGER NOT NULL,
 *       updated_at INTEGER NOT NULL
 *   );
 *
 * One row per stage. The step function runs inside an explicit
 * transaction so that output writes (which the step performs against
 * the same sqlite handle through its `user` pointer) commit atomically
 * with the cursor advance.
 *
 * Direct prepared-statement calls carry the `raw-sql-ok:kernel-primitive`
 * marker on each call site — this primitive intentionally sits below the
 * AR lifecycle because it is the foundation other models will use, and a
 * cursor row is not a model. */

#include "platform/time_compat.h"
#include "util/stage.h"

#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct stage {
    char           name[STAGE_NAME_MAX];
    stage_step_fn  step;
    void          *user;

    /* Last persisted cursor value (mirrored in stage_cursor table).
     * Read under the per-stage mutex; updated only on successful
     * commit. */
    pthread_mutex_t lock;
    uint64_t        cursor;

    _Atomic uint64_t advanced_count;
    _Atomic uint64_t blocked_count;
    _Atomic uint64_t idle_count;
    _Atomic uint64_t error_count;
};

/* ── Helpers ───────────────────────────────────────────────────────── */

const char *stage_result_name(job_result_t r)
{
    switch (r) {
    case JOB_ADVANCED: return "advanced";
    case JOB_BLOCKED:  return "blocked";
    case JOB_IDLE:     return "idle";
    case JOB_FATAL:    return "error";
    }
    return "(invalid)";
}

static int64_t wall_now_s(void)
{
    struct timespec ts;
    platform_time_realtime_timespec(&ts);
    return (int64_t)ts.tv_sec;
}

/* ── Schema bootstrap ──────────────────────────────────────────────── */

bool stage_table_ensure(sqlite3 *db)
{
    if (!db) LOG_FAIL("stage", "table_ensure: null db");
    const char *sql =
        "CREATE TABLE IF NOT EXISTS stage_cursor ("
        "  name TEXT PRIMARY KEY,"
        "  cursor INTEGER NOT NULL,"
        "  updated_at INTEGER NOT NULL"
        ")";
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "[stage] table_ensure: %s\n",  // obs-ok:stage-schema-failure
                err ? err : "(no message)");
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}

/* ── Cursor read / write (direct sqlite3) ─────────────────────────── */

/* Read the persisted cursor for `name`. On miss, *out is set to 0 and
 * the function returns true (a fresh stage starts at cursor 0). */
static bool cursor_read(sqlite3 *db, const char *name, uint64_t *out)
{
    *out = 0;
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT cursor FROM stage_cursor WHERE name = ?1";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "[stage] cursor_read prepare: %s\n",  // obs-ok:stage-prepare-failure
                sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);  // raw-sql-ok:kernel-primitive
    if (rc == SQLITE_ROW) {
        *out = (uint64_t)sqlite3_column_int64(stmt, 0);
    } else if (rc != SQLITE_DONE) {
        fprintf(stderr, "[stage] cursor_read step: rc=%d %s\n",  // obs-ok:stage-step-failure
                rc, sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return false;
    }
    sqlite3_finalize(stmt);
    return true;
}

/* UPSERT the cursor row. Caller is responsible for transaction
 * lifecycle — this function only runs the UPSERT statement. */
static bool cursor_write_locked(sqlite3 *db, const char *name, uint64_t value)
{
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "INSERT INTO stage_cursor (name, cursor, updated_at) "
        "VALUES (?1, ?2, ?3) "
        "ON CONFLICT(name) DO UPDATE SET "
        "  cursor = excluded.cursor, "
        "  updated_at = excluded.updated_at";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "[stage] cursor_write prepare: %s\n",  // obs-ok:stage-prepare-failure
                sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_text (stmt, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)value);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)wall_now_s());
    int rc = sqlite3_step(stmt);  // raw-sql-ok:kernel-primitive
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "[stage] cursor_write step: rc=%d %s\n",  // obs-ok:stage-step-failure
                rc, sqlite3_errmsg(db));
        return false;
    }
    return true;
}

/* ── Lifecycle ─────────────────────────────────────────────────────── */

stage_t *stage_create(const char *name, stage_step_fn step, void *user)
{
    if (!name || name[0] == '\0') LOG_NULL("stage", "create: empty name");
    if (!step)                    LOG_NULL("stage", "create: null step");
    size_t nlen = strlen(name);
    if (nlen >= STAGE_NAME_MAX)
        LOG_NULL("stage", "create: name too long: %zu", nlen);

    stage_t *s = zcl_calloc(1, sizeof(*s), "stage_t");
    if (!s) LOG_NULL("stage", "create: alloc failed for '%s'", name);
    memcpy(s->name, name, nlen + 1);
    s->step = step;
    s->user = user;
    pthread_mutex_init(&s->lock, NULL);
    atomic_store(&s->advanced_count, 0u);
    atomic_store(&s->blocked_count,  0u);
    atomic_store(&s->idle_count,     0u);
    atomic_store(&s->error_count,    0u);
    return s;
}

void stage_destroy(stage_t *s)
{
    if (!s) return;
    pthread_mutex_destroy(&s->lock);
    free(s);
}

const char *stage_name(const stage_t *s)          { return s ? s->name : ""; }
uint64_t    stage_cursor(const stage_t *s)
{
    if (!s) return 0;
    /* The lock guards the cursor field but a torn read of 64 bits is
     * not possible on the platforms we target; readers tolerate a
     * very-slightly-stale value, so we skip locking on the hot read
     * path. */
    return s->cursor;
}

uint64_t stage_advanced_count(const stage_t *s)
{ return s ? atomic_load(&s->advanced_count) : 0u; }
uint64_t stage_blocked_count(const stage_t *s)
{ return s ? atomic_load(&s->blocked_count) : 0u; }
uint64_t stage_idle_count(const stage_t *s)
{ return s ? atomic_load(&s->idle_count) : 0u; }
uint64_t stage_error_count(const stage_t *s)
{ return s ? atomic_load(&s->error_count) : 0u; }

/* ── Run one step ──────────────────────────────────────────────────── */

job_result_t stage_run_once(stage_t *s, sqlite3 *db)
{
    if (!s || !db) {
        fprintf(stderr, "[stage] run_once: null arg s=%p db=%p\n",  // obs-ok:stage-arg-failure
                (void *)s, (void *)db);
        if (s) atomic_fetch_add(&s->error_count, 1u);
        return JOB_FATAL;
    }

    pthread_mutex_lock(&s->lock);

    /* Load the persisted cursor before invoking the step. We don't
     * trust the cached `s->cursor` because a sibling process (cold
     * import) could have updated it; reading inside the txn gives us
     * the freshest value visible to this connection. */
    uint64_t cur = 0;
    if (!cursor_read(db, s->name, &cur)) {
        pthread_mutex_unlock(&s->lock);
        atomic_fetch_add(&s->error_count, 1u);
        return JOB_FATAL;
    }
    s->cursor = cur;

    struct stage_step_ctx ctx = {
        .cursor_in  = cur,
        .cursor_out = cur,
        .user       = s->user,
    };
    /* Caller-prepared blocker_record fields start zero. */
    memset(&ctx.blocker, 0, sizeof(ctx.blocker));

    /* Begin an explicit transaction so the step's writes + our cursor
     * UPSERT land atomically. If the step itself wants to do bulk I/O
     * outside the transaction it must roll its own; the contract here
     * is "cursor advance and step output are atomic." */
    char *err = NULL;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "[stage] BEGIN: %s\n",  // obs-ok:stage-begin-failure
                err ? err : "(no message)");
        if (err) sqlite3_free(err);
        pthread_mutex_unlock(&s->lock);
        atomic_fetch_add(&s->error_count, 1u);
        return JOB_FATAL;
    }

    job_result_t r = s->step(&ctx);

    if (r == JOB_ADVANCED) {
        if (ctx.cursor_out <= cur) {
            /* Contract violation: JOB_ADVANCED must move the cursor
             * forward. Roll back to keep the invariant intact. */
            fprintf(stderr,  // obs-ok:stage-advance-noop
                "[stage] %s: ADVANCED but cursor_out(%llu) <= cursor_in(%llu); rolling back\n",
                s->name,
                (unsigned long long)ctx.cursor_out,
                (unsigned long long)cur);
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            pthread_mutex_unlock(&s->lock);
            atomic_fetch_add(&s->error_count, 1u);
            return JOB_FATAL;
        }
        if (!cursor_write_locked(db, s->name, ctx.cursor_out)) {
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            pthread_mutex_unlock(&s->lock);
            atomic_fetch_add(&s->error_count, 1u);
            return JOB_FATAL;
        }
        if (sqlite3_exec(db, "COMMIT", NULL, NULL, &err) != SQLITE_OK) {
            fprintf(stderr, "[stage] COMMIT: %s\n",  // obs-ok:stage-commit-failure
                    err ? err : "(no message)");
            if (err) sqlite3_free(err);
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            pthread_mutex_unlock(&s->lock);
            atomic_fetch_add(&s->error_count, 1u);
            return JOB_FATAL;
        }
        s->cursor = ctx.cursor_out;
        atomic_fetch_add(&s->advanced_count, 1u);
        pthread_mutex_unlock(&s->lock);
        return JOB_ADVANCED;
    }

    /* Non-advancing outcomes: roll back the txn (the step may have
     * touched scratch state) and surface the result. */
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    pthread_mutex_unlock(&s->lock);

    if (r == JOB_BLOCKED) {
        /* Record the blocker if the step filled out an id. Empty id
         * means the step signalled BLOCKED but didn't fill the record
         * — log and treat as ERROR so the caller is not misled. */
        if (ctx.blocker.id[0] == '\0') {
            fprintf(stderr, "[stage] %s: BLOCKED with empty id\n",  // obs-ok:stage-blocked-empty
                    s->name);
            atomic_fetch_add(&s->error_count, 1u);
            return JOB_FATAL;
        }
        if (ctx.blocker.owner_subsystem[0] == '\0') {
            /* Owner field is bounded; truncate the (longer) stage name
             * with explicit precision so -Wformat-truncation is happy. */
            snprintf(ctx.blocker.owner_subsystem,
                     sizeof(ctx.blocker.owner_subsystem),
                     "%.*s",
                     (int)(sizeof(ctx.blocker.owner_subsystem) - 1),
                     s->name);
        }
        (void)blocker_set(&ctx.blocker);
        atomic_fetch_add(&s->blocked_count, 1u);
        return JOB_BLOCKED;
    }

    if (r == JOB_IDLE) {
        atomic_fetch_add(&s->idle_count, 1u);
        return JOB_IDLE;
    }

    /* JOB_FATAL or any other value */
    atomic_fetch_add(&s->error_count, 1u);
    return JOB_FATAL;
}

/* ── Boot-time restore ─────────────────────────────────────────────── */

bool stage_set_cursor(stage_t *s, sqlite3 *db, uint64_t value)
{
    if (!s || !db) LOG_FAIL("stage", "set_cursor: null arg");
    pthread_mutex_lock(&s->lock);
    char *err = NULL;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "[stage] set_cursor BEGIN: %s\n",  // obs-ok:stage-begin-failure
                err ? err : "(no message)");
        if (err) sqlite3_free(err);
        pthread_mutex_unlock(&s->lock);
        return false;
    }
    if (!cursor_write_locked(db, s->name, value)) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        pthread_mutex_unlock(&s->lock);
        return false;
    }
    if (sqlite3_exec(db, "COMMIT", NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "[stage] set_cursor COMMIT: %s\n",  // obs-ok:stage-commit-failure
                err ? err : "(no message)");
        if (err) sqlite3_free(err);
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        pthread_mutex_unlock(&s->lock);
        return false;
    }
    s->cursor = value;
    pthread_mutex_unlock(&s->lock);
    return true;
}

bool stage_set_named_cursor_if_behind(sqlite3 *db, const char *name,
                                      uint64_t value)
{
    if (!db || !name || name[0] == '\0')
        LOG_FAIL("stage", "set_named_cursor: invalid arg db=%p name=%p",
                 (void *)db, (const void *)name);

    if (!stage_table_ensure(db))
        return false;

    char *err = NULL;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "[stage] set_named_cursor BEGIN: %s\n",  // obs-ok:stage-begin-failure
                err ? err : "(no message)");
        if (err) sqlite3_free(err);
        return false;
    }

    uint64_t current = 0;
    if (!cursor_read(db, name, &current)) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return false;
    }
    if (current >= value) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return true;
    }

    if (!cursor_write_locked(db, name, value)) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return false;
    }
    if (sqlite3_exec(db, "COMMIT", NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "[stage] set_named_cursor COMMIT: %s\n",  // obs-ok:stage-commit-failure
                err ? err : "(no message)");
        if (err) sqlite3_free(err);
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return false;
    }
    return true;
}
