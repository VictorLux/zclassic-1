/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Wallet Backup Service — see header for rationale.
 *
 * Implementation strategy
 * -----------------------
 *
 * SQLite's online backup API (sqlite3_backup_init) copies the
 * whole database. We want only the wallet tables, so instead we
 * open the destination file as a fresh DB, ATTACH the source
 * via its on-disk path, and `CREATE TABLE <name> AS SELECT * FROM
 * src.<name>` for each wallet table. That keeps the destination
 * file small (users typically have a handful of wallet rows, not
 * the full 3M-row blocks table) and avoids copying UTXO data
 * that would leak peer-observable chain state to the backup.
 *
 * The ATTACH path must be absolute — sqlite3_db_filename returns
 * it for an opened connection, so we read that off the source
 * handle at backup time instead of asking the caller to thread
 * it through the config.
 *
 * Row-count verification
 * ----------------------
 *
 * After the CREATE TABLE AS SELECT statements run, we reopen the
 * destination in a second connection and count wallet_keys rows.
 * That round-trip proves the file is readable, the schema is as
 * expected, and the same number of keys landed as we thought we
 * wrote. Mismatches set last_error and emit
 * EV_WALLET_BACKUP_FAILED; the file is NOT deleted — operators
 * need the bytes even when verification fails.
 */

#include "platform/time_compat.h"
#include "services/wallet_backup_service.h"

#include "event/event.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <sqlite3.h>

#include "util/ar_step_readonly.h"
#include "util/log_macros.h"
#include "util/thread_registry.h"

/* ── Wallet table list ──────────────────────────────────────── */

static const char *const WALLET_TABLES[] = {
    "wallet_keys",
    "wallet_sapling_keys",
    "wallet_seed",
    "wallet_scripts",
    "wallet_transactions",
    "wallet_utxos",
    "wallet_sapling_notes",
};

#define WALLET_TABLE_COUNT (sizeof(WALLET_TABLES) / sizeof(WALLET_TABLES[0]))

/* ── Module state ───────────────────────────────────────────── */

struct wallet_backup_service_state {
    pthread_mutex_t lock;
    pthread_t       thread;
    bool            thread_running;
    bool            stop_requested;

    struct wallet_backup_config cfg;
    struct node_db             *db;

    /* Snapshot counters */
    int64_t total_runs;
    int64_t total_failures;
    int64_t last_run_unix;
    int64_t last_size_bytes;
    int64_t last_key_count;
    int64_t last_duration_ms;
    char    last_path[512];
    char    last_error[256];

    /* Debounced event trigger (D4: plan §5.4).
     * Set by wallet_backup_service_on_key_change; cleared by the
     * thread after running a debounce-eligible backup. */
    bool    key_change_pending;
    int64_t total_triggers;     /* total on_key_change calls (all, incl. coalesced) */
    int64_t total_trigger_runs; /* backups that actually ran due to a trigger */
};

static struct wallet_backup_service_state g_wbs = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

/* ── Helpers ────────────────────────────────────────────────── */

