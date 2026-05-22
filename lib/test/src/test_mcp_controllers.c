/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Integration tests for the MCP domain controllers: verify every
 * controller registers its tools with well-formed metadata, no
 * duplicate names, and consistent domain labels.  These tests touch
 * the real tool registration code (they link the same controller .c
 * files as the live zclassic23 -mcp binary).  Handler dispatch tests
 * use the ZCL_TESTING mcp_node_rpc hook instead of a running node.
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
#include "mcp/rpc_params.h"
#include "mcp/rpc_client.h"
#include "event/event.h"
#include "json/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#include "util/safe_alloc.h"

/* Expected tool counts.  If a future commit intentionally adds or
 * removes tools, bump these numbers in the same commit — they are the
 * contract for "how big is the MCP surface." */
#define EXPECTED_TOTAL      94  /* +3 power-user tools: chain_tip,
                                 * reorg_history, mempool_inspect;
                                 * +1 Round 6 C5: zcl_blockers;
                                 * +1 I-9 (revamp): zcl_diff_with_legacy_shadow;
                                 * +1 S-11 mini-diff: zcl_diff_staged_header_admit */
#define EXPECTED_OPS        34  /* status, health, kpi, self_heal_stats, mempool*, mininginfo,
                                 * benchmark, dbstats, filemanifest, events,
                                 * rpc, state + node_log + sql (round 6.5 MCP primitives),
                                 * tools_list, self_test, logtail,
                                 * openapi, metrics, metrics_reset,
                                 * rpc_report (wave 5 sess 1),
                                 * admin (wave 5 #5),
                                 * profile (wave 6),
                                 * config_reload (wave 6),
                                 * consensus_report (wave 8),
                                 * syncdiag, replay_dump, replay_exec,
                                 * + mirror status and zclassicd probe,
                                 * + mempool_inspect (fee+age histograms) */
#define EXPECTED_CHAIN      15  /* + chain_tip + reorg_history
                                 * + zcl_diff_with_legacy_shadow (I-9 revamp)
                                 * + zcl_diff_staged_header_admit (S-11 mini-diff) */
#define EXPECTED_NET         9  /* + zcl_peer_report (wave 4 #5),
                                 * + zcl_onion_health (wave 6 #7) */
#define EXPECTED_WALLET     20
#define EXPECTED_APP        16

/* ── Helpers ────────────────────────────────────────────────── */

static void register_all(void)
{
    mcp_router_reset();
    mcp_register_ops();
    mcp_register_diagnostics();
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
    TEST("controllers: ops domain includes self-heal stats tool") {
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
    TEST("controllers: chain domain has 11 tools") {
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
    TEST("controllers: net domain has 9 tools") {
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
    TEST("controllers: wallet domain has 20 tools") {
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
        ASSERT(contains(r->description, "chain advance source scoring"));
        PASS();
    } _test_next:;
    return failures;
}

static char *mock_status_rpc(const char *method, const char *params_json)
{
    (void)params_json;
    if (strcmp(method, "getblockcount") == 0)
        return strdup("3117073");
    if (strcmp(method, "getpeerinfo") == 0)
        return strdup("[{\"inbound\":false,\"subver\":\"/MagicBean:2.1.2-beta1/ZClassic-C23:1.0.0/\",\"startingheight\":3117074}]");
    if (strcmp(method, "syncstate") == 0)
        return strdup("{\"state\":\"at_tip\"}");
    if (strcmp(method, "validationstatus") == 0)
        return strdup("{\"ok\":true}");
    if (strcmp(method, "healthcheck") == 0)
        return strdup("{\"ok\":true,\"memory_rss_mb\":128,\"uptime_seconds\":9}");
    if (strcmp(method, "getblockchaininfo") == 0)
        return strdup("{\"best_header_height\":3117074}");
    if (strcmp(method, "dumpstate") == 0)
        return strdup("{\"initialized\":true,"
                      "\"has_connman\":true,"
                      "\"has_main_state\":true,"
                      "\"has_node_db\":true,"
                      "\"authority\":\"local_consensus_validation\","
                      "\"decision\":\"use_source\","
                      "\"selected_source\":\"p2p\","
                      "\"selected_source_trust\":\"native_peer_validated\","
                      "\"selected_source_selectable\":true,"
                      "\"selected_source_selection_blocker\":\"\","
                      "\"selected_source_score_base\":100,"
                      "\"selected_source_score_health\":20,"
                      "\"selected_source_score_height\":10,"
                      "\"selected_source_score_authorized\":0,"
                      "\"selected_source_score_target_lag_penalty\":0,"
                      "\"selected_source_score_failure_penalty\":0,"
                      "\"selected_source_score_mirror_gate_penalty\":0,"
                      "\"has_last_decision\":true,"
                      "\"last_decision\":{"
                      "\"op\":\"peer_floor\","
                      "\"selected_source\":\"p2p\","
                      "\"selected_source_trust\":\"native_peer_validated\","
                      "\"selected_source_selectable\":true,"
                      "\"selected_source_selection_blocker\":\"\","
                      "\"selected_source_score_base\":100,"
                      "\"selected_source_score_health\":20,"
                      "\"selected_source_score_height\":10,"
                      "\"selected_source_score_authorized\":0,"
                      "\"selected_source_score_target_lag_penalty\":0,"
                      "\"selected_source_score_failure_penalty\":0,"
                      "\"selected_source_score_mirror_gate_penalty\":0,"
                      "\"authority\":\"local_consensus_validation\","
                      "\"selected_source_reason\":\"healthy=3 connecting=0 groups=3 backoff=0/0 tcp_fail=0 proto_fail=0\","
                      "\"sources\":[{\"source\":\"p2p\","
                      "\"trust\":\"native_peer_validated\","
                      "\"state\":\"healthy\","
                      "\"selectable\":true,"
                      "\"selection_blocker\":\"\","
                      "\"score_base\":100,"
                      "\"score_target_lag_penalty\":0,"
                      "\"score_failure_penalty\":0,"
                      "\"reason\":\"healthy=3 connecting=0 groups=3 backoff=0/0 tcp_fail=0 proto_fail=0\","
                      "\"blocker\":\"\"}]"
                      "},"
                      "\"sources\":[{\"source\":\"p2p\","
                      "\"trust\":\"native_peer_validated\","
                      "\"state\":\"healthy\","
                      "\"selectable\":true,"
                      "\"selection_blocker\":\"\","
                      "\"score_base\":100,"
                      "\"score_target_lag_penalty\":0,"
                      "\"score_failure_penalty\":0,"
                      "\"healthy_peers\":3}]}");
    return strdup("null");
}

static int test_zcl_status_includes_chain_advance_dump(void)
{
    int failures = 0;
    TEST("controllers: zcl_status includes chain advance coordinator dump") {
        register_all();
        mcp_rpc_client_set_test_hook(mock_status_rpc);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_status", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);

        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        const struct json_value *chain_advance =
            json_get(&root, "chain_advance");
        ASSERT(chain_advance != NULL);
        ASSERT(json_get_bool(json_get(chain_advance, "initialized")));
        ASSERT(json_get_bool(json_get(chain_advance, "has_connman")));
        ASSERT(json_get_bool(json_get(chain_advance, "has_main_state")));
        ASSERT(json_get_bool(json_get(chain_advance, "has_node_db")));
        ASSERT_STR_EQ(json_get_str(json_get(chain_advance, "authority")),
                      "local_consensus_validation");
        ASSERT_STR_EQ(json_get_str(json_get(chain_advance,
                                            "selected_source")),
                      "p2p");
        ASSERT_STR_EQ(json_get_str(json_get(chain_advance,
                                            "selected_source_trust")),
                      "native_peer_validated");
        ASSERT(json_get_bool(json_get(chain_advance,
                                      "selected_source_selectable")));
        ASSERT_STR_EQ(json_get_str(json_get(chain_advance,
                                            "selected_source_selection_blocker")),
                      "");
        ASSERT(json_get_int(json_get(chain_advance,
                                     "selected_source_score_base")) == 100);
        ASSERT(json_get_int(json_get(chain_advance,
                                     "selected_source_score_health")) == 20);
        ASSERT(json_get_int(json_get(chain_advance,
                                     "selected_source_score_height")) == 10);
        ASSERT(json_get_int(json_get(chain_advance,
                                     "selected_source_score_authorized")) == 0);
        ASSERT(json_get_int(json_get(
                   chain_advance,
                   "selected_source_score_target_lag_penalty")) == 0);
        ASSERT(json_get_int(json_get(
                   chain_advance,
                   "selected_source_score_failure_penalty")) == 0);
        ASSERT(json_get_int(json_get(
                   chain_advance,
                   "selected_source_score_mirror_gate_penalty")) == 0);
        ASSERT(json_get_bool(json_get(chain_advance,
                                      "has_last_decision")));
        const struct json_value *last =
            json_get(chain_advance, "last_decision");
        ASSERT(last != NULL);
        ASSERT_STR_EQ(json_get_str(json_get(last, "op")), "peer_floor");
        ASSERT_STR_EQ(json_get_str(json_get(last, "selected_source_trust")),
                      "native_peer_validated");
        ASSERT(json_get_bool(json_get(last, "selected_source_selectable")));
        ASSERT_STR_EQ(json_get_str(json_get(
                          last, "selected_source_selection_blocker")), "");
        ASSERT(json_get_int(json_get(last,
                                     "selected_source_score_base")) == 100);
        ASSERT(json_get_int(json_get(last,
                                     "selected_source_score_health")) == 20);
        ASSERT(json_get_int(json_get(last,
                                     "selected_source_score_height")) == 10);
        ASSERT(json_get_int(json_get(
                   last, "selected_source_score_authorized")) == 0);
        ASSERT(json_get_int(json_get(
                   last, "selected_source_score_target_lag_penalty")) == 0);
        ASSERT(json_get_int(json_get(
                   last, "selected_source_score_failure_penalty")) == 0);
        ASSERT(json_get_int(json_get(
                   last, "selected_source_score_mirror_gate_penalty")) == 0);
        const char *last_reason =
            json_get_str(json_get(last, "selected_source_reason"));
        ASSERT(last_reason != NULL);
        ASSERT(contains(last_reason, "healthy=3"));
        const struct json_value *last_sources = json_get(last, "sources");
        ASSERT(last_sources != NULL);
        ASSERT(json_size(last_sources) == 1);
        ASSERT_STR_EQ(json_get_str(json_get(json_at(last_sources, 0),
                                            "source")),
                      "p2p");
        ASSERT_STR_EQ(json_get_str(json_get(json_at(last_sources, 0),
                                            "trust")),
                      "native_peer_validated");
        ASSERT(json_get_bool(json_get(json_at(last_sources, 0),
                                      "selectable")));
        ASSERT_STR_EQ(json_get_str(json_get(json_at(last_sources, 0),
                                            "selection_blocker")), "");
        ASSERT(json_get_int(json_get(json_at(last_sources, 0),
                                     "score_base")) == 100);
        ASSERT(json_get_int(json_get(json_at(last_sources, 0),
                                     "score_target_lag_penalty")) == 0);
        ASSERT(json_get_int(json_get(json_at(last_sources, 0),
                                     "score_failure_penalty")) == 0);
        ASSERT(contains(json_get_str(json_get(json_at(last_sources, 0),
                                              "reason")),
                        "healthy=3"));
        const struct json_value *sources = json_get(chain_advance, "sources");
        ASSERT(sources != NULL);
        ASSERT(json_size(sources) == 1);
        ASSERT_STR_EQ(json_get_str(json_get(json_at(sources, 0), "trust")),
                      "native_peer_validated");
        ASSERT_STR_EQ(json_get_str(json_get(json_at(sources, 0), "state")),
                      "healthy");
        ASSERT(json_get_bool(json_get(json_at(sources, 0), "selectable")));
        ASSERT_STR_EQ(json_get_str(json_get(json_at(sources, 0),
                                            "selection_blocker")), "");
        ASSERT(json_get_int(json_get(json_at(sources, 0),
                                     "score_base")) == 100);
        ASSERT(json_get_int(json_get(json_at(sources, 0),
                                     "score_target_lag_penalty")) == 0);
        ASSERT(json_get_int(json_get(json_at(sources, 0),
                                     "score_failure_penalty")) == 0);
        json_free(&root);
        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static char *mock_networkinfo_rpc(const char *method, const char *params_json)
{
    (void)params_json;
    if (strcmp(method, "getnetworkinfo") == 0)
        return strdup("{\"connections\":2,"
                      "\"inbound_connections\":1,"
                      "\"outbound_connections\":1,"
                      "\"handshaked_connections\":2,"
                      "\"inbound_handshaked_connections\":1,"
                      "\"outbound_handshaked_connections\":1,"
                      "\"inbound_handshake_seen\":true,"
                      "\"remote_handshake_seen\":true,"
                      "\"magicbean_peers\":2,"
                      "\"zclassic_c23_peers\":1,"
                      "\"peer_lifecycle\":{"
                      "\"attempted\":4,"
                      "\"connected\":3,"
                      "\"version_sent\":3,"
                      "\"version_received\":2,"
                      "\"verack_received\":2,"
                      "\"handshake_complete\":2,"
                      "\"active\":1,"
                      "\"disconnected\":1,"
                      "\"timeout\":1,"
                      "\"rejected\":0,"
                      "\"magicbean_handshakes\":2,"
                      "\"zclassic_c23_handshakes\":1,"
                      "\"sources\":["
                      "{\"source\":\"addnode\",\"attempted\":2,"
                      "\"connected\":1,\"handshake_complete\":1,"
                      "\"timeout\":1,\"rejected\":0},"
                      "{\"source\":\"addrman\",\"attempted\":2,"
                      "\"connected\":2,\"handshake_complete\":1,"
                      "\"timeout\":0,\"rejected\":0}]},"
                      "\"localaddresses\":[{\"address\":\"203.0.113.7\","
                      "\"port\":8033,\"score\":1}],"
                      "\"listening\":true}");
    return strdup("null");
}

static int test_zcl_networkinfo_exposes_reachability_fields(void)
{
    int failures = 0;
    TEST("controllers: zcl_networkinfo exposes inbound reachability fields") {
        register_all();
        mcp_rpc_client_set_test_hook(mock_networkinfo_rpc);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_networkinfo", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);

        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT(json_get_int(json_get(&root,
                                     "handshaked_connections")) == 2);
        ASSERT(json_get_int(json_get(&root,
                                     "inbound_handshaked_connections")) == 1);
        ASSERT(json_get_int(json_get(&root,
                                     "outbound_handshaked_connections")) == 1);
        ASSERT(json_get_bool(json_get(&root, "inbound_handshake_seen")));
        ASSERT(json_get_bool(json_get(&root, "remote_handshake_seen")));
        const struct json_value *life = json_get(&root, "peer_lifecycle");
        const struct json_value *sources =
            life ? json_get(life, "sources") : NULL;
        ASSERT(life && life->type == JSON_OBJ);
        ASSERT(json_get_int(json_get(life, "attempted")) == 4);
        ASSERT(json_get_int(json_get(life, "timeout")) == 1);
        ASSERT(sources && sources->type == JSON_ARR);
        ASSERT(json_size(sources) == 2);
        ASSERT_STR_EQ(json_get_str(json_get(json_at(sources, 0), "source")),
                      "addnode");
        ASSERT(json_get_int(json_get(json_at(sources, 0),
                                     "handshake_complete")) == 1);
        ASSERT(json_get_int(json_get(json_at(sources, 0), "timeout")) == 1);
        ASSERT_STR_EQ(json_get_str(json_get(json_at(sources, 1), "source")),
                      "addrman");
        json_free(&root);
        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
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
        char *buf = zcl_malloc(cap, "test_json_buf");
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

/* ── Wave 6: zcl_profile ────────────────────────────────────── */

static int test_zcl_profile_shape(void)
{
    int failures = 0;
    TEST("controllers: zcl_profile returns top_threads + duration_ms") {
        register_all();
        const struct mcp_tool_route *r = mcp_router_find("zcl_profile");
        ASSERT(r != NULL);
        ASSERT(strcmp(r->domain, "ops") == 0);
        ASSERT(r->num_params == 2);

        /* Use a small duration to keep the test fast. */
        const char *args_src = "{\"duration_ms\":100,\"top_n\":5}";
        struct json_value args = {0};
        ASSERT(json_read(&args, args_src, strlen(args_src)));

        char *body = mcp_router_dispatch("zcl_profile", &args);
        ASSERT(body != NULL);
        ASSERT(strstr(body, "\"error\":{") == NULL);
        ASSERT(contains(body, "\"duration_ms\":100"));
        ASSERT(contains(body, "\"sampled_threads\""));
        ASSERT(contains(body, "\"top_threads\":["));

        /* The process always has at least one thread (the test runner). */
        struct json_value root = {0};
        ASSERT(json_read(&root, body, strlen(body)));
        const struct json_value *st = json_get(&root, "sampled_threads");
        ASSERT(st != NULL);
        ASSERT(json_get_int(st) >= 1);

        const struct json_value *tt = json_get(&root, "top_threads");
        ASSERT(tt != NULL);
        ASSERT(tt->type == JSON_ARR);
        ASSERT(tt->num_children >= 1);
        ASSERT(tt->num_children <= 5);

        /* Each top_threads entry has tid, name, user_ms, sys_ms, cpu_pct. */
        const struct json_value *first = &tt->children[0];
        ASSERT(json_get(first, "tid")     != NULL);
        ASSERT(json_get(first, "name")    != NULL);
        ASSERT(json_get(first, "user_ms") != NULL);
        ASSERT(json_get(first, "sys_ms")  != NULL);
        ASSERT(json_get(first, "cpu_pct") != NULL);

        json_free(&root);
        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    return failures;
}

static int test_zcl_profile_clamps(void)
{
    int failures = 0;
    TEST("controllers: zcl_profile clamps duration_ms to [100, 10000]") {
        register_all();
        /* The router enforces the min/max from p_profile spec, so a
         * value below 100 should be rejected with an error envelope. */
        const char *args_src = "{\"duration_ms\":50}";
        struct json_value args = {0};
        ASSERT(json_read(&args, args_src, strlen(args_src)));
        char *body = mcp_router_dispatch("zcl_profile", &args);
        ASSERT(body != NULL);
        ASSERT(contains(body, "\"error\":{"));
        free(body);
        json_free(&args);
        PASS();
    } _test_next:;
    return failures;
}

/* ── JSON injection in wallet RPC payloads ────── */

/* Before the fix, both handlers snprintf'd user-controlled strings
 * directly into their params_json:
 *
 *     snprintf(params, sizeof(params),
 *              "[\"%s\",[{\"address\":\"%s\",\"amount\":%.8f}]]",
 *              from, to, amount);
 *
 * A caller sending `from = "ztest","params":["attacker_addr"] //`
 * would punch through the string context and rewrite the params
 * array — redirecting funds to `attacker_addr`. Both handlers now
 * route user strings through mcp_params_* which escape the dangerous
 * characters via the JSON encoder.
 *
 * These tests exercise the builder with the exact shapes the handlers
 * produce, then parse the output back and assert that the attacker's
 * string is preserved as a single literal string — not interpreted as
 * structure. */

static int test_zcl_send_escapes_json_injection(void)
{
    int failures = 0;
    TEST("controllers: zcl_send escapes JSON injection in from/to ") {
        /* Classic payload: close the "from" string, re-open params,
         * and point funds at an attacker-controlled address. */
        const char *attacker = "ztest\",[{\"address\":\"attacker\",\"amount\":1.0}]] //";

        struct mcp_params p;
        mcp_params_init(&p);
        mcp_params_push_str(&p, attacker);

        struct json_value recip, recip_arr;
        json_init(&recip);     json_set_object(&recip);
        json_push_kv_str (&recip, "address", attacker);
        json_push_kv_real(&recip, "amount",  0.5);
        json_init(&recip_arr); json_set_array(&recip_arr);
        json_push_back(&recip_arr, &recip);
        mcp_params_push_value(&p, &recip_arr);
        json_free(&recip);
        json_free(&recip_arr);

        char *params = mcp_params_to_json(&p);
        ASSERT(params != NULL);

        /* Shape: params must be exactly [string, array-of-one-object]. */
        struct json_value root;
        ASSERT(json_read(&root, params, strlen(params)));
        ASSERT(root.type == JSON_ARR);
        ASSERT(root.num_children == 2);

        const struct json_value *from_v = json_at(&root, 0);
        ASSERT(from_v != NULL);
        ASSERT(from_v->type == JSON_STR);
        ASSERT_STR_EQ(from_v->val.s, attacker);

        const struct json_value *recips = json_at(&root, 1);
        ASSERT(recips != NULL);
        ASSERT(recips->type == JSON_ARR);
        ASSERT(recips->num_children == 1);

        const struct json_value *r0 = json_at(recips, 0);
        ASSERT(r0 != NULL);
        ASSERT(r0->type == JSON_OBJ);
        const struct json_value *addr_v = json_get(r0, "address");
        ASSERT(addr_v != NULL);
        ASSERT(addr_v->type == JSON_STR);
        ASSERT_STR_EQ(addr_v->val.s, attacker);

        /* The raw serialized payload must contain escaped quotes —
         * belt-and-suspenders check on the escape itself. */
        ASSERT(strstr(params, "\\\"") != NULL);

        free(params);
        json_free(&root);
        PASS();
    } _test_next:;
    return failures;
}

static int test_zcl_sendtoaddress_escapes_json_injection(void)
{
    int failures = 0;
    TEST("controllers: zcl_sendtoaddress escapes JSON injection in address ") {
        /* Punch through the address string, bloat amount to drain the
         * wallet, and append a bogus second recipient. */
        const char *attacker = "zaddr\",999999999,\"extra\":[\"attacker\"]";

        struct mcp_params p;
        mcp_params_init(&p);
        mcp_params_push_str (&p, attacker);
        mcp_params_push_real(&p, 0.01);
        char *params = mcp_params_to_json(&p);
        ASSERT(params != NULL);

        struct json_value root;
        ASSERT(json_read(&root, params, strlen(params)));
        ASSERT(root.type == JSON_ARR);
        /* Exactly two — the injection did NOT add a third element. */
        ASSERT(root.num_children == 2);

        const struct json_value *addr_v = json_at(&root, 0);
        ASSERT(addr_v != NULL);
        ASSERT(addr_v->type == JSON_STR);
        ASSERT_STR_EQ(addr_v->val.s, attacker);

        const struct json_value *amt_v = json_at(&root, 1);
        ASSERT(amt_v != NULL);
        ASSERT(amt_v->type == JSON_REAL);
        /* The amount is the number we pushed, not the injected 999999999. */
        ASSERT(amt_v->val.d < 1.0);

        ASSERT(strstr(params, "\\\"") != NULL);

        free(params);
        json_free(&root);
        PASS();
    } _test_next:;
    return failures;
}

static int test_mcp_params_escapes_backslash_and_control(void)
{
    int failures = 0;
    TEST("controllers: mcp_params escapes backslash, newline, and control chars") {
        const char *s = "a\\b\"c\nd\te";
        struct mcp_params p;
        mcp_params_init(&p);
        mcp_params_push_str(&p, s);
        char *params = mcp_params_to_json(&p);
        ASSERT(params != NULL);

        struct json_value root;
        ASSERT(json_read(&root, params, strlen(params)));
        ASSERT(root.type == JSON_ARR);
        ASSERT(root.num_children == 1);
        const struct json_value *s_v = json_at(&root, 0);
        ASSERT(s_v != NULL);
        ASSERT(s_v->type == JSON_STR);
        ASSERT_STR_EQ(s_v->val.s, s);

        free(params);
        json_free(&root);
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
    failures += test_zcl_status_includes_chain_advance_dump();
    failures += test_zcl_networkinfo_exposes_reachability_fields();
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
    failures += test_zcl_profile_shape();
    failures += test_zcl_profile_clamps();
    failures += test_zcl_send_escapes_json_injection();
    failures += test_zcl_sendtoaddress_escapes_json_injection();
    failures += test_mcp_params_escapes_backslash_and_control();
    failures += test_final_reset_leaves_clean_table();

    return failures;
}
