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

#ifdef __cplusplus
}
#endif

#endif /* ZCL_MCP_METRICS_H */