static int64_t wbs_now_ms(void)
{
    struct timespec ts;
    platform_time_monotonic_timespec(&ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int64_t wbs_now_unix(void)
{
    return (int64_t)platform_time_wall_time_t();
}

void wallet_backup_config_defaults(struct wallet_backup_config *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->interval_seconds = WALLET_BACKUP_DEFAULT_INTERVAL_SEC;
    cfg->max_versions     = WALLET_BACKUP_DEFAULT_MAX_VERSIONS;
    cfg->encrypt          = false;
}

void wallet_backup_status_snapshot(struct wallet_backup_status *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&g_wbs.lock);
    out->running          = g_wbs.thread_running;
    out->total_runs       = g_wbs.total_runs;
    out->total_failures   = g_wbs.total_failures;
    out->last_run_unix    = g_wbs.last_run_unix;
    out->last_size_bytes  = g_wbs.last_size_bytes;
    out->last_key_count   = g_wbs.last_key_count;
    out->last_duration_ms = g_wbs.last_duration_ms;
    snprintf(out->last_path,  sizeof(out->last_path),  "%s", g_wbs.last_path);
    snprintf(out->last_error, sizeof(out->last_error), "%s", g_wbs.last_error);
    pthread_mutex_unlock(&g_wbs.lock);
}

/* Create backup_dir with mode 0700 if missing. Returns true if the
 * directory exists on successful return. */
static bool wbs_ensure_backup_dir(const char *dir)
{
    if (!dir || !*dir) LOG_FAIL("wallet_backup", "backup dir is NULL or empty");
    struct stat st;
    if (stat(dir, &st) == 0)
        return S_ISDIR(st.st_mode);
    if (mkdir(dir, 0700) != 0) {
        fprintf(stderr, "wallet_backup: mkdir %s: %s\n",
                dir, strerror(errno));
        return false;
    }
    return true;
}

/* Return the on-disk path backing the source sqlite connection.
 * Returns NULL for memory databases. */
static const char *wbs_source_path(sqlite3 *src)
{
    if (!src) LOG_NULL("wallet_backup", "source_path: NULL db handle");
    const char *p = sqlite3_db_filename(src, "main");
    if (!p || !*p) LOG_NULL("wallet_backup", "source_path: db has no file path (in-memory?)");
    return p;
}

/* SHA-style filename: wallet_backup_<unix_ts>_<usec>.sqlite. The
 * usec disambiguates rapid successive runs (tests call
 * run_once several times back-to-back). */
static void wbs_build_backup_path(const char *dir, char *out, size_t cap)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    snprintf(out, cap, "%s/%s%lld_%06ld%s",
             dir,
             WALLET_BACKUP_FILENAME_PREFIX,
             (long long)tv.tv_sec,
             (long)tv.tv_usec,
             WALLET_BACKUP_FILENAME_SUFFIX);
}

/* ── Row-count reader ──────────────────────────────────────── */

static int64_t wbs_count_rows(sqlite3 *db, const char *table)
{
    if (!db || !table) LOG_ERR("wallet_backup", "count_rows: NULL db or table");
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT count(*) FROM %s", table);
    sqlite3_stmt *st = NULL;
    int64_t n = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        if (AR_STEP_ROW_READONLY(st) == SQLITE_ROW)
            n = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    return n;
}

/* ── Core primitive ─────────────────────────────────────────── */

