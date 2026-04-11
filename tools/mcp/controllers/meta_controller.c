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

#include "json/json.h"

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

/* ── Route table ─────────────────────────────────────────────── */

static const struct mcp_param_spec p_logtail[] = {
    { "count",  MCP_PARAM_INT, false, "Number of events to scan",
      1, 10000, 0, 0, NULL, "100" },
    { "domain", MCP_PARAM_STR, false,
      "Event type prefix filter (e.g. \"MCP\", \"VAL.\", \"NET.\")",
      0, 0, 0, 64, NULL, NULL },
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
};

void mcp_register_meta(void)
{
    for (size_t i = 0; i < sizeof(k_routes) / sizeof(k_routes[0]); i++)
        mcp_router_register(&k_routes[i]);
}
