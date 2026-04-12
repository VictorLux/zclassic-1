/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Database Maintenance Scheduler — see header for rationale.
 *
 * Implementation strategy
 * -----------------------
 * Each of the three operations has an independent last-run
 * timestamp stored in the service state. The background thread
 * ticks every `tick_seconds` and, for each op, checks
 * `now - last_run >= interval`. When true it runs the op via
 * the same `db_maintenance_run_now()` path that synchronous
 * callers use, so failure reporting is unified.
 *
 * SQLite commands:
 *   wal     → "PRAGMA wal_checkpoint(TRUNCATE);"
 *             Truncates the WAL back to zero after flushing all
 *             committed frames into the main file. Cheap.
 *   analyze → "ANALYZE;"
 *             Rebuilds the sqlite_stat1 table used by the query
 *             planner. Cheap on databases with a few 10s of MB.
 *   vacuum  → "VACUUM;"
 *             Rebuilds the whole file into a new file, copies
 *             across, and replaces it. Holds the DB lock for
 *             the duration — can take minutes for a GB db.
 *             Only run when the caller-supplied gate says OK.
 *
 * Thread safety
 * -------------
 * The scheduler owns a mutex guarding lifecycle state and the
 * last-run timestamps. `run_now` takes the same mutex so a
 * synchronous caller and the scheduler never race on the same
 * op. SQLite calls in `run_now` happen with the mutex held —
 * that's intentional: if the scheduler is mid-vacuum and a test
 * calls run_now("analyze"), the analyze waits for the vacuum
 * rather than fighting over the db handle.
 */

#include "services/db_maintenance.h"

#include "event/event.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include <sys/stat.h>

#include <sqlite3.h>

#include "util/log_macros.h"

/* ── Module state ───────────────────────────────────────────── */

struct db_maintenance_state {
    pthread_mutex_t lock;
    pthread_t       thread;
    bool            thread_running;
    bool            stop_requested;

    struct node_db *db;
    struct db_maintenance_schedule sched;

    /* Resolved schedule (defaults applied). */
    int wal_minutes;
    int analyze_hours;
    int vacuum_days;
    int tick_seconds;
    int64_t wal_max_bytes;

    /* Last-run timestamps (UNIX seconds). 0 = never run. */
    int64_t wal_last_unix;
    int64_t wal_last_duration_ms;
    int64_t analyze_last_unix;
    int64_t analyze_last_duration_ms;
    int64_t vacuum_last_unix;
    int64_t vacuum_last_duration_ms;

    int64_t total_runs;
    int64_t total_failures;
    char    last_error[256];

    db_maintenance_vacuum_gate_fn vacuum_gate;
};

static struct db_maintenance_state g_dbm = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

/* ── Defaults ───────────────────────────────────────────────── */

void db_maintenance_schedule_defaults(struct db_maintenance_schedule *s)
{
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->wal_checkpoint_minutes = DB_MAINT_DEFAULT_WAL_MINUTES;
    s->analyze_hours          = DB_MAINT_DEFAULT_ANALYZE_HOURS;
    s->vacuum_days            = DB_MAINT_DEFAULT_VACUUM_DAYS;
    s->tick_seconds           = 60;
}

void db_maintenance_status_snapshot(struct db_maintenance_status *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&g_dbm.lock);
    out->running                  = g_dbm.thread_running;
    out->wal_last_unix            = g_dbm.wal_last_unix;
    out->wal_last_duration_ms     = g_dbm.wal_last_duration_ms;
    out->analyze_last_unix        = g_dbm.analyze_last_unix;
    out->analyze_last_duration_ms = g_dbm.analyze_last_duration_ms;
    out->vacuum_last_unix         = g_dbm.vacuum_last_unix;
    out->vacuum_last_duration_ms  = g_dbm.vacuum_last_duration_ms;
    out->total_runs               = g_dbm.total_runs;
    out->total_failures           = g_dbm.total_failures;
    snprintf(out->last_error, sizeof(out->last_error),
             "%s", g_dbm.last_error);
    pthread_mutex_unlock(&g_dbm.lock);
}

void db_maintenance_set_vacuum_gate(db_maintenance_vacuum_gate_fn fn)
{
    pthread_mutex_lock(&g_dbm.lock);
    g_dbm.vacuum_gate = fn;
    pthread_mutex_unlock(&g_dbm.lock);
}

/* ── Helpers ────────────────────────────────────────────────── */

static int64_t dbm_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int64_t dbm_now_unix(void)
{
    return (int64_t)time(NULL);
}

