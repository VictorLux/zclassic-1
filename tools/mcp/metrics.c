/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * MCP Metrics — implementation.  See metrics.h for the contract.
 */

#include "mcp/metrics.h"
#include "event/event.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* Bucket upper bounds in microseconds — matches Prometheus "le" labels.
 * Last bucket (+Inf) is implicit via the _count total. */
static const int64_t k_bucket_us[MCP_METRICS_HIST_BUCKETS] = {
    1000,       /* 1   ms */
    5000,       /* 5   ms */
    25000,      /* 25  ms */
    100000,     /* 100 ms */
    500000,     /* 500 ms */
    2000000,    /* 2000 ms */
};

static const char *k_bucket_le_label[MCP_METRICS_HIST_BUCKETS] = {
    "0.001", "0.005", "0.025", "0.1", "0.5", "2.0",
};

struct counter_entry {
    char     tool[48];
    char     code[24];
    uint64_t count;
};

struct tool_hist {
    char     tool[48];
    uint64_t buckets[MCP_METRICS_HIST_BUCKETS];  /* le bucket counts (non-cumulative) */
    uint64_t overflow;                           /* > last bucket */
    uint64_t total_count;                        /* _count */
    uint64_t total_us;                           /* running sum (for _sum) */
};

static struct counter_entry g_counters[MCP_METRICS_MAX_COUNTERS];
static size_t               g_counter_count;

/* +1 slot reserved for the "__other__" overflow bucket so that when we
 * hit MCP_METRICS_MAX_TOOLS we can still create the fold-in entry. */
static struct tool_hist     g_hists[MCP_METRICS_MAX_TOOLS + 1];
static size_t               g_hist_count;

static uint64_t             g_total_requests;
static uint64_t             g_total_errors;

static pthread_mutex_t      g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool                 g_observer_installed = false;

/* ── Parsers ────────────────────────────────────────────────── */

/* Extract a field value from a "key=value ..." payload.  Writes up to
 * cap-1 chars into out.  Stops at space or end.  Returns true if found. */
static bool parse_kv(const char *payload, size_t len, const char *key,
                     char *out, size_t cap)
{
    if (cap == 0) return false;
    out[0] = '\0';
    size_t klen = strlen(key);
    for (size_t i = 0; i < len; ) {
        /* Skip leading spaces */
        while (i < len && payload[i] == ' ') i++;
        /* Match key= */
        if (i + klen + 1 <= len &&
            strncmp(payload + i, key, klen) == 0 &&
            payload[i + klen] == '=') {
            size_t j = i + klen + 1;
            size_t o = 0;
            while (j < len && payload[j] != ' ' && o + 1 < cap) {
                out[o++] = payload[j++];
            }
            out[o] = '\0';
            return true;
        }
        /* Skip token */
        while (i < len && payload[i] != ' ') i++;
    }
    return false;
}

/* ── Registry ops ───────────────────────────────────────────── */

static int hist_slot(const char *tool)
{
    for (size_t i = 0; i < g_hist_count; i++)
        if (strncmp(g_hists[i].tool, tool, sizeof(g_hists[i].tool)) == 0)
            return (int)i;
    if (g_hist_count >= MCP_METRICS_MAX_TOOLS) {
        /* Fold into "__other__" */
        for (size_t i = 0; i < g_hist_count; i++)
            if (strcmp(g_hists[i].tool, "__other__") == 0) return (int)i;
        /* Create it */
        size_t idx = g_hist_count++;
        memset(&g_hists[idx], 0, sizeof(g_hists[idx]));
        snprintf(g_hists[idx].tool, sizeof(g_hists[idx].tool), "%s", "__other__");
        return (int)idx;
    }
    size_t idx = g_hist_count++;
    memset(&g_hists[idx], 0, sizeof(g_hists[idx]));
    snprintf(g_hists[idx].tool, sizeof(g_hists[idx].tool), "%s", tool);
    return (int)idx;
}

static int counter_slot(const char *tool, const char *code)
{
    for (size_t i = 0; i < g_counter_count; i++)
        if (strncmp(g_counters[i].tool, tool, sizeof(g_counters[i].tool)) == 0 &&
            strncmp(g_counters[i].code, code, sizeof(g_counters[i].code)) == 0)
            return (int)i;
    if (g_counter_count >= MCP_METRICS_MAX_COUNTERS) return -1;
    size_t idx = g_counter_count++;
    snprintf(g_counters[idx].tool, sizeof(g_counters[idx].tool), "%s", tool);
    snprintf(g_counters[idx].code, sizeof(g_counters[idx].code), "%s", code);
    g_counters[idx].count = 0;
    return (int)idx;
}

static void record_locked(const char *tool, const char *code, int64_t dur_us)
{
    if (!tool || !tool[0]) tool = "__missing__";
    if (!code || !code[0]) code = "UNKNOWN";

    int ci = counter_slot(tool, code);
    if (ci >= 0) g_counters[ci].count++;

    g_total_requests++;
    if (strcmp(code, "OK") != 0) g_total_errors++;

    if (dur_us >= 0) {
        int hi = hist_slot(tool);
        if (hi >= 0) {
            struct tool_hist *h = &g_hists[hi];
            bool placed = false;
            for (int b = 0; b < MCP_METRICS_HIST_BUCKETS; b++) {
                if (dur_us <= k_bucket_us[b]) {
                    h->buckets[b]++;
                    placed = true;
                    break;
                }
            }
            if (!placed) h->overflow++;
            h->total_count++;
            h->total_us += (uint64_t)dur_us;
        }
    }
}

void mcp_metrics_record(const char *tool, const char *code, int64_t dur_us)
{
    pthread_mutex_lock(&g_lock);
    record_locked(tool, code, dur_us);
    pthread_mutex_unlock(&g_lock);
}

