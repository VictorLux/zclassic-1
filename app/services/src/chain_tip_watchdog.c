/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tip-stuck watchdog. See services/chain_tip_watchdog.h. */

#include "services/chain_tip_watchdog.h"

#include "services/chain_advance_coordinator.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include "util/supervisor.h"
#include "util/thread_registry.h"
#include "util/log_macros.h"
#include "event/event.h"
#include "json/json.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Default escalation thresholds (seconds). */
#define CHAIN_TIP_WD_DEFAULT_MIRROR_SECS    300
#define CHAIN_TIP_WD_DEFAULT_RESERVED_SECS  600
#define CHAIN_TIP_WD_DEFAULT_RESTART_SECS  1200

/* ── Module state ──────────────────────────────────────────────────── */

static struct main_state       *g_ms          = NULL;
static struct liveness_contract g_contract;
/* Atomic so an early dumpstate from another thread observes the
 * register result without a memory-ordering hazard. (Live: first dump
 * after restart returned registered=false; re-dump returned true.) */
static _Atomic supervisor_child_id g_id       = SUPERVISOR_INVALID_ID;

static _Atomic int64_t  g_highest_tip        = 0;
static _Atomic int64_t  g_last_advance_us    = 0;
static _Atomic int64_t  g_last_advance_unix  = 0;
static _Atomic int      g_escalation         = 0;

static _Atomic uint64_t g_fires_mirror   = 0;
static _Atomic uint64_t g_fires_reserved = 0;
static _Atomic uint64_t g_fires_restart  = 0;

static _Atomic int64_t  g_thr_mirror   = CHAIN_TIP_WD_DEFAULT_MIRROR_SECS;
static _Atomic int64_t  g_thr_reserved = CHAIN_TIP_WD_DEFAULT_RESERVED_SECS;
static _Atomic int64_t  g_thr_restart  = CHAIN_TIP_WD_DEFAULT_RESTART_SECS;

static int64_t mono_us_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* ── Supervisor tick ───────────────────────────────────────────────── */

static void chain_tip_wd_tick(struct liveness_contract *c)
{
    (void)c;
    if (!g_ms) return;

    int64_t h = (int64_t)active_chain_height(&g_ms->chain_active);
    int64_t now_us = mono_us_now();
    int64_t prev = atomic_load(&g_highest_tip);

    if (h > prev) {
        atomic_store(&g_highest_tip, h);
        atomic_store(&g_last_advance_us, now_us);
        atomic_store(&g_last_advance_unix, (int64_t)time(NULL));
        atomic_store(&g_escalation, 0);
        supervisor_progress(atomic_load(&g_id), h);
        return;
    }

    /* Tip didn't advance. Compute age and apply escalation ladder. */
    int64_t last = atomic_load(&g_last_advance_us);
    if (last == 0) {
        /* First-ever tick: seed the timer; nothing to escalate yet. */
        atomic_store(&g_last_advance_us, now_us);
        atomic_store(&g_last_advance_unix, (int64_t)time(NULL));
        supervisor_progress(atomic_load(&g_id), h);
        return;
    }
    int64_t age_s = (now_us - last) / 1000000;
    int level = atomic_load(&g_escalation);

    int64_t thr_mirror  = atomic_load(&g_thr_mirror);
    int64_t thr_restart = atomic_load(&g_thr_restart);

    if (level < 1 && thr_mirror > 0 && age_s >= thr_mirror) {
        atomic_store(&g_escalation, 1);
        atomic_fetch_add(&g_fires_mirror, 1u);
        fprintf(stderr,  // obs-ok:tip-wd-mirror
            "[chain_tip_watchdog] forcing mirror promotion: h=%lld age=%llds\n",
            (long long)h, (long long)age_s);
        event_emitf(EV_CHAIN_ADVANCE_DECISION, 0,
            "chain_tip_watchdog force_mirror h=%lld age=%llds",
            (long long)h, (long long)age_s);
        chain_advance_coordinator_force_mirror_promotion("chain_tip_watchdog:mirror");
    }

    if (level < 3 && thr_restart > 0 && age_s >= thr_restart) {
        atomic_store(&g_escalation, 3);
        atomic_fetch_add(&g_fires_restart, 1u);
        fprintf(stderr,  // obs-ok:tip-wd-restart
            "[chain_tip_watchdog] requesting shutdown: h=%lld age=%llds\n",
            (long long)h, (long long)age_s);
        event_emitf(EV_CHAIN_ADVANCE_DECISION, 0,
            "chain_tip_watchdog request_shutdown h=%lld age=%llds",
            (long long)h, (long long)age_s);
        thread_registry_request_shutdown();
    }

    /* Keep the supervisor's progress timer happy: we ARE ticking, just
     * not making chain progress. The escalation logic above is the
     * authoritative path; supervisor stall would be redundant noise. */
    supervisor_progress(atomic_load(&g_id), h);
}