bool wallet_backup_run_once(const char *backup_dir,
                             struct node_db *db,
                             char *out_path, size_t out_path_cap,
                             int64_t *out_key_count,
                             char *err_out, size_t err_cap)
{
    if (err_out && err_cap) err_out[0] = '\0';
    if (out_path && out_path_cap) out_path[0] = '\0';
    if (out_key_count) *out_key_count = -1;

    if (!backup_dir || !db || !db->open || !db->db) {
        if (err_out) snprintf(err_out, err_cap, "null arg or db not open");
        return false;
    }

    if (!wbs_ensure_backup_dir(backup_dir)) {
        if (err_out) snprintf(err_out, err_cap,
                "cannot create backup_dir %s", backup_dir);
        return false;
    }

    const char *src_path = wbs_source_path(db->db);
    if (!src_path) {
        if (err_out) snprintf(err_out, err_cap,
                "source db has no file path (in-memory?)");
        return false;
    }

    /* In-memory source is valid for tests: use the ATTACH TO
     * "file::memory:?cache=shared" form only if the caller opened
     * it with a real filename. Here we simply require a disk file
     * — tests that want to exercise the primitive use a tmpdir. */

    char dst_path[640];
    wbs_build_backup_path(backup_dir, dst_path, sizeof(dst_path));

    /* Open the destination as a fresh empty db. */
    sqlite3 *dst = NULL;
    int rc = sqlite3_open_v2(dst_path, &dst,
        SQLITE_OPEN_CREATE | SQLITE_OPEN_READWRITE, NULL);
    if (rc != SQLITE_OK) {
        if (err_out) snprintf(err_out, err_cap,
                "sqlite3_open dst: %s", sqlite3_errmsg(dst));
        if (dst) sqlite3_close(dst);
        unlink(dst_path);
        return false;
    }

    /* ATTACH the source by absolute path under alias "src". */
    {
        sqlite3_stmt *att = NULL;
        rc = sqlite3_prepare_v2(dst,
            "ATTACH DATABASE ? AS src", -1, &att, NULL);
        if (rc != SQLITE_OK || !att) {
            if (err_out) snprintf(err_out, err_cap,
                    "prepare ATTACH: %s", sqlite3_errmsg(dst));
            if (att) sqlite3_finalize(att);
            sqlite3_close(dst);
            unlink(dst_path);
            return false;
        }
        sqlite3_bind_text(att, 1, src_path, -1, SQLITE_STATIC);
        if (AR_STEP_ROW_READONLY(att) != SQLITE_DONE) {
            if (err_out) snprintf(err_out, err_cap,
                    "step ATTACH: %s", sqlite3_errmsg(dst));
            sqlite3_finalize(att);
            sqlite3_close(dst);
            unlink(dst_path);
            return false;
        }
        sqlite3_finalize(att);
    }

    /* For each wallet table, run CREATE TABLE IF NOT EXISTS t AS
     * SELECT ... The AS SELECT form copies both schema and rows
     * in one statement; if the source table is missing we just
     * skip it (older databases may not have every table). */
    char *errmsg = NULL;
    bool all_ok = true;
    for (size_t i = 0; i < WALLET_TABLE_COUNT; i++) {
        const char *table = WALLET_TABLES[i];
        /* Check the source even has this table. */
        char exists_sql[256];
        snprintf(exists_sql, sizeof(exists_sql),
            "SELECT name FROM src.sqlite_master "
            "WHERE type='table' AND name='%s'", table);
        sqlite3_stmt *chk = NULL;
        bool src_has = false;
        if (sqlite3_prepare_v2(dst, exists_sql, -1, &chk, NULL) == SQLITE_OK) {
            src_has = AR_STEP_ROW_READONLY(chk) == SQLITE_ROW;
            sqlite3_finalize(chk);
        }
        if (!src_has) continue;

        char sql[256];
        snprintf(sql, sizeof(sql),
            "CREATE TABLE %s AS SELECT * FROM src.%s", table, table);
        rc = sqlite3_exec(dst, sql, NULL, NULL, &errmsg);
        if (rc != SQLITE_OK) {
            if (err_out) snprintf(err_out, err_cap,
                    "copy %s: %s", table, errmsg ? errmsg : "?");
            sqlite3_free(errmsg);
            errmsg = NULL;
            all_ok = false;
            break;
        }
    }

    /* Detach + close. */
    (void)sqlite3_exec(dst, "DETACH DATABASE src", NULL, NULL, NULL);
    sqlite3_close(dst);

    if (!all_ok) {
        /* Leave the dst file on disk for forensics, but emit the
         * failure event and bail out. */
        struct stat st;
        int64_t bytes = stat(dst_path, &st) == 0 ? (int64_t)st.st_size : -1;
        event_emitf(EV_WALLET_BACKUP_FAILED, 0,
                    "path=%s bytes=%lld reason=%s",
                    dst_path, (long long)bytes,
                    err_out ? err_out : "unknown");
        return false;
    }

    /* Round-trip verification: reopen the backup file read-only,
     * count the wallet_keys rows, and compare against the source.
     * If the counts differ the file is left on disk but we return
     * false so the caller knows the output is not usable. */
    int64_t src_key_count = wbs_count_rows(db->db, "wallet_keys");
    int64_t dst_key_count = -1;
    {
        sqlite3 *verify = NULL;
        if (sqlite3_open_v2(dst_path, &verify,
                SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) {
            dst_key_count = wbs_count_rows(verify, "wallet_keys");
            sqlite3_close(verify);
        }
    }

    if (dst_key_count < 0 || dst_key_count != src_key_count) {
        if (err_out) snprintf(err_out, err_cap,
                "verify row count mismatch src=%lld dst=%lld",
                (long long)src_key_count, (long long)dst_key_count);
        event_emitf(EV_WALLET_BACKUP_FAILED, 0,
                    "path=%s reason=%s",
                    dst_path, err_out ? err_out : "count_mismatch");
        return false;
    }

    struct stat st;
    int64_t bytes = stat(dst_path, &st) == 0 ? (int64_t)st.st_size : -1;
    event_emitf(EV_WALLET_BACKUP, 0,
                "path=%s bytes=%lld keys=%lld",
                dst_path, (long long)bytes, (long long)dst_key_count);

    if (out_path) snprintf(out_path, out_path_cap, "%s", dst_path);
    if (out_key_count) *out_key_count = dst_key_count;

    return true;
}

/* ── Rotation / listing ─────────────────────────────────────── */

struct wbs_file {
    char    name[256];
    int64_t mtime;
};

static int wbs_cmp_mtime_desc(const void *a, const void *b)
{
    const struct wbs_file *fa = a;
    const struct wbs_file *fb = b;
    if (fa->mtime > fb->mtime) return -1; // raw-return-ok:qsort-comparator
    if (fa->mtime < fb->mtime) return 1;
    return 0;
}

static int wbs_scan_backup_dir(const char *dir,
                                struct wbs_file *out, int max)
{
    if (!dir || !out || max <= 0) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < max) {
        if (strncmp(e->d_name, WALLET_BACKUP_FILENAME_PREFIX,
                    strlen(WALLET_BACKUP_FILENAME_PREFIX)) != 0)
            continue;
        size_t nl = strlen(e->d_name);
        size_t sl = strlen(WALLET_BACKUP_FILENAME_SUFFIX);
        if (nl < sl || strcmp(e->d_name + nl - sl,
                               WALLET_BACKUP_FILENAME_SUFFIX) != 0)
            continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        snprintf(out[n].name, sizeof(out[n].name), "%s", e->d_name);
        out[n].mtime = (int64_t)st.st_mtime;
        n++;
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof(struct wbs_file), wbs_cmp_mtime_desc);
    return n;
}