void mcp_metrics_reset(void)
{
    pthread_mutex_lock(&g_lock);
    g_counter_count = 0;
    g_hist_count = 0;
    g_total_requests = 0;
    g_total_errors = 0;
    memset(g_counters, 0, sizeof(g_counters));
    memset(g_hists, 0, sizeof(g_hists));
    pthread_mutex_unlock(&g_lock);
}

/* ── Event observer ─────────────────────────────────────────── */

static void mcp_request_observer(enum event_type type, uint32_t peer_id,
                                  const void *payload, uint32_t payload_len,
                                  void *ctx)
{
    (void)peer_id; (void)ctx;
    if (type != EV_MCP_REQUEST) return;
    if (!payload || payload_len == 0) return;

    char tool[48], code[24], dur_str[24];
    if (!parse_kv(payload, payload_len, "tool", tool, sizeof(tool))) return;
    if (!parse_kv(payload, payload_len, "code", code, sizeof(code))) return;

    int64_t dur = -1;
    if (parse_kv(payload, payload_len, "dur_us", dur_str, sizeof(dur_str)))
        dur = strtoll(dur_str, NULL, 10);

    mcp_metrics_record(tool, code, dur);
}

void mcp_metrics_init(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_observer_installed) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    event_observe(EV_MCP_REQUEST, mcp_request_observer, NULL);
    g_observer_installed = true;
    pthread_mutex_unlock(&g_lock);
}

/* ── Introspection ──────────────────────────────────────────── */

size_t mcp_metrics_counter_count(void)
{
    pthread_mutex_lock(&g_lock);
    size_t n = g_counter_count;
    pthread_mutex_unlock(&g_lock);
    return n;
}

uint64_t mcp_metrics_get(const char *tool, const char *code)
{
    pthread_mutex_lock(&g_lock);
    uint64_t v = 0;
    for (size_t i = 0; i < g_counter_count; i++) {
        if (strcmp(g_counters[i].tool, tool) == 0 &&
            strcmp(g_counters[i].code, code) == 0) {
            v = g_counters[i].count;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return v;
}

uint64_t mcp_metrics_total_requests(void)
{
    pthread_mutex_lock(&g_lock);
    uint64_t v = g_total_requests;
    pthread_mutex_unlock(&g_lock);
    return v;
}

uint64_t mcp_metrics_total_errors(void)
{
    pthread_mutex_lock(&g_lock);
    uint64_t v = g_total_errors;
    pthread_mutex_unlock(&g_lock);
    return v;
}

/* ── Prometheus text format ─────────────────────────────────── */

__attribute__((format(printf, 4, 5)))
static size_t append(char *buf, size_t cap, size_t pos, const char *fmt, ...)
{
    if (pos >= cap) return pos;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + pos, cap - pos, fmt, ap);
    va_end(ap);
    if (n < 0) return pos;
    if ((size_t)n >= cap - pos) return cap - 1;
    return pos + (size_t)n;
}

size_t mcp_metrics_render_prometheus(char *buf, size_t cap)
{
    if (!buf || cap == 0) return 0;
    pthread_mutex_lock(&g_lock);

    size_t pos = 0;
    pos = append(buf, cap, pos,
        "# HELP zcl_mcp_requests_total MCP requests labeled by tool + code\n"
        "# TYPE zcl_mcp_requests_total counter\n");
    for (size_t i = 0; i < g_counter_count; i++) {
        pos = append(buf, cap, pos,
            "zcl_mcp_requests_total{tool=\"%s\",code=\"%s\"} %llu\n",
            g_counters[i].tool, g_counters[i].code,
            (unsigned long long)g_counters[i].count);
    }

    pos = append(buf, cap, pos,
        "# HELP zcl_mcp_request_duration_seconds Histogram of handler durations\n"
        "# TYPE zcl_mcp_request_duration_seconds histogram\n");
    for (size_t i = 0; i < g_hist_count; i++) {
        const struct tool_hist *h = &g_hists[i];
        uint64_t cumulative = 0;
        for (int b = 0; b < MCP_METRICS_HIST_BUCKETS; b++) {
            cumulative += h->buckets[b];
            pos = append(buf, cap, pos,
                "zcl_mcp_request_duration_seconds_bucket{tool=\"%s\",le=\"%s\"} %llu\n",
                h->tool, k_bucket_le_label[b], (unsigned long long)cumulative);
        }
        cumulative += h->overflow;
        pos = append(buf, cap, pos,
            "zcl_mcp_request_duration_seconds_bucket{tool=\"%s\",le=\"+Inf\"} %llu\n",
            h->tool, (unsigned long long)cumulative);
        pos = append(buf, cap, pos,
            "zcl_mcp_request_duration_seconds_sum{tool=\"%s\"} %.6f\n",
            h->tool, (double)h->total_us / 1000000.0);
        pos = append(buf, cap, pos,
            "zcl_mcp_request_duration_seconds_count{tool=\"%s\"} %llu\n",
            h->tool, (unsigned long long)h->total_count);
    }

    pos = append(buf, cap, pos,
        "# HELP zcl_mcp_requests_summary_total Aggregate MCP request counts\n"
        "# TYPE zcl_mcp_requests_summary_total counter\n"
        "zcl_mcp_requests_summary_total{kind=\"total\"} %llu\n"
        "zcl_mcp_requests_summary_total{kind=\"error\"} %llu\n",
        (unsigned long long)g_total_requests,
        (unsigned long long)g_total_errors);

    if (pos < cap) buf[pos] = '\0';
    pthread_mutex_unlock(&g_lock);
    return pos;
}
