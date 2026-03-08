/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "rpc/net_rpc.h"
#include "json/json.h"
#include "net/version.h"
#include "util/clientversion.h"

static bool rpc_getnetworkinfo(const struct json_value *params, bool help,
                                 struct json_value *result)
{
    (void)params;
    if (help) {
        json_set_str(result,
            "getnetworkinfo\n"
            "Returns an object containing various state info "
            "regarding P2P networking.");
        return true;
    }

    json_set_object(result);
    json_push_kv_int(result, "version", CLIENT_VERSION);
    json_push_kv_str(result, "subversion", CLIENT_NAME);
    json_push_kv_int(result, "protocolversion", PROTOCOL_VERSION);
    json_push_kv_int(result, "connections", 0);

    struct json_value networks;
    json_set_array(&networks);
    json_push_kv(result, "networks", &networks);
    json_free(&networks);

    json_push_kv_real(result, "relayfee", 0.00000100);

    struct json_value localaddrs;
    json_set_array(&localaddrs);
    json_push_kv(result, "localaddresses", &localaddrs);
    json_free(&localaddrs);

    return true;
}

static bool rpc_getpeerinfo(const struct json_value *params, bool help,
                              struct json_value *result)
{
    (void)params;
    if (help) {
        json_set_str(result,
            "getpeerinfo\n"
            "Returns data about each connected network node.");
        return true;
    }

    json_set_array(result);
    return true;
}

static bool rpc_getconnectioncount(const struct json_value *params, bool help,
                                     struct json_value *result)
{
    (void)params;
    if (help) {
        json_set_str(result,
            "getconnectioncount\n"
            "Returns the number of connections to other nodes.");
        return true;
    }

    json_set_int(result, 0);
    return true;
}

static bool rpc_ping_rpc(const struct json_value *params, bool help,
                           struct json_value *result)
{
    (void)params;
    if (help) {
        json_set_str(result,
            "ping\n"
            "Requests that a ping be sent to all other nodes.");
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
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_append(t, &cmds[i]);
}