/* Maps an op name to the SQL string to execute. NULL for an
 * unknown op. Caller validates. */
static const char *dbm_sql_for_op(const char *op)
{
    if (!op) return NULL;
    if (strcmp(op, "wal")     == 0) return "PRAGMA wal_checkpoint(TRUNCATE);";
    if (strcmp(op, "analyze") == 0) return "ANALYZE;";
    if (strcmp(op, "vacuum")  == 0) return "VACUUM;";
    return NULL;
}

/* Update the per-op last-run state after a successful run.
 * Assumes g_dbm.lock is held. */
static void dbm_note_run_locked(const char *op,
                                 int64_t unix_now, int64_t duration_ms)
{
    if (strcmp(op, "wal") == 0) {
        g_dbm.wal_last_unix        = unix_now;
        g_dbm.wal_last_duration_ms = duration_ms;
    } else if (strcmp(op, "analyze") == 0) {
        g_dbm.analyze_last_unix        = unix_now;
        g_dbm.analyze_last_duration_ms = duration_ms;
    } else if (strcmp(op, "vacuum") == 0) {
        g_dbm.vacuum_last_unix        = unix_now;
        g_dbm.vacuum_last_duration_ms = duration_ms;
    }
}

/* ── run_now ────────────────────────────────────────────────── */

bool db_maintenance_run_now(struct node_db *db, const char *op)
{
    if (!db || !db->open || !db->db) LOG_FAIL("db_maint", "run_now called with null or closed db");
    const char *sql = dbm_sql_for_op(op);
    if (!sql) LOG_FAIL("db_maint", "unknown maintenance op: %s", op ? op : "(null)");

    pthread_mutex_lock(&g_dbm.lock);

    event_emitf(EV_DB_MAINTENANCE_START, 0, "op=%s", op);

    int64_t start_ms = dbm_now_ms();
    char *errmsg = NULL;
    int rc = sqlite3_exec(db->db, sql, NULL, NULL, &errmsg);
    int64_t elapsed_ms = dbm_now_ms() - start_ms;

    if (rc != SQLITE_OK) {
        g_dbm.total_failures++;
        snprintf(g_dbm.last_error, sizeof(g_dbm.last_error),
                 "op=%s %s", op, errmsg ? errmsg : "sqlite error");
        event_emitf(EV_DB_MAINTENANCE_FAILED, 0,
                    "op=%s reason=%s", op,
                    errmsg ? errmsg : "sqlite error");
        sqlite3_free(errmsg);
        pthread_mutex_unlock(&g_dbm.lock);
        return false;
    }

    dbm_note_run_locked(op, dbm_now_unix(), elapsed_ms);
    g_dbm.total_runs++;
    g_dbm.last_error[0] = '\0';

    event_emitf(EV_DB_MAINTENANCE_DONE, 0,
                "op=%s elapsed_ms=%" PRId64,
                op, elapsed_ms);
    pthread_mutex_unlock(&g_dbm.lock);
    return true;
}

/* ── Thread loop ────────────────────────────────────────────── */

static void dbm_sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* Returns true if `last_unix == 0` (never run) or the interval
 * has elapsed since the last run. */
static bool dbm_due(int64_t last_unix, int64_t interval_seconds)
{
    if (last_unix == 0) return true;
    return (dbm_now_unix() - last_unix) >= interval_seconds;
}

/* Returns the WAL file size in bytes, or 0 if unavailable. */
static int64_t dbm_wal_size(struct node_db *db)
{
    if (!db || !db->open || !db->db) return 0;
    const char *db_path = sqlite3_db_filename(db->db, "main");
    if (!db_path) return 0;
    char wal_path[1024];
    snprintf(wal_path, sizeof(wal_path), "%s-wal", db_path);
    struct stat st;
    if (stat(wal_path, &st) != 0) return 0;
    return (int64_t)st.st_size;
}

