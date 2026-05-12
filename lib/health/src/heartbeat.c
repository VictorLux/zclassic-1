/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Heartbeat ring + single sweeper thread. See heartbeat.h for the
 * design rationale and edge-triggered stall semantics.
 */

#include "health/heartbeat.h"
#include "core/utiltime.h"
#include "util/thread_registry.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

struct entry {
    bool             active;
    char             name[HEALTH_NAME_MAX];
    int64_t          deadline_secs;
    void           (*on_stall)(void *);
    void            *ctx;
    _Atomic int64_t  last_beat_us;
    int64_t          last_stall_beat_us;  /* beat-timestamp the last
                                            * stall fired against; used to
                                            * edge-trigger (don't refire
                                            * unless a fresh heartbeat
                                            * arrived since) */
    int              on_stall_fired;
};

static struct entry    g_entries[HEALTH_REGISTRY_CAP];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static _Atomic bool    g_running = false;
static _Atomic bool    g_started = false;
static pthread_t       g_sweeper;
static _Atomic int     g_check_interval_ms = 1000;

void health_set_check_interval_ms(int ms)
{
    if (ms < 1) ms = 1;
    atomic_store(&g_check_interval_ms, ms);
}

health_subsystem_id health_register(const char *name,
                                     int64_t deadline_secs,
                                     void (*on_stall)(void *),
                                     void *ctx)
{
    if (!name || !on_stall || deadline_secs <= 0)
        return HEALTH_INVALID_ID;

    int64_t now_us = GetTimeMicros();

    pthread_mutex_lock(&g_mu);
    int slot = -1;
    for (int i = 0; i < HEALTH_REGISTRY_CAP; i++) {
        if (!g_entries[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        pthread_mutex_unlock(&g_mu);
        fprintf(stderr, "[health] registry full (cap=%d), cannot register '%s'\n",
                HEALTH_REGISTRY_CAP, name);
        return HEALTH_INVALID_ID;
    }

    g_entries[slot].active = true;
    strncpy(g_entries[slot].name, name, HEALTH_NAME_MAX - 1);
    g_entries[slot].name[HEALTH_NAME_MAX - 1] = '\0';
    g_entries[slot].deadline_secs = deadline_secs;
    g_entries[slot].on_stall = on_stall;
    g_entries[slot].ctx = ctx;
    atomic_store(&g_entries[slot].last_beat_us, now_us);
    /* Sentinel: NEVER seed last_stall_beat_us == last_beat_us, because
     * the edge-trigger check skips when they're equal (treating it as
     * "already fired against this beat"). 0 means "no stall ever fired
     * against this entry"; the first missed-deadline sweep will fire. */
    g_entries[slot].last_stall_beat_us = 0;
    g_entries[slot].on_stall_fired = 0;
    pthread_mutex_unlock(&g_mu);

    return slot;
}

void health_heartbeat(health_subsystem_id id)
{
    if (id < 0 || id >= HEALTH_REGISTRY_CAP) return;
    if (!g_entries[id].active) return;
    atomic_store(&g_entries[id].last_beat_us, GetTimeMicros());
}

void health_unregister(health_subsystem_id id)
{
    if (id < 0 || id >= HEALTH_REGISTRY_CAP) return;
    pthread_mutex_lock(&g_mu);
    g_entries[id].active = false;
    pthread_mutex_unlock(&g_mu);
}

int health_snapshot_all(struct health_snapshot *out, int max)
{
    if (!out || max <= 0) return 0;
    int64_t now_us = GetTimeMicros();
    int n = 0;

    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < HEALTH_REGISTRY_CAP && n < max; i++) {
        if (!g_entries[i].active) continue;
        int64_t beat_us = atomic_load(&g_entries[i].last_beat_us);
        int64_t age_us  = now_us - beat_us;
        strncpy(out[n].name, g_entries[i].name, HEALTH_NAME_MAX);
        out[n].name[HEALTH_NAME_MAX - 1] = '\0';
        out[n].deadline_secs = g_entries[i].deadline_secs;
        out[n].last_beat_age_secs = age_us / 1000000;
        out[n].on_stall_fired = g_entries[i].on_stall_fired;
        out[n].currently_stalled = (age_us / 1000000) > g_entries[i].deadline_secs;
        n++;
    }
    pthread_mutex_unlock(&g_mu);
    return n;
}

static void sweep_once(void)
{
    int64_t now_us = GetTimeMicros();

    /* Snapshot the entries we need to fire on, OUTSIDE the lock.
     * Calling on_stall() while holding g_mu would deadlock the
     * callback if it touches the ring (e.g. heartbeats a sibling).
     * Collect fire candidates first, then release the lock and
     * invoke. */
    struct {
        void (*fn)(void *);
        void  *ctx;
        int    slot;
        int64_t beat_us;
    } fires[HEALTH_REGISTRY_CAP];
    int nfires = 0;

    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < HEALTH_REGISTRY_CAP; i++) {
        if (!g_entries[i].active) continue;
        int64_t beat_us = atomic_load(&g_entries[i].last_beat_us);
        int64_t age_us  = now_us - beat_us;
        int64_t deadline_us = g_entries[i].deadline_secs * (int64_t)1000000;
        if (age_us <= deadline_us) continue;

        /* Edge trigger: only fire if beat_us has advanced since the
         * last firing, OR this is the first stall ever for this entry
         * (last_stall_beat_us is its initial registration timestamp). */
        if (beat_us == g_entries[i].last_stall_beat_us) {
            /* Already fired against this beat-timestamp. Skip. */
            continue;
        }
        g_entries[i].last_stall_beat_us = beat_us;
        g_entries[i].on_stall_fired++;
        fires[nfires].fn = g_entries[i].on_stall;
        fires[nfires].ctx = g_entries[i].ctx;
        fires[nfires].slot = i;
        fires[nfires].beat_us = beat_us;
        nfires++;
    }
    pthread_mutex_unlock(&g_mu);

    for (int k = 0; k < nfires; k++) {
        fires[k].fn(fires[k].ctx);
    }
}

static void *sweeper_thread(void *arg)
{
    (void)arg;
    while (atomic_load(&g_running)) {
        sweep_once();
        int interval_ms = atomic_load(&g_check_interval_ms);
        struct timespec ts = {
            .tv_sec  = interval_ms / 1000,
            .tv_nsec = (long)(interval_ms % 1000) * 1000000L,
        };
        nanosleep(&ts, NULL);
    }
    return NULL;
}

bool health_start(void)
{
    bool expected = false;
    if (!atomic_compare_exchange_strong(&g_started, &expected, true))
        return true; /* already running */

    atomic_store(&g_running, true);
    int rc = thread_registry_spawn_ex("zcl_health_sweep", sweeper_thread,
                                       NULL, &g_sweeper);
    if (rc != 0) {
        fprintf(stderr, "[health] sweeper spawn failed: rc=%d\n", rc);
        atomic_store(&g_running, false);
        atomic_store(&g_started, false);
        return false;
    }
    return true;
}

void health_stop(void)
{
    bool expected = true;
    if (!atomic_compare_exchange_strong(&g_started, &expected, false))
        return; /* not running */
    atomic_store(&g_running, false);
    pthread_join(g_sweeper, NULL);
}

void health_reset_for_test(void)
{
    health_stop();
    pthread_mutex_lock(&g_mu);
    memset(g_entries, 0, sizeof(g_entries));
    pthread_mutex_unlock(&g_mu);
    atomic_store(&g_check_interval_ms, 1000);
}
