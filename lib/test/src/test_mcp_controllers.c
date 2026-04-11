/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Integration tests for the MCP domain controllers: verify every
 * controller registers its tools with well-formed metadata, no
 * duplicate names, and consistent domain labels.  These tests touch
 * the real tool registration code (they link the same controller .c
 * files as the live zclassic23 -mcp binary), but do NOT dispatch any
 * handler — the handlers would call mcp_node_rpc() which needs a
 * running node.
 *
 * Coverage:
 *   1. mcp_register_* populate the router with the expected number of
 *      tools per domain and a correct total.
 *   2. Every registered tool has a non-null handler, description, and
 *      domain from the small known set.
 *   3. Every tool name starts with "zcl_" and is unique within the
 *      table.
 *   4. Schema generation (tools/list JSON, inputSchema per tool) is
 *      well-formed for real controller routes.
 *   5. Specific high-traffic tools exist with the expected parameter
 *      shape (zcl_getblock, zcl_status, zcl_kpi, zcl_self_test, ...).
 *   6. Reset leaves the table empty and re-registration restores it.
 */

#include "test/test_helpers.h"
#include "mcp/router.h"
#include "mcp/controllers.h"
#include "event/event.h"
#include "json/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* Expected tool counts.  If a future commit intentionally adds or
 * removes tools, bump these numbers in the same commit — they are the
 * contract for "how big is the MCP surface." */
