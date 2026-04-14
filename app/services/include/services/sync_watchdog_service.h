/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Sync Watchdog Service — detects and recovers from sync stalls.
 *
 * The watchdog monitors sync state progression and triggers automatic
 * recovery when the node gets stuck:
 *   - HEADER_STALL:  headers not advancing for 300s
 *   - BLOCK_STALL:   chain height not advancing for 300s
 *   - STATE_STUCK:   any non-tip state unchanged for 600s
 *   - REPEATED_RESTART: circuit breaker after 3 recoveries in 30min */

#ifndef ZCL_SERVICES_SYNC_WATCHDOG_H
#define ZCL_SERVICES_SYNC_WATCHDOG_H

#include "event/event.h"
#include "net/connman.h"
#include "net/download.h"
#include "validation/main_state.h"
#include <stdbool.h>
#include <stdint.h>

/* Recovery type identifiers */
enum watchdog_recovery_type {
    WATCHDOG_NONE = 0,
    WATCHDOG_HEADER_STALL,
    WATCHDOG_HEADER_LAG,
    WATCHDOG_BLOCK_STALL,
    WATCHDOG_STATE_STUCK,
    WATCHDOG_REPEATED_RESTART,
};

/* Watchdog status snapshot (for RPC) */
struct sync_watchdog_status {
    bool     enabled;
    uint64_t checks_run;
    uint64_t recoveries_triggered;
    int64_t  last_recovery_time;
    enum watchdog_recovery_type last_recovery_type;
    enum sync_state current_state;
    int64_t  current_state_duration_secs;
    int      current_state_entry_height;
};

/* Initialize watchdog state and register state-change callback.
 * Call once at startup. */
void sync_watchdog_init(void);

/* Run one watchdog check cycle. Call from message processing loop (~every 30s).
 * Returns the recovery type triggered, or WATCHDOG_NONE. */
enum watchdog_recovery_type sync_watchdog_check(
    struct connman *cm,
    struct download_manager *dm,
    struct main_state *ms);

/* Get current watchdog status for RPC. */
void sync_watchdog_get_status(struct sync_watchdog_status *out);

/* Return human-readable name for a recovery type. */
const char *watchdog_recovery_type_name(enum watchdog_recovery_type type);

/* Get how long the current sync state has been active (seconds). */
int64_t sync_get_state_duration(void);

/* Get the height at which the current sync state was entered. */
int sync_get_state_entry_height(void);

/* Record the last header rejection reason for escalation diagnostics. */
void sync_watchdog_set_last_reject_reason(const char *reason);

/* Called internally when sync state changes to update timestamps.
 * Registered as callback via sync_set_state_change_callback(). */
void sync_watchdog_on_state_change(enum sync_state new_state, int height);

#endif /* ZCL_SERVICES_SYNC_WATCHDOG_H */