int wallet_backup_list(const char *backup_dir,
                        char (*out_paths)[512], int max)
{
    struct wbs_file files[256];
    int n = wbs_scan_backup_dir(backup_dir, files,
        max < (int)(sizeof(files) / sizeof(files[0]))
            ? max : (int)(sizeof(files) / sizeof(files[0])));
    for (int i = 0; i < n; i++)
        snprintf(out_paths[i], 512, "%s/%s", backup_dir, files[i].name);
    return n;
}

int wallet_backup_rotate(const char *backup_dir, int max_versions)
{
    if (max_versions <= 0) return 0;
    struct wbs_file files[256];
    int n = wbs_scan_backup_dir(backup_dir, files, 256);
    int deleted = 0;
    for (int i = max_versions; i < n; i++) {
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", backup_dir, files[i].name);
        if (unlink(full) == 0)
            deleted++;
    }
    return deleted;
}

/* ── Synchronous entry points ───────────────────────────────── */

static bool wbs_run_one_locked(void)
{
    int64_t started_ms = wbs_now_ms();
    char path[512] = "";
    char err[256]  = "";
    int64_t key_count = -1;
    bool ok = wallet_backup_run_once(g_wbs.cfg.backup_dir, g_wbs.db,
                                      path, sizeof(path),
                                      &key_count,
                                      err, sizeof(err));
    int64_t elapsed = wbs_now_ms() - started_ms;

    if (ok) {
        g_wbs.total_runs++;
        g_wbs.last_run_unix    = wbs_now_unix();
        g_wbs.last_key_count   = key_count;
        g_wbs.last_duration_ms = elapsed;
        snprintf(g_wbs.last_path, sizeof(g_wbs.last_path), "%s", path);
        g_wbs.last_error[0] = '\0';
        struct stat st;
        g_wbs.last_size_bytes =
            stat(path, &st) == 0 ? (int64_t)st.st_size : -1;
        /* Rotate after success — never lose the newest backup. */
        int max = g_wbs.cfg.max_versions > 0
            ? g_wbs.cfg.max_versions
            : WALLET_BACKUP_DEFAULT_MAX_VERSIONS;
        (void)wallet_backup_rotate(g_wbs.cfg.backup_dir, max);
    } else {
        g_wbs.total_failures++;
        snprintf(g_wbs.last_error, sizeof(g_wbs.last_error), "%s", err);
    }
    return ok;
}

