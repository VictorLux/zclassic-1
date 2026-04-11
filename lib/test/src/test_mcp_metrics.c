/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for the MCP metrics subsystem — counter registry, Prometheus
 * text rendering, observer hookup via EV_MCP_REQUEST, and reset
 * semantics.
 */

#include "test/test_helpers.h"
#include "mcp/metrics.h"
#include "event/event.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

static bool contains(const char *hay, const char *needle)
{
    return hay && needle && strstr(hay, needle) != NULL;
}

static int test_reset_empty(void)
{
    int failures = 0;
    TEST("metrics: reset produces an empty registry") {
        mcp_metrics_reset();
        ASSERT(mcp_metrics_counter_count() == 0);
        ASSERT(mcp_metrics_total_requests() == 0);
        ASSERT(mcp_metrics_total_errors() == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int test_record_counts(void)
{
    int failures = 0;
    TEST("metrics: record increments per-(tool, code) counters") {
        mcp_metrics_reset();
        mcp_metrics_record("zcl_status", "OK", 1200);
        mcp_metrics_record("zcl_status", "OK", 4500);
        mcp_metrics_record("zcl_status", "OK", 12000);
        mcp_metrics_record("zcl_status", "MISSING_PARAM", 500);
        mcp_metrics_record("zcl_getblock", "OK", 800);

        ASSERT(mcp_metrics_get("zcl_status", "OK") == 3);
        ASSERT(mcp_metrics_get("zcl_status", "MISSING_PARAM") == 1);
        ASSERT(mcp_metrics_get("zcl_getblock", "OK") == 1);
        ASSERT(mcp_metrics_total_requests() == 5);
        ASSERT(mcp_metrics_total_errors() == 1);
        PASS();
    } _test_next:;
    return failures;
}

static int test_histogram_buckets(void)
{
    int failures = 0;
    TEST("metrics: histogram buckets catch under- and over-flow") {
        mcp_metrics_reset();
        mcp_metrics_record("t1", "OK", 500);          /* 0.5 ms → 0.001 */
        mcp_metrics_record("t1", "OK", 3000);         /* 3 ms   → 0.005 */
        mcp_metrics_record("t1", "OK", 20000);        /* 20 ms  → 0.025 */
        mcp_metrics_record("t1", "OK", 80000);        /* 80 ms  → 0.1 */
        mcp_metrics_record("t1", "OK", 300000);       /* 300 ms → 0.5 */
        mcp_metrics_record("t1", "OK", 1500000);      /* 1.5 s  → 2.0 */
        mcp_metrics_record("t1", "OK", 5000000);      /* 5 s    → +Inf */

        char buf[8192];
        mcp_metrics_render_prometheus(buf, sizeof(buf));

        /* Cumulative le bucket counts for tool t1 */
        ASSERT(contains(buf, "zcl_mcp_request_duration_seconds_bucket{tool=\"t1\",le=\"0.001\"} 1"));
        ASSERT(contains(buf, "zcl_mcp_request_duration_seconds_bucket{tool=\"t1\",le=\"0.005\"} 2"));
        ASSERT(contains(buf, "zcl_mcp_request_duration_seconds_bucket{tool=\"t1\",le=\"0.025\"} 3"));
        ASSERT(contains(buf, "zcl_mcp_request_duration_seconds_bucket{tool=\"t1\",le=\"0.1\"} 4"));
        ASSERT(contains(buf, "zcl_mcp_request_duration_seconds_bucket{tool=\"t1\",le=\"0.5\"} 5"));
        ASSERT(contains(buf, "zcl_mcp_request_duration_seconds_bucket{tool=\"t1\",le=\"2.0\"} 6"));
        ASSERT(contains(buf, "zcl_mcp_request_duration_seconds_bucket{tool=\"t1\",le=\"+Inf\"} 7"));
        ASSERT(contains(buf, "zcl_mcp_request_duration_seconds_count{tool=\"t1\"} 7"));
        PASS();
    } _test_next:;
    return failures;
}

static int test_prometheus_format(void)
{
    int failures = 0;
    TEST("metrics: Prometheus text starts with HELP/TYPE lines") {
        mcp_metrics_reset();
        mcp_metrics_record("zcl_kpi", "OK", 100);

        char buf[4096];
        size_t n = mcp_metrics_render_prometheus(buf, sizeof(buf));
        ASSERT(n > 0);
        ASSERT(contains(buf, "# HELP zcl_mcp_requests_total"));
        ASSERT(contains(buf, "# TYPE zcl_mcp_requests_total counter"));
        ASSERT(contains(buf,
            "zcl_mcp_requests_total{tool=\"zcl_kpi\",code=\"OK\"} 1"));
        ASSERT(contains(buf, "# HELP zcl_mcp_request_duration_seconds"));
        ASSERT(contains(buf, "zcl_mcp_requests_summary_total{kind=\"total\"} 1"));
        ASSERT(contains(buf, "zcl_mcp_requests_summary_total{kind=\"error\"} 0"));
        PASS();
    } _test_next:;
    return failures;
}

static int test_reset_clears(void)
{
    int failures = 0;
    TEST("metrics: reset clears counters and histograms") {
        mcp_metrics_reset();
        mcp_metrics_record("zcl_status", "OK", 1000);
        mcp_metrics_record("zcl_balance", "OK", 2000);
        ASSERT(mcp_metrics_counter_count() == 2);

        mcp_metrics_reset();
        ASSERT(mcp_metrics_counter_count() == 0);
        ASSERT(mcp_metrics_total_requests() == 0);
        ASSERT(mcp_metrics_get("zcl_status", "OK") == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int test_observer_hookup(void)
{
    int failures = 0;
    TEST("metrics: EV_MCP_REQUEST observer accumulates counters") {
        event_log_init();
        event_clear_observers(EV_MCP_REQUEST);
        mcp_metrics_init();
        mcp_metrics_reset();

        event_emitf(EV_MCP_REQUEST, 0,
                    "tool=zcl_status code=OK dur_us=1500");
        event_emitf(EV_MCP_REQUEST, 0,
                    "tool=zcl_status code=OK dur_us=800");
        event_emitf(EV_MCP_REQUEST, 0,
                    "tool=zcl_send code=RATE_LIMITED kind=destructive");

        ASSERT(mcp_metrics_get("zcl_status", "OK") == 2);
        ASSERT(mcp_metrics_get("zcl_send", "RATE_LIMITED") == 1);
        ASSERT(mcp_metrics_total_requests() >= 3);
        ASSERT(mcp_metrics_total_errors() >= 1);
        PASS();
    } _test_next:;
    return failures;
}

static int test_cardinality_cap(void)
{
    int failures = 0;
    TEST("metrics: runaway tool names fold into __other__") {
        mcp_metrics_reset();
        for (int i = 0; i < MCP_METRICS_MAX_TOOLS + 10; i++) {
            char name[32];
            snprintf(name, sizeof(name), "tool_%d", i);
            mcp_metrics_record(name, "OK", 100);
        }
        char buf[131072];
        mcp_metrics_render_prometheus(buf, sizeof(buf));
        ASSERT(contains(buf, "__other__"));
        PASS();
    } _test_next:;
    return failures;
}

static int test_envelope_truncation(void)
{
    int failures = 0;
    TEST("metrics: render handles tiny buffers gracefully") {
        mcp_metrics_reset();
        mcp_metrics_record("zcl_status", "OK", 1000);

        char small[64];
        size_t n = mcp_metrics_render_prometheus(small, sizeof(small));
        ASSERT(n < sizeof(small));
        ASSERT(small[n] == '\0' || small[sizeof(small) - 1] == '\0');
        PASS();
    } _test_next:;
    return failures;
}

/* ── Entry point ────────────────────────────────────────────── */

int test_mcp_metrics(void);

int test_mcp_metrics(void)
{
    int failures = 0;
    event_log_init();

    failures += test_reset_empty();
    failures += test_record_counts();
    failures += test_histogram_buckets();
    failures += test_prometheus_format();
    failures += test_reset_clears();
    failures += test_observer_hookup();
    failures += test_cardinality_cap();
    failures += test_envelope_truncation();

    mcp_metrics_reset();
    return failures;
}
