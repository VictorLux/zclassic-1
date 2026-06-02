/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Disk Monitor — see header for rationale.
 *
 * Uses `statvfs` for portable free-space queries. Polls in a
 * background pthread with small sleep increments so stop()
 * returns promptly.
 *
 * State transitions are edge-triggered: we only emit an event
 * when the level *changes*. That keeps EV_DISK_LOW from flooding
 * the log every 60 seconds on a sustained low-disk situation.
 */

#include "platform/time_compat.h"
#include "services/disk_monitor.h"

#include "event/event.h"
#include "util/thread_registry.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>
#include <time.h>

#include "util/log_macros.h"

/* ── Module state ───────────────────────────────────────────── */

struct disk_monitor_state {
    pthread_mutex_t lock;
    pthread_t       thread;
    bool            thread_running;
    bool            stop_requested;

    struct disk_monitor_config cfg;

    /* Resolved thresholds (after filling in defaults). */
    int64_t warn_free_bytes;
    int64_t refuse_free_bytes;
    int     poll_seconds;

    /* Last poll observation. */
    enum disk_monitor_level last_level;
    int64_t                 last_free_bytes;
    int64_t                 last_poll_unix;

    /* Hot-path lock-free flag mirroring last_level. */
    _Atomic int atomic_level;
};

static struct disk_monitor_state g_dm = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .last_free_bytes = -1,
    .atomic_level = DISK_MONITOR_OK,
};

/* ── Defaults ───────────────────────────────────────────────── */

void disk_monitor_config_defaults(struct disk_monitor_config *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->warn_free_bytes   = DISK_MONITOR_DEFAULT_WARN_BYTES;
    cfg->refuse_free_bytes = DISK_MONITOR_DEFAULT_REFUSE_BYTES;
    cfg->poll_seconds      = DISK_MONITOR_DEFAULT_POLL_SECONDS;
}

/* ── Primitive ──────────────────────────────────────────────── */

int64_t disk_monitor_free_bytes(const char *path)
{
    if (!path || !*path) {
        fprintf(stderr, "[disk] %s: path is NULL or empty\n", __func__);
        return -1; // raw-return-ok:logged-above
    }
    struct statvfs st;
    if (statvfs(path, &st) != 0) {
        fprintf(stderr, "[disk] %s: statvfs failed for '%s': %s\n",
                __func__, path, strerror(errno));
        return -1; // raw-return-ok:logged-above
    }
    /* `f_bavail` is the blocks available to unprivileged users —
     * the right number to compare against a "should I keep
     * writing?" threshold. Multiplied by `f_frsize` which is the
     * fundamental block size (not `f_bsize` which can differ). */
    return (int64_t)st.f_bavail * (int64_t)st.f_frsize;
}

/* ── Classification ─────────────────────────────────────────── */

static enum disk_monitor_level
dm_classify(int64_t free_bytes, int64_t refuse, int64_t warn)
{
    if (free_bytes < 0)         return DISK_MONITOR_OK; /* unknown */
    if (free_bytes < refuse)    return DISK_MONITOR_CRITICAL;
    if (free_bytes < warn)      return DISK_MONITOR_LOW;
    return DISK_MONITOR_OK;
}

static const char *dm_level_name(enum disk_monitor_level lvl)
{
    switch (lvl) {
        case DISK_MONITOR_OK:       return "ok";
        case DISK_MONITOR_LOW:      return "low";
        case DISK_MONITOR_CRITICAL: return "critical";
        default:                    return "unknown";
    }
}

/* Assumes g_dm.lock is held. */
static void dm_run_one_locked(void)
{
    int64_t free_b = disk_monitor_free_bytes(g_dm.cfg.datadir);
    enum disk_monitor_level new_level =
        dm_classify(free_b, g_dm.refuse_free_bytes, g_dm.warn_free_bytes);

    g_dm.last_free_bytes = free_b;
    g_dm.last_poll_unix  = (int64_t)platform_time_wall_time_t();

    enum disk_monitor_level prev = g_dm.last_level;
    g_dm.last_level = new_level;
    atomic_store(&g_dm.atomic_level, (int)new_level);

    if (new_level == prev) return; /* edge-triggered only */

    switch (new_level) {
        case DISK_MONITOR_OK:
            event_emitf(EV_DISK_OK, 0,
                        "path=%s free=%" PRId64 " prev=%s",
                        g_dm.cfg.datadir ? g_dm.cfg.datadir : "",
                        free_b, dm_level_name(prev));
            break;
        case DISK_MONITOR_LOW:
            event_emitf(EV_DISK_LOW, 0,
                        "path=%s free=%" PRId64 " warn=%" PRId64 " prev=%s",
                        g_dm.cfg.datadir ? g_dm.cfg.datadir : "",
                        free_b, g_dm.warn_free_bytes,
                        dm_level_name(prev));
            break;
        case DISK_MONITOR_CRITICAL:
            event_emitf(EV_DISK_CRITICAL, 0,
                        "path=%s free=%" PRId64 " refuse=%" PRId64 " prev=%s",
                        g_dm.cfg.datadir ? g_dm.cfg.datadir : "",
                        free_b, g_dm.refuse_free_bytes,
                        dm_level_name(prev));
            break;
    }
}