bool wallet_backup_now(void)
{
    pthread_mutex_lock(&g_wbs.lock);
    if (!g_wbs.db || !g_wbs.cfg.backup_dir) {
        pthread_mutex_unlock(&g_wbs.lock);
        LOG_FAIL("wallet_backup", "backup_now: service not initialized (db=%p dir=%s)",
                 (void *)g_wbs.db, g_wbs.cfg.backup_dir ? g_wbs.cfg.backup_dir : "NULL");
    }
    bool ok = wbs_run_one_locked();
    pthread_mutex_unlock(&g_wbs.lock);
    return ok;
}

/* ── Thread loop ────────────────────────────────────────────── */

static void wbs_sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void *wbs_thread_fn(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_wbs.lock);
    int interval = g_wbs.cfg.interval_seconds > 0
        ? g_wbs.cfg.interval_seconds
        : WALLET_BACKUP_DEFAULT_INTERVAL_SEC;
    pthread_mutex_unlock(&g_wbs.lock);

    /* Do one immediate backup on start so the user always has a
     * fresh copy within a few seconds of boot — the worst failure
     * is the boot that hasn't reached its first hourly tick yet. */
    (void)wallet_backup_now();

    int64_t next_at_ms = wbs_now_ms() + (int64_t)interval * 1000;
    while (true) {
        pthread_mutex_lock(&g_wbs.lock);
        bool stop = g_wbs.stop_requested;
        bool pending = g_wbs.key_change_pending;
        int64_t last_ok = g_wbs.last_run_unix;
        pthread_mutex_unlock(&g_wbs.lock);
        if (stop) break;

        bool ran_this_tick = false;
        if (wbs_now_ms() >= next_at_ms) {
            (void)wallet_backup_now();
            ran_this_tick = true;
            /* Re-read interval in case config was updated. */
            pthread_mutex_lock(&g_wbs.lock);
            interval = g_wbs.cfg.interval_seconds > 0
                ? g_wbs.cfg.interval_seconds
                : WALLET_BACKUP_DEFAULT_INTERVAL_SEC;
            pthread_mutex_unlock(&g_wbs.lock);
            next_at_ms = wbs_now_ms() + (int64_t)interval * 1000;
        } else if (pending) {
            /* Debounced trigger path: fire if the last backup (of any
             * kind) is older than WALLET_BACKUP_TRIGGER_MIN_INTERVAL_SEC.
             * Multiple triggers that arrive inside the window collapse
             * into this single run. */
            int64_t now_s = wbs_now_unix();
            if (last_ok == 0 ||
                now_s >= last_ok + WALLET_BACKUP_TRIGGER_MIN_INTERVAL_SEC) {
                (void)wallet_backup_now();
                ran_this_tick = true;
                pthread_mutex_lock(&g_wbs.lock);
                g_wbs.total_trigger_runs++;
                pthread_mutex_unlock(&g_wbs.lock);
            }
        }

        if (ran_this_tick) {
            pthread_mutex_lock(&g_wbs.lock);
            g_wbs.key_change_pending = false;
            pthread_mutex_unlock(&g_wbs.lock);
        }

        /* Sleep in small increments so stop_requested is honoured
         * without waiting up to `interval` seconds. */
        wbs_sleep_ms(200);
    }

    pthread_mutex_lock(&g_wbs.lock);
    g_wbs.thread_running = false;
    pthread_mutex_unlock(&g_wbs.lock);
    return NULL;
}

