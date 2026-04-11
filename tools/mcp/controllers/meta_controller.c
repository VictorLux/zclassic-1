/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * MCP meta controller: operator tools that introspect the MCP surface
 * itself.  These tools are domain="ops" but live in their own file
 * because they pull in the router internals.
 *
 *   zcl_tools_list — dump the routing table as JSON
 *   zcl_self_test  — call every tool with safe defaults, report pass/fail
 *   zcl_logtail    — tail the structured event log, optional domain filter
 */

#include "../controllers.h"
#include "../router.h"
#include "../rpc_client.h"
#include "../metrics.h"

#include "json/json.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Destructive tool list ───────────────────────────────────── */
/* self_test skips these because they modify state on the node, the
 * network, or the wallet.  Keep this list in sync with any new routes
 * that write externally.  (A future version could add a `flags` field
 * on mcp_tool_route — for now a central list is simpler.) */
static const char *const k_destructive[] = {
    /* wallet */
    "zcl_send",
    "zcl_sendtoaddress",
    "zcl_importprivkey",
    "zcl_rescanblockchain",
    "zcl_replaywalletfromchain",
    "zcl_dumpprivkey",           /* exposes secrets — treat as destructive */
    /* net */
    "zcl_addnode",
    "zcl_pingpeer",              /* fires a P2P message */
    /* app (state-modifying) */
    "zcl_name_register",
    "zcl_msg_send",
    "zcl_msg_send_named",
    "zcl_msg_read",              /* mutates read-state */
    "zcl_market_offer",
    "zcl_market_buy",
    "zcl_swap_initiate",
    "zcl_swap_participate",
    /* meta-tools */
    "zcl_self_test",             /* avoid recursion */
    "zcl_rpc",                   /* arbitrary RPC — skip by default */
    "zcl_metrics_reset",         /* resets metric counters — treat as destructive */
};

static bool is_destructive(const char *name)
{
    for (size_t i = 0; i < sizeof(k_destructive)/sizeof(k_destructive[0]); i++)
        if (strcmp(k_destructive[i], name) == 0)
            return true;
    return false;
}

/* ── Self-test argument overrides ─────────────────────────────── */
/* Some tools have a required param the router has no default for, but
 * a well-known, safe value still exercises the code path.  self_test
 * looks up this table before skipping.  The overrides are strings so
 * they survive router validation (values are re-parsed per call). */
static const struct {
    const char *tool;
    const char *args_json;
} k_self_test_overrides[] = {
    /* Block 1 exists on every synced node; verbosity defaults to JSON. */
    { "zcl_getblock",    "{\"block_id\":\"1\"}" },
    /* Safe: returns a "not found" body for an unregistered name. */
    { "zcl_name_resolve", "{\"name\":\"__self_test_probe__\"}" },
    /* zcl_profile sleeps duration_ms per call — clamp to 100ms in
     * self_test so the whole sweep doesn't balloon by a second on
     * every run. */
    { "zcl_profile",     "{\"duration_ms\":100,\"top_n\":3}" },
};

static const char *self_test_override_args(const char *tool)
{
    for (size_t i = 0;
         i < sizeof(k_self_test_overrides)/sizeof(k_self_test_overrides[0]);
         i++) {
        if (strcmp(k_self_test_overrides[i].tool, tool) == 0)
            return k_self_test_overrides[i].args_json;
    }
    return NULL;
}

/* True if any required param has no default value we can synthesize. */
static bool has_unfillable_required(const struct mcp_tool_route *r)
{
    for (size_t i = 0; i < r->num_params; i++) {
        const struct mcp_param_spec *p = &r->params[i];
        if (p->required && !p->default_json)
            return true;
    }
    return false;
}

/* ── Handlers ────────────────────────────────────────────────── */

static int h_zcl_tools_list(const struct mcp_request *req,
                             struct mcp_response *res)
{
    (void)req;
    size_t cap = 131072;
    char *out = malloc(cap);
    if (!out) return -1;
    int pos = snprintf(out, cap, "{\"count\":%zu,\"tools\":",
                       mcp_router_count());
    pos += (int)mcp_router_tools_list_json(out + pos, cap - (size_t)pos);
    if ((size_t)pos + 2 < cap) { out[pos++] = '}'; out[pos] = 0; }
    res->body = out;
    return 0;
}

