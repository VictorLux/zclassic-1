/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * MCP wallet controller: balance, addresses, sending. */

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

DEFINE_PT(h_zcl_balance,         "z_gettotalbalance")
DEFINE_PT(h_zcl_getnewaddress,   "getnewaddress")
DEFINE_PT(h_zcl_z_getnewaddress, "z_getnewaddress")

static int h_zcl_send(const struct mcp_request *req, struct mcp_response *res)
{
    const char *from = json_get_str(json_get(req->args, "from"));
    const char *to   = json_get_str(json_get(req->args, "to"));
    const struct json_value *amt = json_get(req->args, "amount");
    double amount = (amt && amt->type == JSON_REAL) ? json_get_real(amt)
                                                    : (double)json_get_int(amt);
    char params[512];
    snprintf(params, sizeof(params),
             "[\"%s\",[{\"address\":\"%s\",\"amount\":%.8f}]]",
             from, to, amount);
    char *out = mcp_node_rpc("z_sendmany", params);
    if (!out) return -1;
    res->body = out;
    return 0;
}

static const struct mcp_param_spec p_send[] = {
    { "from",   MCP_PARAM_STR,  true, "Source address",
      0, 0, 1, 128, NULL, NULL },
    { "to",     MCP_PARAM_STR,  true, "Destination address",
      0, 0, 1, 128, NULL, NULL },
    { "amount", MCP_PARAM_REAL, true, "Amount in ZCL",
      0, 0, 0, 0, NULL, NULL },
};

static const struct mcp_tool_route k_routes[] = {
    { "zcl_balance", "wallet",
      "Total wallet balance: transparent + shielded.",
      NULL, 0, h_zcl_balance },
    { "zcl_getnewaddress", "wallet",
      "Generate new transparent (t-addr) receiving address.",
      NULL, 0, h_zcl_getnewaddress },
    { "zcl_z_getnewaddress", "wallet",
      "Generate new shielded Sapling (z-addr) receiving address.",
      NULL, 0, h_zcl_z_getnewaddress },
    { "zcl_send", "wallet",
      "Send ZCL (transparent or shielded).",
      p_send, sizeof(p_send) / sizeof(p_send[0]), h_zcl_send },
};

void mcp_register_wallet(void)
{
    for (size_t i = 0; i < sizeof(k_routes) / sizeof(k_routes[0]); i++)
        mcp_router_register(&k_routes[i]);
}