bool wallet_backup_start(const struct wallet_backup_config *cfg,
                          struct node_db *db)
{
    if (!cfg || !db || !cfg->backup_dir)
        LOG_FAIL("wallet_backup", "start: NULL config, db, or backup_dir");

    pthread_mutex_lock(&g_wbs.lock);
    if (g_wbs.thread_running) {
        pthread_mutex_unlock(&g_wbs.lock);
        return true;
    }

    /* Refuse to back up into the same datadir as the source — the
     * whole point is an *external* copy. We detect this by
     * comparing the backup_dir to the directory containing the
     * source db file. */
    const char *src_path = wbs_source_path(db->db);
    if (src_path) {
        char src_dir[1024];
        snprintf(src_dir, sizeof(src_dir), "%s", src_path);
        char *slash = strrchr(src_dir, '/');
        if (slash) *slash = '\0';
        if (strcmp(src_dir, cfg->backup_dir) == 0) {
            pthread_mutex_unlock(&g_wbs.lock);
            fprintf(stderr,
                "wallet_backup: refusing to back up into source dir %s\n",
                src_dir);
            return false;
        }
    }

    if (!wbs_ensure_backup_dir(cfg->backup_dir)) {
        pthread_mutex_unlock(&g_wbs.lock);
        LOG_FAIL("wallet_backup", "start: cannot create backup dir %s", cfg->backup_dir);
    }

    g_wbs.cfg = *cfg;
    g_wbs.db = db;
    g_wbs.stop_requested = false;
    g_wbs.thread_running = true;

    int rc = thread_registry_spawn_ex("zcl_wallet_bk", wbs_thread_fn, NULL,
                                       &g_wbs.thread);
    if (rc != 0) {
        g_wbs.thread_running = false;
        pthread_mutex_unlock(&g_wbs.lock);
        fprintf(stderr, "wallet_backup: thread_registry_spawn_ex failed (%d)\n", rc);
        return false;
    }
    pthread_mutex_unlock(&g_wbs.lock);
    return true;
}

void wallet_backup_stop(void)
{
    pthread_t th;
    bool joinable = false;
    pthread_mutex_lock(&g_wbs.lock);
    if (g_wbs.thread_running) {
        g_wbs.stop_requested = true;
        th = g_wbs.thread;
        joinable = true;
    }
    pthread_mutex_unlock(&g_wbs.lock);

    if (joinable) {
        pthread_join(th, NULL);
        pthread_mutex_lock(&g_wbs.lock);
        g_wbs.thread_running = false;
        g_wbs.stop_requested = false;
        g_wbs.db = NULL;
        g_wbs.key_change_pending = false;
        pthread_mutex_unlock(&g_wbs.lock);
    }
}

/* ── Event triggers (D4: plan §5.4) ─────────────────────────── */

void wallet_backup_service_on_key_change(void)
{
    pthread_mutex_lock(&g_wbs.lock);
    /* Count every call, even coalesced ones, for debugging /
     * test visibility. Only set the pending flag if the thread is
     * running — otherwise the next wallet_backup_start() will do a
     * first-run immediately and pick up the state anyway. */
    g_wbs.total_triggers++;
    if (g_wbs.thread_running) {
        g_wbs.key_change_pending = true;
    }
    pthread_mutex_unlock(&g_wbs.lock);
}

void wallet_backup_service_on_keypool_topup(void)
{
    wallet_backup_service_on_key_change();
}