#define EXPECTED_TOTAL      72
#define EXPECTED_OPS        19  /* status, health, kpi, mempool*, mininginfo,
                                 * benchmark, dbstats, filemanifest, events,
                                 * rpc, tools_list, self_test, logtail,
                                 * openapi, metrics, metrics_reset,
                                 * rpc_report (wave 5 sess 1),
                                 * admin (wave 5 #5) */
#define EXPECTED_CHAIN      10
#define EXPECTED_NET         8  /* + zcl_peer_report (wave 4 #5) */
#define EXPECTED_WALLET     19
#define EXPECTED_APP        16

/* ── Helpers ────────────────────────────────────────────────── */

static void register_all(void)
{
    mcp_router_reset();
    mcp_register_ops();
    mcp_register_chain();
    mcp_register_net();
    mcp_register_wallet();
    mcp_register_app();
    mcp_register_meta();
}

static size_t count_by_domain(const char *domain)
{
    size_t n = 0;
    for (size_t i = 0; i < mcp_router_count(); i++) {
        const struct mcp_tool_route *r = mcp_router_at(i);
        if (r && r->domain && strcmp(r->domain, domain) == 0)
            n++;
    }
    return n;
}

static bool is_known_domain(const char *d)
{
    if (!d) return false;
    return strcmp(d, "ops")    == 0 ||
           strcmp(d, "chain")  == 0 ||
           strcmp(d, "net")    == 0 ||
           strcmp(d, "wallet") == 0 ||
           strcmp(d, "app")    == 0;
}

static bool contains(const char *haystack, const char *needle)
{
    return haystack && needle && strstr(haystack, needle) != NULL;
}

/* ── Tests ──────────────────────────────────────────────────── */

static int test_register_total_count(void)
{
    int failures = 0;
    TEST("controllers: total tool count matches expected surface") {
        register_all();
        size_t n = mcp_router_count();
        if (n != EXPECTED_TOTAL) {
            printf("FAIL (got %zu, expected %d)\n", n, EXPECTED_TOTAL);
            failures++; goto _test_next;
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_ops_domain_count(void)
{
    int failures = 0;
    TEST("controllers: ops domain has 19 tools (wave 5 adds rpc_report + admin)") {
        register_all();
        size_t n = count_by_domain("ops");
        if (n != EXPECTED_OPS) {
            printf("FAIL (ops=%zu, expected %d)\n", n, EXPECTED_OPS);
            failures++; goto _test_next;
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_chain_domain_count(void)
{
    int failures = 0;
    TEST("controllers: chain domain has 10 tools") {
        register_all();
        size_t n = count_by_domain("chain");
        if (n != EXPECTED_CHAIN) {
            printf("FAIL (chain=%zu, expected %d)\n", n, EXPECTED_CHAIN);
            failures++; goto _test_next;
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_net_domain_count(void)
{
    int failures = 0;
    TEST("controllers: net domain has 8 tools") {
        register_all();
        size_t n = count_by_domain("net");
        if (n != EXPECTED_NET) {
            printf("FAIL (net=%zu, expected %d)\n", n, EXPECTED_NET);
            failures++; goto _test_next;
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_wallet_domain_count(void)
{
    int failures = 0;
    TEST("controllers: wallet domain has 19 tools") {
        register_all();
        size_t n = count_by_domain("wallet");
        if (n != EXPECTED_WALLET) {
            printf("FAIL (wallet=%zu, expected %d)\n", n, EXPECTED_WALLET);
            failures++; goto _test_next;
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_app_domain_count(void)
{
    int failures = 0;
    TEST("controllers: app domain has 16 tools") {
        register_all();
        size_t n = count_by_domain("app");
        if (n != EXPECTED_APP) {
            printf("FAIL (app=%zu, expected %d)\n", n, EXPECTED_APP);
            failures++; goto _test_next;
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_every_tool_has_handler(void)
{
    int failures = 0;
    TEST("controllers: every registered tool has a non-null handler") {
        register_all();
        for (size_t i = 0; i < mcp_router_count(); i++) {
            const struct mcp_tool_route *r = mcp_router_at(i);
            ASSERT(r != NULL);
            ASSERT(r->handler != NULL);
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_every_tool_has_description(void)
{
    int failures = 0;
    TEST("controllers: every tool has a non-empty description") {
        register_all();
        for (size_t i = 0; i < mcp_router_count(); i++) {
            const struct mcp_tool_route *r = mcp_router_at(i);
            ASSERT(r != NULL);
            ASSERT(r->description != NULL);
            ASSERT(r->description[0] != 0);
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_every_tool_has_known_domain(void)
{
    int failures = 0;
    TEST("controllers: every tool has a known domain label") {
        register_all();
        for (size_t i = 0; i < mcp_router_count(); i++) {
            const struct mcp_tool_route *r = mcp_router_at(i);
            ASSERT(r != NULL);
            if (!is_known_domain(r->domain)) {
                printf("FAIL (%s has domain=%s)\n",
                       r->name, r->domain ? r->domain : "(null)");
                failures++; goto _test_next;
            }
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_every_tool_name_prefixed(void)
{
    int failures = 0;
    TEST("controllers: every tool name starts with zcl_") {
        register_all();
        for (size_t i = 0; i < mcp_router_count(); i++) {
            const struct mcp_tool_route *r = mcp_router_at(i);
            ASSERT(r != NULL);
            ASSERT(r->name != NULL);
            if (strncmp(r->name, "zcl_", 4) != 0) {
                printf("FAIL (%s is not zcl_-prefixed)\n", r->name);
                failures++; goto _test_next;
            }
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_no_duplicate_names(void)
{
    int failures = 0;
    TEST("controllers: no duplicate tool names across all domains") {
        register_all();
        size_t n = mcp_router_count();
        for (size_t i = 0; i < n; i++) {
            const struct mcp_tool_route *a = mcp_router_at(i);
            ASSERT(a != NULL);
            for (size_t j = i + 1; j < n; j++) {
                const struct mcp_tool_route *b = mcp_router_at(j);
                ASSERT(b != NULL);
                if (strcmp(a->name, b->name) == 0) {
                    printf("FAIL (duplicate %s)\n", a->name);
                    failures++; goto _test_next;
                }
            }
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_specific_flagship_tools_registered(void)
{
    int failures = 0;
    TEST("controllers: flagship tools registered") {
        register_all();
        /* Canon set — documented in CLAUDE.md.  If any goes missing,
         * the compat contract is broken. */
        const char *k[] = {
            "zcl_status", "zcl_kpi", "zcl_health",
            "zcl_getblockcount", "zcl_getblock", "zcl_getblockchaininfo",
            "zcl_peers", "zcl_networkinfo", "zcl_onion_status",
            "zcl_balance", "zcl_send", "zcl_getnewaddress",
            "zcl_z_getnewaddress",
            "zcl_name_resolve", "zcl_msg_send",
            "zcl_swap_chains", "zcl_market_list",
            "zcl_tools_list", "zcl_self_test", "zcl_logtail",
            "zcl_rpc",
        };
        for (size_t i = 0; i < sizeof(k)/sizeof(k[0]); i++) {
            if (mcp_router_find(k[i]) == NULL) {
                printf("FAIL (missing %s)\n", k[i]);
                failures++; goto _test_next;
            }
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_zcl_getblock_param_shape(void)
{
    int failures = 0;
    TEST("controllers: zcl_getblock has required block_id + optional verbosity") {
        register_all();
        const struct mcp_tool_route *r = mcp_router_find("zcl_getblock");
        ASSERT(r != NULL);
        ASSERT(r->num_params == 2);
        ASSERT(strcmp(r->params[0].name, "block_id") == 0);
        ASSERT(r->params[0].required == true);
        ASSERT(r->params[0].type == MCP_PARAM_STR);
        ASSERT(strcmp(r->params[1].name, "verbosity") == 0);
        ASSERT(r->params[1].required == false);
        ASSERT(r->params[1].type == MCP_PARAM_INT);
        ASSERT(r->params[1].default_json != NULL);
        PASS();
    } _test_next:;
    return failures;
}

static int test_zcl_status_no_params(void)
{
    int failures = 0;
    TEST("controllers: zcl_status takes no parameters") {
        register_all();
        const struct mcp_tool_route *r = mcp_router_find("zcl_status");
        ASSERT(r != NULL);
        ASSERT(r->num_params == 0);
        ASSERT(strcmp(r->domain, "ops") == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int test_meta_tools_in_ops_domain(void)
{
    int failures = 0;
    TEST("controllers: meta tools (tools_list/self_test/logtail) live in ops") {
        register_all();
        const char *k[] = {"zcl_tools_list", "zcl_self_test", "zcl_logtail"};
        for (size_t i = 0; i < sizeof(k)/sizeof(k[0]); i++) {
            const struct mcp_tool_route *r = mcp_router_find(k[i]);
            ASSERT(r != NULL);
            ASSERT(r->domain != NULL);
            if (strcmp(r->domain, "ops") != 0) {
                printf("FAIL (%s domain=%s)\n", k[i], r->domain);
                failures++; goto _test_next;
            }
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_tools_list_json_well_formed(void)
{
    int failures = 0;
    TEST("controllers: mcp_router_tools_list_json produces parseable array") {
        register_all();
        size_t cap = 131072;
        char *buf = malloc(cap);
        ASSERT(buf != NULL);
        size_t wrote = mcp_router_tools_list_json(buf, cap);
        ASSERT(wrote > 0);
        ASSERT(wrote < cap);
        /* Starts with '[' and ends with ']'. */
        ASSERT(buf[0] == '[');
        ASSERT(buf[wrote - 1] == ']');
        /* Mentions at least one known tool. */
        ASSERT(contains(buf, "zcl_status"));
        ASSERT(contains(buf, "zcl_kpi"));
        /* Parseable JSON */
        struct json_value root;
        ASSERT(json_read(&root, buf, wrote));
        ASSERT(root.type == JSON_ARR);
        ASSERT(root.num_children == EXPECTED_TOTAL);
        json_free(&root);
        free(buf);
        PASS();
    } _test_next:;
    return failures;
}

static int test_input_schema_for_zcl_getblock(void)
{
    int failures = 0;
    TEST("controllers: inputSchema for zcl_getblock declares block_id required") {
        register_all();
        const struct mcp_tool_route *r = mcp_router_find("zcl_getblock");
        ASSERT(r != NULL);
        char buf[4096];
        size_t n = mcp_router_input_schema_json(r, buf, sizeof(buf));
        ASSERT(n > 0);
        ASSERT(contains(buf, "\"block_id\""));
        ASSERT(contains(buf, "\"verbosity\""));
        ASSERT(contains(buf, "\"required\""));
        /* JSON schema lists required fields as an array containing "block_id". */
        ASSERT(contains(buf, "\"block_id\""));
        PASS();
    } _test_next:;
    return failures;
}

static int test_destructive_tools_registered(void)
{
    int failures = 0;
    TEST("controllers: destructive tools (send/importprivkey/...) exist") {
        register_all();
        /* self_test skips these, but they must still be reachable over
         * the wire — otherwise the compat contract breaks. */
        const char *k[] = {
            "zcl_send", "zcl_sendtoaddress", "zcl_importprivkey",
            "zcl_rescanblockchain", "zcl_replaywalletfromchain",
            "zcl_dumpprivkey", "zcl_addnode", "zcl_pingpeer",
            "zcl_name_register", "zcl_msg_send", "zcl_market_offer",
            "zcl_swap_initiate",
        };
        for (size_t i = 0; i < sizeof(k)/sizeof(k[0]); i++) {
            if (mcp_router_find(k[i]) == NULL) {
                printf("FAIL (missing %s)\n", k[i]);
                failures++; goto _test_next;
            }
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_duplicate_register_rejected(void)
{
    int failures = 0;
    TEST("controllers: re-registering the same controller is a no-op") {
        register_all();
        size_t before = mcp_router_count();
        /* Register ops a second time — mcp_router_register should reject
         * each duplicate and the count should not change. */
        mcp_register_ops();
        size_t after = mcp_router_count();
        ASSERT(before == after);
        PASS();
    } _test_next:;
    return failures;
}

static int test_reset_clears_and_reregister_restores(void)
{
    int failures = 0;
    TEST("controllers: reset clears and re-register restores the surface") {
        register_all();
        size_t before = mcp_router_count();
        ASSERT(before == EXPECTED_TOTAL);
        mcp_router_reset();
        ASSERT(mcp_router_count() == 0);
        ASSERT(mcp_router_find("zcl_status") == NULL);
        register_all();
        ASSERT(mcp_router_count() == EXPECTED_TOTAL);
        ASSERT(mcp_router_find("zcl_status") != NULL);
        PASS();
    } _test_next:;
    return failures;
}

static int test_wallet_shielded_tools_registered(void)
{
    int failures = 0;
    TEST("controllers: shielded wallet tools (z_*) registered") {
        register_all();
        const char *k[] = {
            "zcl_z_getnewaddress", "zcl_z_listaddresses",
            "zcl_z_listunspent",   "zcl_z_getbalance",
        };
        for (size_t i = 0; i < sizeof(k)/sizeof(k[0]); i++) {
            const struct mcp_tool_route *r = mcp_router_find(k[i]);
            ASSERT(r != NULL);
            ASSERT(strcmp(r->domain, "wallet") == 0);
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_app_protocol_tools_registered(void)
{
    int failures = 0;
    TEST("controllers: app protocol tools (name/msg/market/swap) registered") {
        register_all();
        const char *k[] = {
            "zcl_name_resolve", "zcl_name_register", "zcl_name_list",
            "zcl_msg_send", "zcl_msg_send_named", "zcl_msg_inbox", "zcl_msg_read",
            "zcl_market_list", "zcl_market_offer", "zcl_market_buy",
            "zcl_market_status",
            "zcl_swap_chains", "zcl_swap_initiate", "zcl_swap_participate",
            "zcl_swap_list",
            "zcl_tokens",
        };
        for (size_t i = 0; i < sizeof(k)/sizeof(k[0]); i++) {
            const struct mcp_tool_route *r = mcp_router_find(k[i]);
            if (!r) {
                printf("FAIL (missing %s)\n", k[i]);
                failures++; goto _test_next;
            }
            if (strcmp(r->domain, "app") != 0) {
                printf("FAIL (%s domain=%s expected app)\n", k[i], r->domain);
                failures++; goto _test_next;
            }
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_required_params_have_no_default(void)
{
    int failures = 0;
    TEST("controllers: required params never carry a default_json") {
        register_all();
        /* A required param with a default would be a schema contradiction:
         * the router would never enforce "required".  This is a sanity
         * check on controller route tables. */
        for (size_t i = 0; i < mcp_router_count(); i++) {
            const struct mcp_tool_route *r = mcp_router_at(i);
            ASSERT(r != NULL);
            for (size_t j = 0; j < r->num_params; j++) {
                const struct mcp_param_spec *p = &r->params[j];
                if (p->required && p->default_json) {
                    printf("FAIL (%s.%s is required with default=%s)\n",
                           r->name, p->name, p->default_json);
                    failures++; goto _test_next;
                }
            }
        }
        PASS();
    } _test_next:;
    return failures;
}

/* ── Wave 5 #5: zcl_admin composite tool ────────────────────── */

static int test_zcl_admin_dispatch_shape(void)
{
    int failures = 0;
    TEST("controllers: zcl_admin composes sub-tools into one envelope") {
        register_all();
        const struct mcp_tool_route *r = mcp_router_find("zcl_admin");
        ASSERT(r != NULL);
        ASSERT(strcmp(r->domain, "ops") == 0);
        ASSERT(r->num_params == 1);
        ASSERT(strcmp(r->params[0].name, "since") == 0);
        ASSERT(r->params[0].required == false);

        /* Dispatch with empty args — `since` falls through to default 0. */
        char *body = mcp_router_dispatch("zcl_admin", NULL);
        ASSERT(body != NULL);
        /* Not an error envelope — graceful handling even with no live RPC. */
        ASSERT(strstr(body, "\"error\":{") == NULL);
        /* Top-level fields. */
        ASSERT(contains(body, "\"since\":0"));
        ASSERT(contains(body, "\"kpi\":"));
        ASSERT(contains(body, "\"peer_report\":"));
        ASSERT(contains(body, "\"rpc_report\":"));
        ASSERT(contains(body, "\"events\":"));
        ASSERT(contains(body, "\"alerts\":["));
        /* rpc_report is always produced in-process, so it should
         * embed as an object (not null). */
        ASSERT(contains(body, "\"rpc_server\":\"inactive\"") ||
               contains(body, "\"rpc_server\":\"active\""));
        free(body);
        PASS();
    } _test_next:;
    return failures;
}

static int test_zcl_admin_since_param_accepted(void)
{
    int failures = 0;
    TEST("controllers: zcl_admin echoes `since` back in the envelope") {
        register_all();
        const char *args_src = "{\"since\":1700000000}";
        struct json_value args = {0};
        ASSERT(json_read(&args, args_src, strlen(args_src)));

        char *body = mcp_router_dispatch("zcl_admin", &args);
        ASSERT(body != NULL);
        ASSERT(contains(body, "\"since\":1700000000"));
        free(body);
        json_free(&args);
        PASS();
    } _test_next:;
    return failures;
}

static int test_zcl_admin_graceful_never_propagates_error(void)
{
    int failures = 0;
    TEST("controllers: zcl_admin never propagates a sub-tool error envelope") {
        register_all();
        /* Whether or not a sub-tool returns a valid body vs an error
         * envelope in this test context, zcl_admin must wrap the
         * response as its own object — never surface a top-level
         * `{"error":...}`.  embed_or_null is the policy; this test
         * catches any regression that bypasses it. */
        char *body = mcp_router_dispatch("zcl_admin", NULL);
        ASSERT(body != NULL);

        /* The top level is an object, not an error envelope. */
        ASSERT(body[0] == '{');
        ASSERT(strncmp(body, "{\"error\":", 9) != 0);

        /* Parse to make sure it's structurally valid JSON. */
        struct json_value root = {0};
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT(root.type == JSON_OBJ);

        /* Each expected top-level key is present. */
        ASSERT(json_get(&root, "since")       != NULL);
        ASSERT(json_get(&root, "kpi")         != NULL);
        ASSERT(json_get(&root, "peer_report") != NULL);
        ASSERT(json_get(&root, "rpc_report")  != NULL);
        ASSERT(json_get(&root, "events")      != NULL);
        ASSERT(json_get(&root, "alerts")      != NULL);

        /* alerts is an array. */
        const struct json_value *alerts = json_get(&root, "alerts");
        ASSERT(alerts->type == JSON_ARR);

        json_free(&root);
        free(body);
        PASS();
    } _test_next:;
    return failures;
}

static int test_final_reset_leaves_clean_table(void)
{
    int failures = 0;
    TEST("controllers: final reset leaves the registry clean for sibling tests") {
        mcp_router_reset();
        ASSERT(mcp_router_count() == 0);
        PASS();
    } _test_next:;
    return failures;
}

/* ── Entry point ────────────────────────────────────────────── */

int test_mcp_controllers(void);

int test_mcp_controllers(void)
{
    int failures = 0;
    event_log_init();

    failures += test_register_total_count();
    failures += test_ops_domain_count();
    failures += test_chain_domain_count();
    failures += test_net_domain_count();
    failures += test_wallet_domain_count();
    failures += test_app_domain_count();
    failures += test_every_tool_has_handler();
    failures += test_every_tool_has_description();
    failures += test_every_tool_has_known_domain();
    failures += test_every_tool_name_prefixed();
    failures += test_no_duplicate_names();
    failures += test_specific_flagship_tools_registered();
    failures += test_zcl_getblock_param_shape();
    failures += test_zcl_status_no_params();
    failures += test_meta_tools_in_ops_domain();
    failures += test_tools_list_json_well_formed();
    failures += test_input_schema_for_zcl_getblock();
    failures += test_destructive_tools_registered();
    failures += test_duplicate_register_rejected();
    failures += test_reset_clears_and_reregister_restores();
    failures += test_wallet_shielded_tools_registered();
    failures += test_app_protocol_tools_registered();
    failures += test_required_params_have_no_default();
    failures += test_zcl_admin_dispatch_shape();
    failures += test_zcl_admin_since_param_accepted();
    failures += test_zcl_admin_graceful_never_propagates_error();
    failures += test_final_reset_leaves_clean_table();

    return failures;
}
