/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * MCP Metrics — Prometheus-style in-process counters and histograms.
 *
 * Observes EV_MCP_REQUEST and maintains:
 *   - per-(tool, code) request counters
 *   - per-tool latency histogram (5 buckets: 1ms, 5ms, 25ms, 100ms, 500ms)
 *   - aggregated summary counters (total, ok, error, rate-limited, etc.)
 *
 * The metrics view is exposed by the `zcl_metrics` tool, which emits a
 * Prometheus text-format dump that any operator or scraper can consume.
 *
 * `zcl_metrics_reset` clears every counter (destructive-gated).
 */

#ifndef ZCL_MCP_METRICS_H
#define ZCL_MCP_METRICS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Max distinct tool names tracked.  Beyond this limit new tools are
 * folded into a "__other__" bucket rather than growing the table. */
#define MCP_METRICS_MAX_TOOLS 80

/* Max distinct (tool, code) pairs.  Bounded to keep memory predictable
 * and to protect against unbounded-cardinality attacks via tool spoofing. */
#define MCP_METRICS_MAX_COUNTERS 512

/* Histogram bucket count. */
#define MCP_METRICS_HIST_BUCKETS 6

/* Register the EV_MCP_REQUEST observer.  Idempotent — calling twice does
 * nothing on the second call. */
void mcp_metrics_init(void);

/* Manual counter increment — used by tests and call sites that don't
 * route through the event system.  Code "OK" for success; any other
 * string for an error code (AUTH_REQUIRED, RATE_LIMITED, …). */
void mcp_metrics_record(const char *tool, const char *code, int64_t dur_us);

/* Clear all counters.  Tests and `zcl_metrics_reset`. */
void mcp_metrics_reset(void);

/* Write the Prometheus text format dump into buf.  Returns bytes
 * written (excluding NUL).  Truncates silently if the buffer is small. */
size_t mcp_metrics_render_prometheus(char *buf, size_t cap);

/* Introspection (tests). */
size_t mcp_metrics_counter_count(void);
uint64_t mcp_metrics_get(const char *tool, const char *code);
uint64_t mcp_metrics_total_requests(void);
uint64_t mcp_metrics_total_errors(void);

/* ── Peer scoring counters ────────────────────────────────────
 *
 * Subscribed to EV_PEER_MISBEHAVE and EV_PEER_BANNED via the same
 * observer install path as the MCP request counters.  The handler
 * extracts the offence kind from the event payload (the first
 * whitespace-separated word after the score header) and buckets it
 * into a small allowlisted set; anything unrecognised goes into
 * "other".  Counts are exposed in the Prometheus dump as:
 *
 *   zcl_peer_offences_total{kind="..."} N
 *   zcl_peer_offences_total{kind="all"} N         # convenience aggregate
 *   zcl_peer_bans_total N
 *
 * The `zcl_peer_report` MCP tool wraps these in a small JSON object
 * with the live peer-scoring config so an operator can see the
 * threshold/decay/bans-since-boot in one call.
 */

/* Manual record helpers — used by tests and by the in-process event
 * observer.  `kind` should be one of the names returned by
 * peer_offence_name() (timeout, invalid_message, flood,
 * invalid_header, invalid_block) — anything else is folded into
 * "other" rather than expanding the cardinality. */
void mcp_metrics_record_peer_offence(const char *kind);
void mcp_metrics_record_peer_ban(void);

/* Aggregate query helpers (tests + zcl_peer_report). */
uint64_t mcp_metrics_peer_offences_total(void);
uint64_t mcp_metrics_peer_offences_for_kind(const char *kind);
uint64_t mcp_metrics_peer_bans_total(void);

/* Render the peer-scoring summary as a small JSON object suitable
 * for embedding in an MCP response body.  Includes the live config
 * (threshold / ban_hours / decay_per_min) and the per-kind counters.
 * Returns bytes written (excluding NUL); silently truncates on a
 * too-small buffer the same way the Prometheus dump does. */
size_t mcp_metrics_peer_report_json(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_MCP_METRICS_H */