void disk_monitor_poll_now(void)
{
    pthread_mutex_lock(&g_dm.lock);
    dm_run_one_locked();
    pthread_mutex_unlock(&g_dm.lock);
}

/* ── Thread loop ────────────────────────────────────────────── */

static void *dm_thread_fn(void *arg)
{
    (void)arg;
    int64_t next_at_ms;
    int poll_seconds;

    pthread_mutex_lock(&g_dm.lock);
    poll_seconds = g_dm.poll_seconds;
    pthread_mutex_unlock(&g_dm.lock);

    struct timespec now;
    platform_time_monotonic_timespec(&now);
    int64_t now_ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
    next_at_ms = now_ms + (int64_t)poll_seconds * 1000;

    while (true) {
        pthread_mutex_lock(&g_dm.lock);
        bool stop = g_dm.stop_requested;
        pthread_mutex_unlock(&g_dm.lock);
        if (stop) break;

        platform_time_monotonic_timespec(&now);
        now_ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
        if (now_ms >= next_at_ms) {
            pthread_mutex_lock(&g_dm.lock);
            dm_run_one_locked();
            poll_seconds = g_dm.poll_seconds;
            pthread_mutex_unlock(&g_dm.lock);
            next_at_ms = now_ms + (int64_t)poll_seconds * 1000;
        }
        platform_sleep_ms(100);
    }

    pthread_mutex_lock(&g_dm.lock);
    g_dm.thread_running = false;
    pthread_mutex_unlock(&g_dm.lock);
    return NULL;
}

/* ── Lifecycle ──────────────────────────────────────────────── */

struct zcl_result disk_monitor_start(const struct disk_monitor_config *cfg)
{
    if (!cfg || !cfg->datadir)
        return ZCL_ERR(-1, "start called with null config or datadir");

    pthread_mutex_lock(&g_dm.lock);
    if (g_dm.thread_running) {
        pthread_mutex_unlock(&g_dm.lock);
        return ZCL_ERR(-2, "start called but monitor thread already running");
    }

    g_dm.cfg = *cfg;
    g_dm.warn_free_bytes =
        cfg->warn_free_bytes   > 0 ? cfg->warn_free_bytes
                                   : DISK_MONITOR_DEFAULT_WARN_BYTES;
    g_dm.refuse_free_bytes =
        cfg->refuse_free_bytes > 0 ? cfg->refuse_free_bytes
                                   : DISK_MONITOR_DEFAULT_REFUSE_BYTES;
    g_dm.poll_seconds =
        cfg->poll_seconds      > 0 ? cfg->poll_seconds
                                   : DISK_MONITOR_DEFAULT_POLL_SECONDS;

    /* Reject datadir we can't even stat — no point starting a
     * thread that will just emit -1 forever. */
    if (disk_monitor_free_bytes(cfg->datadir) < 0) {
        pthread_mutex_unlock(&g_dm.lock);
        return ZCL_ERR(-3, "cannot stat datadir %s", cfg->datadir);
    }

    /* Synchronous first poll so callers know the level before
     * this function returns (and so tests don't race the thread). */
    dm_run_one_locked();

    g_dm.stop_requested = false;
    g_dm.thread_running = true;
    int rc = thread_registry_spawn_ex("zcl_disk_monitor", dm_thread_fn, NULL,
                                       &g_dm.thread);
    if (rc != 0) {
        g_dm.thread_running = false;
        pthread_mutex_unlock(&g_dm.lock);
        return ZCL_ERR(-4, "thread_registry_spawn_ex failed (%d)", rc);
    }
    pthread_mutex_unlock(&g_dm.lock);
    return ZCL_OK;
}

void disk_monitor_stop(void)
{
    pthread_t th;
    bool joinable = false;

    pthread_mutex_lock(&g_dm.lock);
    if (g_dm.thread_running) {
        g_dm.stop_requested = true;
        th = g_dm.thread;
        joinable = true;
    }
    pthread_mutex_unlock(&g_dm.lock);

    if (joinable) {
        pthread_join(th, NULL);
        pthread_mutex_lock(&g_dm.lock);
        g_dm.thread_running = false;
        g_dm.stop_requested = false;
        pthread_mutex_unlock(&g_dm.lock);
    }
}

/* ── Status / queries ───────────────────────────────────────── */

void disk_monitor_status_snapshot(struct disk_monitor_status *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&g_dm.lock);
    out->running          = g_dm.thread_running;
    out->level            = g_dm.last_level;
    out->last_free_bytes  = g_dm.last_free_bytes;
    out->last_poll_unix   = g_dm.last_poll_unix;
    out->warn_free_bytes  = g_dm.warn_free_bytes;
    out->refuse_free_bytes = g_dm.refuse_free_bytes;
    snprintf(out->datadir, sizeof(out->datadir), "%s",
             g_dm.cfg.datadir ? g_dm.cfg.datadir : "");
    pthread_mutex_unlock(&g_dm.lock);
}

bool disk_monitor_is_critical(void)
{
    return (enum disk_monitor_level)atomic_load(&g_dm.atomic_level) ==
           DISK_MONITOR_CRITICAL;
}

enum disk_monitor_level disk_monitor_level(void)
{
    return (enum disk_monitor_level)atomic_load(&g_dm.atomic_level);
}