static int h_zcl_self_test(const struct mcp_request *req,
                            struct mcp_response *res)
{
    (void)req;

    size_t cap = 131072;
    char *out = malloc(cap);
    if (!out) return -1;
    size_t pos = 0;
    pos += (size_t)snprintf(out + pos, cap - pos,
                            "{\"results\":[");

    size_t total = 0, passed = 0, failed = 0, skipped = 0;
    bool first = true;

    for (size_t i = 0; i < mcp_router_count(); i++) {
        const struct mcp_tool_route *r = mcp_router_at(i);
        if (!r) continue;
        total++;

        const char *status;
        const char *reason = NULL;

        const char *override = self_test_override_args(r->name);
        struct json_value override_val = {0};
        bool have_override = false;
        if (override && json_read(&override_val, override, strlen(override)))
            have_override = true;

        if (is_destructive(r->name)) {
            status = "skipped";
            reason = "destructive";
            skipped++;
        } else if (!have_override && has_unfillable_required(r)) {
            status = "skipped";
            reason = "required-param-without-default";
            skipped++;
        } else {
            /* Call the tool with the override args if we have one;
             * otherwise empty args — optional-default params will fall
             * through to router defaults. */
            char *body = mcp_router_dispatch(
                r->name, have_override ? &override_val : NULL);
            if (!body) {
                status = "fail";
                reason = "no-body";
                failed++;
            } else if (strstr(body, "\"error\":{\"code\":") != NULL) {
                status = "fail";
                reason = "error-envelope";
                failed++;
            } else {
                status = "pass";
                passed++;
            }
            free(body);
        }

        if (have_override) json_free(&override_val);

        if (pos + 256 >= cap) break;
        if (!first) out[pos++] = ',';
        first = false;
        pos += (size_t)snprintf(out + pos, cap - pos,
            "{\"tool\":\"%s\",\"domain\":\"%s\",\"status\":\"%s\"%s%s%s}",
            r->name, r->domain ? r->domain : "",
            status,
            reason ? ",\"reason\":\"" : "",
            reason ? reason : "",
            reason ? "\"" : "");
    }

    pos += (size_t)snprintf(out + pos, cap - pos,
        "],\"summary\":{\"total\":%zu,\"pass\":%zu,\"fail\":%zu,"
        "\"skip\":%zu}}",
        total, passed, failed, skipped);

    res->body = out;
    return 0;
}

static int h_zcl_logtail(const struct mcp_request *req,
                          struct mcp_response *res)
{
    const struct json_value *cnt = json_get(req->args, "count");
    const struct json_value *df  = json_get(req->args, "domain");
    int count = cnt ? (int)json_get_int(cnt) : 100;
    const char *dom = df ? json_get_str(df) : NULL;

    char params[64];
    snprintf(params, sizeof(params), "[%d]", count);
    char *raw = mcp_node_rpc("eventlog", params);
    if (!raw) return -1;
    if (!dom || !dom[0]) { res->body = raw; return 0; }

    /* Parse the eventlog response and filter events[] by type prefix. */
    struct json_value root;
    if (!json_read(&root, raw, strlen(raw))) {
        res->body = raw;
        return 0;
    }
    free(raw);

    const struct json_value *events = json_get(&root, "events");
    const struct json_value *ss     = json_get(&root, "sync_state");
    const char *sstate = ss ? json_get_str(ss) : "";

    size_t out_cap = 65536;
    char *out = malloc(out_cap);
    if (!out) { json_free(&root); return -1; }
    size_t pos = 0;
    pos += (size_t)snprintf(out + pos, out_cap - pos,
                            "{\"sync_state\":\"%s\",\"filter\":\"", sstate);
    for (const char *c = dom; *c && pos + 4 < out_cap; c++) {
        if (*c == '"' || *c == '\\') out[pos++] = '\\';
        out[pos++] = *c;
    }
    pos += (size_t)snprintf(out + pos, out_cap - pos, "\",\"events\":[");

