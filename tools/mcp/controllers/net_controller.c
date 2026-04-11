/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * MCP net controller: peers, network info, peer discovery, ping, games. */

#include "../controllers.h"
#include "../router.h"
#include "../rpc_client.h"

#include "json/json.h"

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
};

void mcp_register_net(void)
{
    for (size_t i = 0; i < sizeof(k_routes) / sizeof(k_routes[0]); i++)
        mcp_router_register(&k_routes[i]);
}
