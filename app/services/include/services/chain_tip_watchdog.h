/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tip-stuck watchdog: a single-purpose supervisor child that does ONE
 * thing — watch active_chain_height advance — and escalates when it
 * doesn't. Designed to be independent of lag_breach_severity logic,
 * mirror catchup state machines, projection deferral counters, and
 * source-scoring competitions, all of which have proven able to mask
 * the underlying "tip is not advancing" signal in prior wedges.
 *
 * Contract (Sync-and-Solid invariant):
 *   The node must advance its tip at least once every 300 s when not
 *   genuinely caught up. If it doesn't, escalate within 30 s.
 *
 * Escalation ladder (configurable thresholds; defaults below):
 *   t=300s  no advance → force_mirror_promotion("tip_wd:300s")
 *   t=600s  no advance → reserved (for future score_reset)
 *   t=1200s no advance → request orderly shutdown; systemd Restart=always
 *                        brings us back with fresh state.
 *
 * Verify-never-trust: this primitive does NOT lower any validation
 * gate. The most-aggressive action it can take is to ask the OS to
 * restart the process. Every block written after restart still goes
 * through the full crypto pipeline.
 *
 * Reentrancy: the contract is registered once at boot, ticked from
 * the supervisor thread. All cross-thread state is atomic. */

#ifndef SERVICES_TIP_WATCHDOG_H
#define SERVICES_TIP_WATCHDOG_H

#include <stdbool.h>
#include <stdint.h>

struct main_state;
struct json_value;

/* Register the watchdog as a supervisor child. Idempotent. Must be
 * called after main_state + chain_active are initialized and after
 * `supervisor_start()`. */
void chain_tip_watchdog_register(struct main_state *ms);

/* Optional retuning. All thresholds in seconds. Zero leaves the
 * existing value unchanged. */
void chain_tip_watchdog_set_thresholds(int64_t mirror_secs,
                                  int64_t reserved_secs,
                                  int64_t restart_secs);

/* Snapshot for diagnostics. */
struct chain_tip_watchdog_stats {
    bool     registered;
    int64_t  highest_tip;
    int64_t  last_advance_unix;     /* CLOCK_REALTIME seconds */
    int64_t  age_secs;              /* seconds since last advance */
    int      escalation_level;      /* 0=ok, 1=mirror, 2=reserved, 3=restart */
    uint64_t fires_mirror;
    uint64_t fires_reserved;
    uint64_t fires_restart;
    int64_t  threshold_mirror_secs;
    int64_t  threshold_reserved_secs;
    int64_t  threshold_restart_secs;
};
void chain_tip_watchdog_get_stats(struct chain_tip_watchdog_stats *out);

/* `zcl_state subsystem=chain_tip_watchdog` dumper. `out` is an
 * already-initialized json object; `key` is ignored. */
bool chain_tip_watchdog_dump_state_json(struct json_value *out, const char *key);

#endif