/* ── Public API ────────────────────────────────────────────────────── */

void chain_tip_watchdog_register(struct main_state *ms)
{
    if (!ms) return;
    if (atomic_load(&g_id) != SUPERVISOR_INVALID_ID) return;  /* idempotent */
    g_ms = ms;
    liveness_contract_init(&g_contract, "chain.chain_tip_watchdog");
    atomic_store(&g_contract.period_secs, (int64_t)30);
    atomic_store(&g_contract.deadline_secs, (int64_t)0);
    atomic_store(&g_contract.progress_max_quiet_us, (int64_t)0);
    g_contract.on_tick  = chain_tip_wd_tick;
    g_contract.on_stall = NULL;
    atomic_store(&g_id, supervisor_register(&g_contract));
    if (atomic_load(&g_id) == SUPERVISOR_INVALID_ID) {
        fprintf(stderr,  // obs-ok:tip-wd-register-fail
            "[chain_tip_watchdog] WARN register failed\n");
    }
}

void chain_tip_watchdog_set_thresholds(int64_t mirror_secs,
                                  int64_t reserved_secs,
                                  int64_t restart_secs)
{
    if (mirror_secs   > 0) atomic_store(&g_thr_mirror,   mirror_secs);
    if (reserved_secs > 0) atomic_store(&g_thr_reserved, reserved_secs);
    if (restart_secs  > 0) atomic_store(&g_thr_restart,  restart_secs);
}

void chain_tip_watchdog_get_stats(struct chain_tip_watchdog_stats *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->registered = (atomic_load(&g_id) != SUPERVISOR_INVALID_ID);
    out->highest_tip = atomic_load(&g_highest_tip);
    out->last_advance_unix = atomic_load(&g_last_advance_unix);
    int64_t last = atomic_load(&g_last_advance_us);
    int64_t now = mono_us_now();
    out->age_secs = (last == 0) ? 0 : (now - last) / 1000000;
    out->escalation_level = atomic_load(&g_escalation);
    out->fires_mirror = atomic_load(&g_fires_mirror);
    out->fires_reserved = atomic_load(&g_fires_reserved);
    out->fires_restart = atomic_load(&g_fires_restart);
    out->threshold_mirror_secs = atomic_load(&g_thr_mirror);
    out->threshold_reserved_secs = atomic_load(&g_thr_reserved);
    out->threshold_restart_secs = atomic_load(&g_thr_restart);
}

bool chain_tip_watchdog_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out) return false;
    struct chain_tip_watchdog_stats s;
    chain_tip_watchdog_get_stats(&s);
    json_push_kv_bool(out, "registered", s.registered);
    json_push_kv_int(out, "highest_tip", s.highest_tip);
    json_push_kv_int(out, "last_advance_unix", s.last_advance_unix);
    json_push_kv_int(out, "age_secs", s.age_secs);
    json_push_kv_int(out, "escalation_level", (int64_t)s.escalation_level);
    json_push_kv_int(out, "fires_mirror", (int64_t)s.fires_mirror);
    json_push_kv_int(out, "fires_reserved", (int64_t)s.fires_reserved);
    json_push_kv_int(out, "fires_restart", (int64_t)s.fires_restart);
    json_push_kv_int(out, "threshold_mirror_secs", s.threshold_mirror_secs);
    json_push_kv_int(out, "threshold_reserved_secs", s.threshold_reserved_secs);
    json_push_kv_int(out, "threshold_restart_secs", s.threshold_restart_secs);
    return true;
}
