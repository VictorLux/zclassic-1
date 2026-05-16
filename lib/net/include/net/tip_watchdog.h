/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * backpressure watchdog — caps RAM blow-up under a tip-stall.
 *
 * Live-outage context (2026-04-18): a regression in update_tip
 * trapped the chain at h=3,081,601; new blocks kept arriving
 * but never advanced chain_tip, so download buffers + connect-block
 * scratch climbed to 6.0 GB RSS before the cgroup OOM path fired.
 * fixed the root cause; this watchdog is the diagnostic
 * backstop that turns the next tip-stall regression into a bounded
 * EV_BACKPRESSURE_* event stream instead of an OOM.
 *
 * State machine:
 *   INACTIVE → ACTIVE  when (now - last_tip_advance > 60s)
 *                       AND  download_queue_bytes > 256 MiB
 *   ACTIVE → INACTIVE  when chain_tip advances OR 120s have elapsed
 *
 * In ACTIVE we drain the download manager's queue + in-flight set
 * (peers stop being asked for new blocks), and inv/block messages
 * arriving from any peer are dropped after parse but before
 * dispatch. Drops emit EV_BACKPRESSURE_REJECT with the peer id but
 * do NOT bump ban-score — peers aren't misbehaving.
 */

#ifndef ZCL_NET_TIP_WATCHDOG_H
#define ZCL_NET_TIP_WATCHDOG_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Tuning constants. Compile-time only in this patch — RPC-tunable
 * policy is a separate row. Numbers chosen against the 2026-04-18
 * incident: a stuck tip held for ~10 minutes accumulated 6 GB of
 * residency; 256 MiB is ~25% of that, well under the cgroup high
 * watermark and large enough that normal IBD bursts don't trip. */
#define TIP_STALL_THRESHOLD_SEC      60
#define DOWNLOAD_QUEUE_HIGH_WATER    (256UL * 1024 * 1024)
#define BACKPRESSURE_REJECT_SEC      120

/* Average block-body footprint used to estimate download_queue_bytes
 * from the in-flight slot count. The download manager doesn't track
 * per-slot bytes — slots only carry the hash + bookkeeping — so we
 * assume each in-flight slot will be paid back with one full block.
 * 2 MiB is the consensus upper bound (MAX_BLOCK_SIZE); using the
 * upper bound favors early backpressure, which is the safer
 * direction for an OOM backstop. */
#define BACKPRESSURE_AVG_BLOCK_BYTES (2UL * 1024 * 1024)

/* Initialize the watchdog. Idempotent; seeds the stall timer to
 * "now" so a fresh process doesn't fire before the chain has had
 * a chance to advance. */
void tip_watchdog_init(void);

/* Note that the chain tip has advanced. Resets the stall timer.
 * Driven by EV_BLOCK_CONNECTED / EV_TIP_UPDATED observers wired in
 * msg_processor_init. The height value is recorded for diagnostics
 * only — only the timestamp matters for stall detection. */
void tip_watchdog_note_tip_advance(int height);

/* Periodic state-machine tick. Cheap (a few atomics + one hash-table
 * stat read). Call from the msgprocessor loops on every iteration.
 * Returns the post-tick value of the active flag. */
bool tip_watchdog_tick(void);

/* Lock-free read of the active flag. */
bool tip_watchdog_is_active(void);

/* Pre-dispatch hook for inv/block messages.
 *
 * Returns true if the caller must drop the message (already emitted
 * EV_BACKPRESSURE_REJECT). Returns false for any non-block command
 * or when the watchdog is INACTIVE — callers can blanket-call it for
 * every message without checking the active flag themselves. Does
 * NOT touch peer ban-score. */
bool tip_watchdog_should_reject(uint32_t peer_id, const char *cmd);

/* ── Test hooks ──────────────────────────────────────────────
 * Drive the watchdog with an explicit clock and queue size.  Not
 * intended for production callers — used by test_net.c cases. */

void tip_watchdog_test_reset(void);
void tip_watchdog_test_set_now_ns(int64_t now_ns);
void tip_watchdog_test_set_queue_bytes(size_t bytes);
void tip_watchdog_test_inject_tip_advance(int height, int64_t when_ns);

struct tip_watchdog_stats {
    uint64_t entered_active;        /* EV_BACKPRESSURE_ACTIVE emits */
    uint64_t cleared;               /* EV_BACKPRESSURE_CLEAR emits */
    uint64_t rejected_messages;     /* EV_BACKPRESSURE_REJECT emits */
    uint64_t drained_queue_entries; /* slots+queue entries dropped on entry */
    int64_t  last_tip_advance_ns;
    int64_t  entered_active_ns;
    bool     active;
};

void tip_watchdog_get_stats(struct tip_watchdog_stats *out);

#endif /* ZCL_NET_TIP_WATCHDOG_H */