static void *dbm_thread_fn(void *arg)
{
    (void)arg;
    while (true) {
        pthread_mutex_lock(&g_dbm.lock);
        bool stop = g_dbm.stop_requested;
        struct node_db *db = g_dbm.db;
        int wal_sec     = g_dbm.wal_minutes  * 60;
        int analyze_sec = g_dbm.analyze_hours * 3600;
        int vacuum_sec  = g_dbm.vacuum_days   * 86400;
        int64_t wal_last     = g_dbm.wal_last_unix;
        int64_t analyze_last = g_dbm.analyze_last_unix;
        int64_t vacuum_last  = g_dbm.vacuum_last_unix;
        db_maintenance_vacuum_gate_fn gate = g_dbm.vacuum_gate;
        int tick = g_dbm.tick_seconds;
        int64_t wal_cap = g_dbm.wal_max_bytes;
        pthread_mutex_unlock(&g_dbm.lock);

        if (stop) break;

        if (db) {
            /* WAL size cap: force checkpoint regardless of interval
             * when WAL exceeds the configured byte limit. */
            bool wal_over_cap = (wal_cap > 0 && dbm_wal_size(db) > wal_cap);
            if (wal_over_cap || dbm_due(wal_last, wal_sec))
                (void)db_maintenance_run_now(db, "wal");
            if (dbm_due(analyze_last, analyze_sec))
                (void)db_maintenance_run_now(db, "analyze");
            if (dbm_due(vacuum_last, vacuum_sec)) {
                bool may_vacuum = gate ? gate() : false;
                if (may_vacuum)
                    (void)db_maintenance_run_now(db, "vacuum");
            }
        }

        /* Sleep in 200ms increments so stop_requested is honoured
         * quickly without waiting a full tick. */
        int total_ms = tick > 0 ? tick * 1000 : 60000;
        int slept = 0;
        while (slept < total_ms) {
            pthread_mutex_lock(&g_dbm.lock);
            bool st = g_dbm.stop_requested;
            pthread_mutex_unlock(&g_dbm.lock);
            if (st) break;
            dbm_sleep_ms(200);
            slept += 200;
        }
    }

    pthread_mutex_lock(&g_dbm.lock);
    g_dbm.thread_running = false;
    pthread_mutex_unlock(&g_dbm.lock);
    return NULL;
}

/* ── Lifecycle ──────────────────────────────────────────────── */

bool db_maintenance_start(struct node_db *db,
                           const struct db_maintenance_schedule *s)
{
    if (!db || !db->open || !db->db || !s) LOG_FAIL("db_maint", "start called with null db or schedule");

    pthread_mutex_lock(&g_dbm.lock);
    if (g_dbm.thread_running) {
        pthread_mutex_unlock(&g_dbm.lock);
        LOG_FAIL("db_maint", "start called but maintenance thread already running");
    }

    g_dbm.db    = db;
    g_dbm.sched = *s;
    g_dbm.wal_minutes   = s->wal_checkpoint_minutes > 0
        ? s->wal_checkpoint_minutes : DB_MAINT_DEFAULT_WAL_MINUTES;
    g_dbm.analyze_hours = s->analyze_hours > 0
        ? s->analyze_hours : DB_MAINT_DEFAULT_ANALYZE_HOURS;
    g_dbm.vacuum_days   = s->vacuum_days > 0
        ? s->vacuum_days : DB_MAINT_DEFAULT_VACUUM_DAYS;
    g_dbm.tick_seconds  = s->tick_seconds > 0 ? s->tick_seconds : 60;

    /* WAL size cap: schedule value, env override, then default. */
    g_dbm.wal_max_bytes = s->wal_max_bytes > 0
        ? s->wal_max_bytes : DB_MAINT_DEFAULT_WAL_MAX_BYTES;
    const char *env_wal = getenv("ZCL_WAL_MAX_BYTES");
    if (env_wal) {
        int64_t v = strtoll(env_wal, NULL, 10);
        if (v > 0)
            g_dbm.wal_max_bytes = v;
        else if (v == 0)
            g_dbm.wal_max_bytes = 0;  /* disable cap */
    }

    g_dbm.stop_requested = false;
    g_dbm.thread_running = true;

    int rc = pthread_create(&g_dbm.thread, NULL, dbm_thread_fn, NULL);
    if (rc != 0) {
        g_dbm.thread_running = false;
        pthread_mutex_unlock(&g_dbm.lock);
        fprintf(stderr, "db_maintenance: pthread_create failed (%d)\n", rc);
        return false;
    }
    pthread_mutex_unlock(&g_dbm.lock);
    return true;
}

void db_maintenance_stop(void)
{
    pthread_t th;
    bool joinable = false;

    pthread_mutex_lock(&g_dbm.lock);
    if (g_dbm.thread_running) {
        g_dbm.stop_requested = true;
        th = g_dbm.thread;
        joinable = true;
    }
    pthread_mutex_unlock(&g_dbm.lock);

    if (joinable) {
        pthread_join(th, NULL);
        pthread_mutex_lock(&g_dbm.lock);
        g_dbm.thread_running = false;
        g_dbm.stop_requested = false;
        g_dbm.db = NULL;
        pthread_mutex_unlock(&g_dbm.lock);
    }
}
