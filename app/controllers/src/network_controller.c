/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "controllers/network_controller.h"
#include "controllers/strong_params.h"
#include "json/json.h"
#include "net/connman.h"
#include "net/version.h"
#include "util/clientversion.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct connman *g_cm = NULL;

void rpc_net_set_connman(struct connman *cm)
{
    g_cm = cm;
}

static bool rpc_getnetworkinfo(const struct json_value *params, bool help,
                                 struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "getnetworkinfo\n"
        "Returns an object containing various state info "
        "regarding P2P networking.");

    json_set_object(result);
    json_push_kv_int(result, "version", CLIENT_VERSION);
    json_push_kv_str(result, "subversion", CLIENT_NAME);
    json_push_kv_int(result, "protocolversion", PROTOCOL_VERSION);

    size_t conns = g_cm ? connman_get_node_count(g_cm) : 0;
    json_push_kv_int(result, "connections", (int64_t)conns);

    struct json_value networks = {0};
    json_set_array(&networks);
    json_push_kv(result, "networks", &networks);
    json_free(&networks);

    json_push_kv_real(result, "relayfee", 0.00000100);

    struct json_value localaddrs = {0};
    json_set_array(&localaddrs);
    json_push_kv(result, "localaddresses", &localaddrs);
    json_free(&localaddrs);

    return true;
}

static bool rpc_getpeerinfo(const struct json_value *params, bool help,
                              struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "getpeerinfo\n"
        "Returns data about each connected network node.");

    json_set_array(result);
    if (!g_cm) return true;

    zcl_mutex_lock(&g_cm->manager.cs_nodes);
    for (size_t i = 0; i < g_cm->manager.num_nodes; i++) {
        struct p2p_node *node = g_cm->manager.nodes[i];
        struct json_value entry = {0};
        json_set_object(&entry);

        json_push_kv_int(&entry, "id", (int64_t)node->id);
        json_push_kv_str(&entry, "addr", node->addr_name);
        json_push_kv_str(&entry, "subver", node->clean_sub_ver);
        json_push_kv_int(&entry, "version", (int64_t)node->version);
        json_push_kv_bool(&entry, "inbound", node->inbound);
        json_push_kv_int(&entry, "startingheight",
                          (int64_t)node->starting_height);
        json_push_kv_int(&entry, "conntime", node->time_connected);
        json_push_kv_int(&entry, "lastsend", node->last_send);
        json_push_kv_int(&entry, "lastrecv", node->last_recv);
        json_push_kv_int(&entry, "bytessent", (int64_t)node->send_bytes);
        json_push_kv_int(&entry, "bytesrecv", (int64_t)node->recv_bytes);

        double ping_ms = (double)node->ping_usec_time / 1000000.0;
        json_push_kv_real(&entry, "pingtime", ping_ms);

        json_push_back(result, &entry);
        json_free(&entry);
    }
    zcl_mutex_unlock(&g_cm->manager.cs_nodes);

    return true;
}

static bool rpc_getconnectioncount(const struct json_value *params, bool help,
                                     struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "getconnectioncount\n"
        "Returns the number of connections to other nodes.");

    size_t conns = g_cm ? connman_get_node_count(g_cm) : 0;
    json_set_int(result, (int64_t)conns);
    return true;
}

static bool rpc_ping_rpc(const struct json_value *params, bool help,
                           struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "ping\n"
        "Requests that a ping be sent to all other nodes.");

    if (g_cm) {
        zcl_mutex_lock(&g_cm->manager.cs_nodes);
        for (size_t i = 0; i < g_cm->manager.num_nodes; i++)
            g_cm->manager.nodes[i]->ping_queued = true;
        zcl_mutex_unlock(&g_cm->manager.cs_nodes);
    }

    json_set_null(result);
    return true;
}

static bool rpc_addnode(const struct json_value *params, bool help,
                         struct json_value *result)
{
    RPC_HELP(help, result,
        "addnode \"node\" \"add|remove|onetry\"\n"
        "Attempts to add or remove a node from the addnode list.");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 2, 2);
    const char *node_str = rpc_require_str(&p, 0, "node");
    const char *cmd = rpc_require_str(&p, 1, "command");
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); return false; }

    if (!g_cm) {
        json_set_str(result, "P2P not initialized");
        return false;
    }

    if (strcmp(cmd, "onetry") == 0 || strcmp(cmd, "add") == 0) {
        /* Parse host:port — split on last colon */
        char host[256];
        uint16_t port = g_cm->manager.default_port;
        strncpy(host, node_str, sizeof(host) - 1);
        host[sizeof(host) - 1] = '\0';
        char *colon = strrchr(host, ':');
        if (colon && colon != host) {
            *colon = '\0';
            int p_val = atoi(colon + 1);
            if (p_val > 0 && p_val <= 65535)
                port = (uint16_t)p_val;
        }
        connman_add_seed_node(g_cm, host, port);
        json_set_null(result);
        return true;
    }

    json_set_null(result);
    return true;
}

void register_net_rpc_commands(struct rpc_table *t)
{
    struct rpc_command cmds[] = {
        { "network", "getnetworkinfo",    rpc_getnetworkinfo,    true },
        { "network", "getpeerinfo",       rpc_getpeerinfo,       true },
        { "network", "getconnectioncount", rpc_getconnectioncount, true },
        { "network", "ping",              rpc_ping_rpc,          true },
        { "network", "addnode",           rpc_addnode,           true },
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_append(t, &cmds[i]);
}
