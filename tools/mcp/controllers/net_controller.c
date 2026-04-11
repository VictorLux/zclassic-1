/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * MCP net controller: peers, network info, peer discovery, ping, games. */

#include "../controllers.h"
#include "../router.h"
#include "../rpc_client.h"

#include "json/json.h"
#include "mcp/metrics.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFINE_PT(name, rpc)                                                   \
    static int name(const struct mcp_request *req, struct mcp_response *res)  \
    {                                                                          \
        (void)req;                                                             \
        char *out = mcp_node_rpc(rpc, NULL);                                   \
        if (!out) return -1;                                                   \
        res->body = out;                                                       \
        return 0;                                                              \
    }

DEFINE_PT(h_zcl_peers,        "getpeerinfo")
DEFINE_PT(h_zcl_networkinfo,  "getnetworkinfo")
DEFINE_PT(h_zcl_onion_status, "healthcheck")
DEFINE_PT(h_zcl_gametypes,    "gametypes")
DEFINE_PT(h_zcl_peerlatency,  "getpeerlatency")

static int h_zcl_addnode(const struct mcp_request *req, struct mcp_response *res)
{
    const char *addr = json_get_str(json_get(req->args, "addr"));
    const struct json_value *act = json_get(req->args, "action");
    char params[256];
    snprintf(params, sizeof(params), "[\"%s\",\"%s\"]",
             addr, act ? json_get_str(act) : "onetry");
    char *out = mcp_node_rpc("addnode", params);
    if (!out) return -1;
    res->body = out;
    return 0;
}

static int h_zcl_pingpeer(const struct mcp_request *req, struct mcp_response *res)
{
    int64_t peer_id = json_get_int(json_get(req->args, "peer_id"));
    char params[64];
    snprintf(params, sizeof(params), "[%lld]", (long long)peer_id);
    char *out = mcp_node_rpc("pingpeer", params);
    if (!out) return -1;
    res->body = out;
    return 0;
}

/* zcl_peer_report — peer scoring summary derived from in-process
 * metrics counters that subscribe to EV_PEER_MISBEHAVE / EV_PEER_BANNED.
 * Returns the live ban threshold/hours/decay config plus offence
 * counts since boot, bucketed by canonical offence kind. */
static int h_zcl_peer_report(const struct mcp_request *req,
                              struct mcp_response *res)
{
    (void)req;
    char body[2048];
    size_t n = mcp_metrics_peer_report_json(body, sizeof(body));
    if (n == 0) return -1;
    res->body = strdup(body);
    return res->body ? 0 : -1;
}

static const struct mcp_param_spec p_addnode[] = {
    { "addr",   MCP_PARAM_STR, true,  "IP:port",
      0, 0, 1, 128, NULL, NULL },
    { "action", MCP_PARAM_STR, false, "add | remove | onetry",
      0, 0, 0, 0, "add,remove,onetry", "\"onetry\"" },
};
static const struct mcp_param_spec p_pingpeer[] = {
    { "peer_id", MCP_PARAM_INT, true, "Peer ID from zcl_peers",
      0, 1000000, 0, 0, NULL, NULL },
};

static const struct mcp_tool_route k_routes[] = {
    { "zcl_peers", "net",
      "Connected peers with addresses, latency, services, heights.",
      NULL, 0, h_zcl_peers },
    { "zcl_networkinfo", "net",
      "Network info: version, connections, relay fee.",
      NULL, 0, h_zcl_networkinfo },
    { "zcl_addnode", "net",
      "Add/remove peer. Actions: add, remove, onetry.",
      p_addnode, sizeof(p_addnode) / sizeof(p_addnode[0]), h_zcl_addnode },
    { "zcl_onion_status", "net",
      "Tor onion service: .onion address, bootstrap state.",
      NULL, 0, h_zcl_onion_status },
    { "zcl_gametypes", "net",
      "P2P game types: Ping (latency measurement), TicTacToe.",
      NULL, 0, h_zcl_gametypes },
    { "zcl_pingpeer", "net",
      "Measure round-trip latency to a connected peer.",
      p_pingpeer, sizeof(p_pingpeer) / sizeof(p_pingpeer[0]), h_zcl_pingpeer },
    { "zcl_peerlatency", "net",
      "Latency for all peers: ping_ms, min_ping_ms, avg_latency_ms.",
      NULL, 0, h_zcl_peerlatency },
    { "zcl_peer_report", "net",
      "Peer scoring report: live ban threshold/hours/decay config plus "
      "per-kind offence counts and total bans observed since boot.",
      NULL, 0, h_zcl_peer_report },
};

void mcp_register_net(void)
{
    for (size_t i = 0; i < sizeof(k_routes) / sizeof(k_routes[0]); i++)
        mcp_router_register(&k_routes[i]);
}