    bool first = true;
    size_t dlen = strlen(dom);
    size_t matched = 0;
    if (events && events->type == JSON_ARR) {
        for (size_t i = 0; i < events->num_children; i++) {
            const struct json_value *ev = &events->children[i];
            const struct json_value *ty = json_get(ev, "type");
            const char *tstr = ty ? json_get_str(ty) : "";
            if (!tstr) continue;
            if (strncmp(tstr, dom, dlen) != 0) continue;

            /* Write this event object */
            if (pos + 2048 >= out_cap) break;
            if (!first) out[pos++] = ',';
            first = false;
            pos += json_write(ev, out + pos, out_cap - pos);
            matched++;
        }
    }
    pos += (size_t)snprintf(out + pos, out_cap - pos,
                            "],\"matched\":%zu}", matched);

    json_free(&root);
    res->body = out;
    return 0;
}

/* ── Schema export (OpenAPI-ish) ─────────────────────────────── */

static int h_zcl_openapi(const struct mcp_request *req,
                          struct mcp_response *res)
{
    (void)req;
    size_t cap = 262144;
    char *out = malloc(cap);
    if (!out) return -1;
    size_t pos = 0;

    pos += (size_t)snprintf(out + pos, cap - pos,
        "{"
        "\"openapi\":\"3.0.0\","
        "\"info\":{\"title\":\"zclassic23 MCP surface\","
                 "\"version\":\"1.0.0\","
                 "\"description\":\"Auto-derived from the MCP router table.\"},"
        "\"paths\":{");

    bool first = true;
    for (size_t i = 0; i < mcp_router_count(); i++) {
        const struct mcp_tool_route *r = mcp_router_at(i);
        if (!r || !r->name) continue;

        if (pos + 4096 >= cap) break;
        if (!first) out[pos++] = ',';
        first = false;

        /* Path entry: "/tools/<name>": { post: { summary, tags, requestBody:
         * { content: { application/json: { schema: <inputSchema> } } },
         * responses: { 200, 4xx } } } */
        pos += (size_t)snprintf(out + pos, cap - pos,
            "\"/tools/%s\":{\"post\":{"
            "\"summary\":\"", r->name);

        /* description (JSON-escape) */
        const char *desc = r->description ? r->description : "";
        for (size_t k = 0; desc[k] && pos + 4 < cap; k++) {
            char c = desc[k];
            if (c == '"' || c == '\\') out[pos++] = '\\';
            if (c == '\n') { out[pos++] = '\\'; c = 'n'; }
            out[pos++] = c;
        }

        pos += (size_t)snprintf(out + pos, cap - pos,
            "\",\"tags\":[\"%s\"],"
            "\"operationId\":\"%s\","
            "\"requestBody\":{\"content\":{\"application/json\":{\"schema\":",
            r->domain ? r->domain : "default", r->name);

        /* inputSchema object */
        pos += mcp_router_input_schema_json(r, out + pos, cap - pos);

        pos += (size_t)snprintf(out + pos, cap - pos,
            "}}},"
            "\"responses\":{"
            "\"200\":{\"description\":\"Success — JSON body from handler\"},"
            "\"400\":{\"description\":\"Validation error envelope\"},"
            "\"401\":{\"description\":\"AUTH_REQUIRED envelope\"},"
            "\"429\":{\"description\":\"RATE_LIMITED envelope\"},"
            "\"504\":{\"description\":\"TOOL_TIMEOUT envelope\"}"
            "}}}");
    }

    pos += (size_t)snprintf(out + pos, cap - pos,
        "},"
        "\"components\":{\"schemas\":{"
        "\"ErrorEnvelope\":{\"type\":\"object\",\"properties\":{"
        "\"error\":{\"type\":\"object\",\"properties\":{"
        "\"code\":{\"type\":\"string\"},"
        "\"message\":{\"type\":\"string\"},"
        "\"tool\":{\"type\":\"string\"},"
        "\"param\":{\"type\":\"string\"}"
        "}}}}"
        "}}}");

    if (pos < cap) out[pos] = 0;
    res->body = out;
    return 0;
}

/* ── Prometheus metrics ─────────────────────────────────────── */

static int h_zcl_metrics(const struct mcp_request *req,
                          struct mcp_response *res)
{
    (void)req;
    size_t cap = 131072;
    char *raw = malloc(cap);
    if (!raw) return -1;
    size_t n = mcp_metrics_render_prometheus(raw, cap);

