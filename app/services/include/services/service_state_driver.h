/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * service_state_driver — drives the canonical `enum service_state` from
 * REAL runtime progress, and persists/restores it across restarts.
 *
 * service_state.c is a pure primitive (a value + a logged transition). It
 * does not know about sync gaps, conditions, or progress.kv. This driver is
 * the app-layer glue that makes the state machine LIVE:
 *
 *   - service_state_driver_tick(): called every ~5s from the self_heal
 *     supervisor. (1) Flips to REPAIRING while a named repair Condition is
 *     actively repairing and restores the prior operational mode when it
 *     clears. (2) Otherwise drives DEGRADED_SERVING <-> SYNCING <-> HEALTHY
 *     from the local-vs-peer tip gap and active-condition count.
 *   - persist/restore: mirror the current operational mode into progress.kv
 *     so the node's last mode is observable on the next boot.
 *
 * Nothing here touches the chain, a consensus gate, or the public tip — it
 * is observability + a state machine over signals that already exist. Every
 * failure is logged and swallowed; the driver never aborts the node. */

#ifndef ZCL_SERVICES_SERVICE_STATE_DRIVER_H
#define ZCL_SERVICES_SERVICE_STATE_DRIVER_H

#include <stdbool.h>

/* Periodic driver tick. Idempotent; safe to call when subsystems are not
 * yet wired (no main_state / no peers) — it simply makes no transition. */
void service_state_driver_tick(void);

/* Write the current service_state id + reason to progress.kv. Returns false
 * (logged) if the store is unavailable or a write fails; never aborts. The
 * transient REPAIRING mode is intentionally NOT persisted by the driver — the
 * underlying operational mode is what survives a restart. */
bool service_state_persist_to_progress_store(void);

/* Restore the persisted operational mode early in boot (after progress.kv is
 * open). Idempotent; a missing/invalid record is a no-op that leaves the
 * state at BOOT. Returns true only if a valid record was applied. */
bool service_state_restore_from_progress_store(void);

#ifdef ZCL_TESTING
/* Override the gap inputs so the sync-gap decision is testable without a live
 * connman / chain. local_h and peer_max feed gap=peer_max-local_h;
 * tip_age_secs feeds the diagnostic reason. */
void service_state_driver_test_set_overrides(int local_h, int peer_max,
                                             int tip_age_secs);
void service_state_driver_test_clear_overrides(void);
#endif

#endif /* ZCL_SERVICES_SERVICE_STATE_DRIVER_H */
