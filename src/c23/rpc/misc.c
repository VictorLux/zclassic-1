/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "rpc/misc.h"
#include "chain/chainparams.h"
#include "json/json.h"
#include "keys/key_io.h"
#include "net/version.h"
#include "util/clientversion.h"
#include "validation/chainstate.h"
#include <stdlib.h>
#include <string.h>

static struct main_state *g_ms = NULL;

void rpc_misc_set_state(struct main_state *ms)
{
    g_ms = ms;
}

static bool rpc_getinfo(const struct json_value *params, bool help,
                          struct json_value *result)
{
    (void)params;
    if (help) {
        json_set_str(result,
            "getinfo\n"
            "Returns an object containing various state info.");
        return true;
    }

    json_set_object(result);
    json_push_kv_int(result, "version", CLIENT_VERSION);
    json_push_kv_int(result, "protocolversion", PROTOCOL_VERSION);

    struct block_index *tip = g_ms ?
        active_chain_tip(&g_ms->chain_active) : NULL;
    json_push_kv_int(result, "blocks", tip ? tip->nHeight : 0);
    json_push_kv_int(result, "timeoffset", 0);
    json_push_kv_int(result, "connections", 0);
    json_push_kv_real(result, "difficulty", 0.0);
    json_push_kv_bool(result, "testnet",
                       strcmp(chain_params_get()->strNetworkID, "test") == 0);
    json_push_kv_real(result, "relayfee", 0.00000100);
    json_push_kv_int(result, "errors", 0);

    return true;
}

static bool rpc_validateaddress(const struct json_value *params, bool help,
                                  struct json_value *result)
{
    if (help || json_size(params) != 1) {
        json_set_str(result,
            "validateaddress \"zcashaddress\"\n"
            "Return information about the given address.\n"
            "Arguments:\n"
            "1. \"address\" (string, required) The address to validate");
        return true;
    }

    const struct json_value *addr_val = json_at(params, 0);
    if (!addr_val || addr_val->type != JSON_STR) {
        json_set_str(result, "Invalid address");
        return false;
    }

    const char *addr = json_get_str(addr_val);
    const struct chain_params *cp = chain_params_get();

    size_t pk_len, sc_len;
    const unsigned char *pk_pfx = chain_params_base58_prefix(
        cp, B58_PUBKEY_ADDRESS, &pk_len);
    const unsigned char *sc_pfx = chain_params_base58_prefix(
        cp, B58_SCRIPT_ADDRESS, &sc_len);

    struct tx_destination dest;
    bool valid = decode_destination(addr, pk_pfx, pk_len,
                                     sc_pfx, sc_len, &dest);

    json_set_object(result);
    json_push_kv_bool(result, "isvalid", valid);
    json_push_kv_str(result, "address", addr);

    if (valid) {
        if (dest.type == DEST_KEY_ID)
            json_push_kv_str(result, "scriptPubKey", "pubkeyhash");
        else if (dest.type == DEST_SCRIPT_ID)
            json_push_kv_str(result, "scriptPubKey", "scripthash");
    }

    return true;
}

static bool rpc_stop(const struct json_value *params, bool help,
                       struct json_value *result)
{
    (void)params;
    if (help) {
        json_set_str(result,
            "stop\n"
            "Stop ZClassic server.");
        return true;
    }

    json_set_str(result, "ZClassic server stopping");
    exit(0);
    return true;
}

void register_misc_rpc_commands(struct rpc_table *t)
{
    struct rpc_command cmds[] = {
        { "control", "getinfo",          rpc_getinfo,          true },
        { "util",    "validateaddress",  rpc_validateaddress,  true },
        { "control", "stop",             rpc_stop,             true },
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_append(t, &cmds[i]);
}