    /* Wrap the Prometheus text in a JSON envelope so the stdio layer
     * can shuttle it as a tool result.  Escape quotes + newlines. */
    size_t out_cap = n * 2 + 128;
    char *out = malloc(out_cap);
    if (!out) { free(raw); return -1; }
    size_t pos = 0;
    pos += (size_t)snprintf(out + pos, out_cap - pos,
        "{\"format\":\"prometheus\",\"text\":\"");
    for (size_t i = 0; i < n && pos + 4 < out_cap; i++) {
        char c = raw[i];
        if (c == '"')       { out[pos++] = '\\'; out[pos++] = '"'; }
        else if (c == '\\') { out[pos++] = '\\'; out[pos++] = '\\'; }
        else if (c == '\n') { out[pos++] = '\\'; out[pos++] = 'n'; }
        else if (c == '\r') { out[pos++] = '\\'; out[pos++] = 'r'; }
        else if (c == '\t') { out[pos++] = '\\'; out[pos++] = 't'; }
        else                { out[pos++] = c; }
    }
    pos += (size_t)snprintf(out + pos, out_cap - pos,
        "\",\"total_requests\":%llu,\"total_errors\":%llu,\"counter_count\":%zu}",
        (unsigned long long)mcp_metrics_total_requests(),
        (unsigned long long)mcp_metrics_total_errors(),
        mcp_metrics_counter_count());

    free(raw);
    res->body = out;
    return 0;
}

static int h_zcl_metrics_reset(const struct mcp_request *req,
                                struct mcp_response *res)
{
    (void)req;
    mcp_metrics_reset();
    char *out = strdup("{\"ok\":true,\"reset\":\"mcp_metrics\"}");
    if (!out) return -1;
    res->body = out;
    return 0;
}

/* zcl_rpc_report — HTTP RPC middleware summary (wave 5 session 1).
 * Live config + stat counters + tracked IPs + active bans from the
 * global rpc_http_middleware registered by httpserver.c.  The report
 * also appears in the Prometheus dump emitted by zcl_metrics, but
 * this tool returns a smaller structured JSON object for operators
 * who want a single-call snapshot instead of a full text scrape. */
static int h_zcl_rpc_report(const struct mcp_request *req,
                             struct mcp_response *res)
{
    (void)req;
    char body[2048];
    size_t n = mcp_metrics_rpc_report_json(body, sizeof(body));
    if (n == 0) return -1;
    res->body = strdup(body);
    return res->body ? 0 : -1;
}

/* ── Admin dashboard (wave 5 #5) ──────────────────────────────
 *
 * zcl_admin is a composite snapshot tool: it dispatches the existing
 * observability tools (zcl_kpi / zcl_peer_report / zcl_rpc_report /
 * zcl_events) in-process, stitches the raw bodies into one envelope,
 * and derives a small "alerts" array from threshold conditions over
 * the nested counters.
 *
 * "Handles missing subsystems gracefully" means: if any sub-dispatch
 * returns an error envelope (tool missing, handler failed, RPC node
 * offline in test mode), that slot is rendered as JSON null and the
 * other slots still populate.  Tests rely on this — they exercise
 * zcl_admin with no live RPC backend and still get back a parseable
 * document.
 *
 * The `since` parameter is accepted for API stability but currently
 * has no runtime effect: the nested counters are cumulative since
 * boot, not windowed.  A future session can add a baseline-snapshot
 * layer to make `since` meaningful.
 */

/* Write `body` into dst as an embedded JSON value.  If body is NULL,
 * looks like an error envelope ({"error":{...}}), or fails to parse
 * as JSON (some legacy sub-tools splice raw "Unauthorized"-style RPC
 * error strings into their output — that's valid for their own
 * downstream consumers but not for embedding inside a composite
 * envelope), writes "null" instead.  Returns bytes written. */
static size_t embed_or_null(const char *body, char *dst, size_t cap)
{
    if (cap == 0) return 0;
    if (!body || strncmp(body, "{\"error\":", 9) == 0) {
        if (cap < 5) return 0;
        memcpy(dst, "null", 4);
        return 4;
    }
    /* Validate as JSON first — reject bare-word "Unauthorized" etc.
     * json_read is forgiving about trailing whitespace and returns
     * false only on a real parse error, so this is a structural
     * guard rather than a strict dialect check. */
    struct json_value probe = {0};
    if (!json_read(&probe, body, strlen(body))) {
        if (cap < 5) return 0;
        memcpy(dst, "null", 4);
        return 4;
    }
    json_free(&probe);

    size_t n = strlen(body);
    if (n >= cap) n = cap - 1;
    memcpy(dst, body, n);
    return n;
}

