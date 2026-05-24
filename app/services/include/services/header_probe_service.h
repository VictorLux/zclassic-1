/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Header Probe Service — pull headers in bulk from a co-located
 * zclassicd (the legacy C++ ZClassic node, RPC 8232) via JSON-RPC
 * when our local header tip lags behind. Validates each fetched
 * header locally via accept_block_header() (Equihash + nBits
 * lineage + checkpoints) and inserts into our block_index.
 *
 * Why this exists:
 *   When P2P header sync stalls or is the bottleneck, the co-located
 *   zclassicd has all the headers and can serve them in single-digit
 *   milliseconds per call. This service closes the gap in seconds
 *   instead of hours.
 *
 * Architecture:
 *   - Pull loop calls getblockhash(h) → getblockheader(hash, false)
 *     for height range [start, start+max] on the local zclassicd.
 *   - Each returned hex-serialized header is parsed via
 *     block_header_deserialize and handed to accept_block_header().
 *   - Optional periodic tick on the unified heartbeat ring fires
 *     header_probe_pull_range whenever our header tip trails the
 *     remote tip by more than `lag_threshold`.
 *   - No new pthreads. Service is OPT-IN — boot.c does not start it.
 *
 * See CLAUDE.md "Adding state introspection" — this module follows
 * the *_dump_state_json convention and is wired into the generic
 * zcl_state dispatcher.
 */

#ifndef ZCL_SERVICES_HEADER_PROBE_H
#define ZCL_SERVICES_HEADER_PROBE_H

#include <stdbool.h>
#include <stdint.h>

struct main_state;
struct chain_params;
struct json_value;

struct header_probe_config {
    const char *rpc_host;       /* default "127.0.0.1" */
    int         rpc_port;       /* default 8232 */
    const char *rpc_user;       /* read zclassic.conf if NULL */
    const char *rpc_password;
    int         cadence_secs;   /* default 30 */
    int         batch_size;     /* default 2000; max 5000 */
    int         lag_threshold;  /* only probe when our_tip < their_tip - this; default 100 */
};

/* Apply config + load credentials. Safe to call before start to
 * override defaults. Idempotent. Returns false only on a missing
 * zclassic.conf when no user/password were supplied. */
bool header_probe_init(const struct header_probe_config *cfg,
                       struct main_state *ms,
                       const struct chain_params *params);

/* Register periodic tick on heartbeat ring. Idempotent.
 *
 * DEPRECATED in Phase 3 PR-1: prefer registering the
 * `header_probe_poll` Job (app/jobs/) with the network supervisor.
 * Still functional for legacy callers / tests. */
bool header_probe_start(void);

/* Unregister periodic tick. Idempotent. */
void header_probe_stop(void);

/* One-shot poll tick. Identical logic to the legacy heartbeat
 * callback: cheap getblockcount to discover remote tip, compare
 * against local header tip, and pull a batch if the lag threshold
 * is exceeded. Safe to call when not initialized (no-op).
 *
 * Used by the `header_probe_poll` Job (app/jobs/) as the body of
 * its supervisor tick callback. Pure scheduling separation — same
 * RPC, same validation, same accept_block_header path. */
void header_probe_tick_once(void);

/* Synchronous one-shot for the MCP tool + tests:
 *   start_height: where to begin (inclusive). Use our_tip+1 normally.
 *   max_headers:  cap on number of headers to pull in this call
 *                 (clamped to [1, 5000]).
 *   out_added:    receives the number successfully accepted (NULL OK).
 * Returns true if at least one header was added or we're already at
 * tip; false if init() hasn't been called or arguments are bogus. */
bool header_probe_pull_range(int start_height, int max_headers,
                             int *out_added);

/* Boot-time blocking pull: drain headers from `from_height` up to the
 * remote zclassicd's current tip in HP_MAX_BATCH-sized chunks. Used by
 * the local_chain_ingest phase-3 prelude to populate block_index so
 * per-block ingest can walk contiguously without waiting on P2P.
 *
 * Stops on: reaching the remote tip, an RPC failure (transient or
 * persistent), an accept_block_header rejection, or repeated zero-add
 * iterations.
 *
 *   out_total_added: count of successfully accepted headers (NULL OK).
 *   out_remote_tip:  remote zclassicd tip at first fetch (NULL OK).
 *
 * Returns true when we reached remote tip; false otherwise (RPC
 * unavailable, init not called, or pull stalled before tip). The
 * detailed reason is in header_probe_stats. */
bool header_probe_pull_range_blocking(int from_height,
                                      int *out_total_added,
                                      int *out_remote_tip);

/* zcl_state subsystem=header_probe dispatcher entry. See CLAUDE.md
 * "Adding state introspection". Reentrant-safe. */
bool header_probe_dump_state_json(struct json_value *out, const char *key);

struct header_probe_stats {
    int64_t calls_total;
    int64_t headers_added;
    int64_t headers_rejected;
    int64_t rpc_errors;
    int     last_remote_height;
    int     last_local_height;
};
void header_probe_stats_snapshot(struct header_probe_stats *out);

/* Test hooks — reset state between unit tests. */
void header_probe_reset_for_test(void);

#endif /* ZCL_SERVICES_HEADER_PROBE_H */