/* Quick substring lookup on a JSON body to pull an integer out for
 * the alerts heuristics.  Returns -1 if the key isn't present or can't
 * be parsed as an integer.  Intentionally lightweight — we don't need
 * a full parser here because we only read counters we emit ourselves. */
static long long scan_int_field(const char *body, const char *key)
{
    if (!body || !key) return -1;
    char needle[64];
    int n = snprintf(needle, sizeof(needle), "\"%s\":", key);
    if (n <= 0 || (size_t)n >= sizeof(needle)) return -1;
    const char *p = strstr(body, needle);
    if (!p) return -1;
    p += (size_t)n;
    while (*p == ' ') p++;
    if (*p == '\0') return -1;
    char *end = NULL;
    long long v = strtoll(p, &end, 10);
    if (end == p) return -1;
    return v;
}

static int h_zcl_admin(const struct mcp_request *req,
                        struct mcp_response *res)
{
    const struct json_value *since_val = json_get(req->args, "since");
    int64_t since = since_val ? json_get_int(since_val) : 0;

    /* Dispatch each sub-tool.  Each returns a malloc'd body or an
     * error envelope — we treat both identically via embed_or_null. */
    char *kpi   = mcp_router_dispatch("zcl_kpi",        NULL);
    char *peer  = mcp_router_dispatch("zcl_peer_report", NULL);
    char *rpc   = mcp_router_dispatch("zcl_rpc_report",  NULL);

    /* zcl_events requires its args as a JSON value, not a string. */
    struct json_value ev_args = {0};
    const char *ev_args_src = "{\"count\":10}";
    bool have_ev_args = json_read(&ev_args, ev_args_src, strlen(ev_args_src));
    char *events = mcp_router_dispatch("zcl_events",
                                        have_ev_args ? &ev_args : NULL);
    if (have_ev_args) json_free(&ev_args);

    /* Compose the composite envelope. */
    size_t cap = 131072;
    char *out = malloc(cap);
    if (!out) {
        free(kpi); free(peer); free(rpc); free(events);
        return -1;
    }
    size_t pos = 0;

    pos += (size_t)snprintf(out + pos, cap - pos,
        "{\"since\":%lld,\"kpi\":", (long long)since);
    pos += embed_or_null(kpi,   out + pos, cap - pos);
    pos += (size_t)snprintf(out + pos, cap - pos, ",\"peer_report\":");
    pos += embed_or_null(peer,  out + pos, cap - pos);
    pos += (size_t)snprintf(out + pos, cap - pos, ",\"rpc_report\":");
    pos += embed_or_null(rpc,   out + pos, cap - pos);
    pos += (size_t)snprintf(out + pos, cap - pos, ",\"events\":");
    pos += embed_or_null(events, out + pos, cap - pos);

    /* ── Alerts: simple threshold heuristics over the embedded JSON
     * counters.  Anything flagged as a non-zero problem gets a
     * short human-readable string pushed onto the array.  Keep the
     * surface small — operators can still scrape the nested bodies
     * for full detail. */
    pos += (size_t)snprintf(out + pos, cap - pos, ",\"alerts\":[");
    bool first_alert = true;
    #define PUSH_ALERT(fmt, ...)                                           \
        do {                                                                \
            if (!first_alert) out[pos++] = ',';                             \
            first_alert = false;                                            \
            pos += (size_t)snprintf(out + pos, cap - pos,                   \
                                    "\"" fmt "\"", __VA_ARGS__);            \
        } while (0)

    long long rpc_rl_global = scan_int_field(rpc, "rate_limited_global");
    long long rpc_rl_per_ip = scan_int_field(rpc, "rate_limited_per_ip");
    long long rpc_banned    = scan_int_field(rpc, "banned_rejected");
    long long rpc_active    = scan_int_field(rpc, "active_bans");
    long long rpc_auth_fail = scan_int_field(rpc, "auth_failures");
    if (rpc_rl_global > 0)
        PUSH_ALERT("rpc_rate_limited_global=%lld", rpc_rl_global);
    if (rpc_rl_per_ip > 0)
        PUSH_ALERT("rpc_rate_limited_per_ip=%lld", rpc_rl_per_ip);
    if (rpc_banned > 0)
        PUSH_ALERT("rpc_banned_rejected=%lld", rpc_banned);
    if (rpc_active > 0)
        PUSH_ALERT("rpc_active_bans=%lld", rpc_active);
    if (rpc_auth_fail > 0)
        PUSH_ALERT("rpc_auth_failures=%lld", rpc_auth_fail);

    long long peer_bans     = scan_int_field(peer, "bans_total");
    long long peer_offences = scan_int_field(peer, "offences_total");
    if (peer_bans > 0)
        PUSH_ALERT("peer_bans_total=%lld", peer_bans);
    if (peer_offences > 100)
        PUSH_ALERT("peer_offences_total=%lld", peer_offences);

    #undef PUSH_ALERT
    pos += (size_t)snprintf(out + pos, cap - pos, "]}");

    free(kpi); free(peer); free(rpc); free(events);

    if (pos < cap) out[pos] = '\0';
    res->body = out;
    return 0;
}

/* ── Route table ─────────────────────────────────────────────── */

static const struct mcp_param_spec p_logtail[] = {
    { "count",  MCP_PARAM_INT, false, "Number of events to scan",
      1, 10000, 0, 0, NULL, "100" },
    { "domain", MCP_PARAM_STR, false,
      "Event type prefix filter (e.g. \"MCP\", \"VAL.\", \"NET.\")",
      0, 0, 0, 64, NULL, NULL },
};

static const struct mcp_param_spec p_admin[] = {
    /* Accepted for API stability; currently echoed back but the
     * embedded counters are cumulative since boot. */
    { "since", MCP_PARAM_INT, false,
      "Unix-seconds baseline for future windowed counters (unused).",
      0, INT64_MAX, 0, 0, NULL, "0" },
};

static const struct mcp_tool_route k_routes[] = {
    { "zcl_tools_list", "ops",
      "Dump the full MCP routing table: every tool with its domain, "
      "description, and parameter schema. Self-documenting surface.",
      NULL, 0, h_zcl_tools_list },
    { "zcl_self_test", "ops",
      "Call every registered tool with safe defaults, reporting "
      "pass/fail/skip. Destructive tools are skipped.",
      NULL, 0, h_zcl_self_test },
    { "zcl_logtail", "ops",
      "Tail the structured event log. Optional domain prefix filter.",
      p_logtail, sizeof(p_logtail) / sizeof(p_logtail[0]), h_zcl_logtail },
    { "zcl_openapi", "ops",
      "Emit an OpenAPI 3.0-flavored schema document derived from the "
      "MCP routing table. Clients can use it for type generation or "
      "auto-test harnesses.",
      NULL, 0, h_zcl_openapi },
    { "zcl_metrics", "ops",
      "Prometheus-text metrics dump: request counters, latency histogram, "
      "and summary totals accumulated in-process.",
      NULL, 0, h_zcl_metrics },
    { "zcl_metrics_reset", "ops",
      "Reset all MCP metric counters. Destructive — gated by the "
      "middleware rate limiter.",
      NULL, 0, h_zcl_metrics_reset },
    { "zcl_rpc_report", "ops",
      "HTTP RPC middleware report: live rate-limit / ban config plus "
      "allowed/rate-limited/banned/auth-failure counters and current "
      "tracked-IP and active-ban gauges. Parallel to zcl_peer_report "
      "for the RPC surface.",
      NULL, 0, h_zcl_rpc_report },
    { "zcl_admin", "ops",
      "Admin dashboard: aggregates zcl_kpi + zcl_peer_report + "
      "zcl_rpc_report + zcl_events into one snapshot and derives "
      "threshold-based alerts from the nested counters. Missing "
      "subsystems render as null; flagship single-call operator tool.",
      p_admin, sizeof(p_admin) / sizeof(p_admin[0]), h_zcl_admin },
};

void mcp_register_meta(void)
{
    for (size_t i = 0; i < sizeof(k_routes) / sizeof(k_routes[0]); i++)
        mcp_router_register(&k_routes[i]);
}
