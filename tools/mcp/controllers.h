/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Entry points for the MCP domain controllers.  Each function
 * registers all tools in one domain with the router.  Call from
 * mcp_server_main() after mcp_router_reset(). */

#ifndef ZCL_MCP_CONTROLLERS_H
#define ZCL_MCP_CONTROLLERS_H

void mcp_register_ops(void);      /* zcl_status, zcl_health, zcl_events, zcl_rpc, ... */
void mcp_register_chain(void);    /* zcl_getblock, zcl_mmb, zcl_syncstate, ...        */
void mcp_register_net(void);      /* zcl_peers, zcl_addnode, zcl_pingpeer, ...        */
void mcp_register_wallet(void);   /* zcl_balance, zcl_send, zcl_getnewaddress, ...    */
void mcp_register_app(void);      /* zcl_name_*, zcl_msg_*, zcl_market_*, zcl_swap_*  */
void mcp_register_meta(void);     /* zcl_tools_list, zcl_self_test, zcl_logtail       */

/* ── Pass-through handler macro ────────────────────────────────────
 *
 * Most MCP handlers are just "call the RPC, forward the JSON body,
 * log on failure". DEFINE_PT generates such a handler from a single
 * line. The macro includes LOG_ERR on the null path so the
 * check-silent-errors lint gate stays green.
 *
 * Usage:
 *   DEFINE_PT(h_zcl_foo, "rpc_method", "mcp.bar")
 *
 * Required headers in the .c that uses this macro:
 *   #include "../router.h"        — struct mcp_request/response
 *   #include "../rpc_client.h"    — mcp_node_rpc()
 *   #include "util/log_macros.h"  — LOG_ERR
 *   <stdio.h>                     — snprintf
 *
 * Handlers that need typed args, conditional dispatch, or richer
 * error messages keep their hand-written form — this macro is
 * deliberately limited to the "no params, generic null log" shape.
 */
#define DEFINE_PT(fn_name, rpc_method, log_tag)                                \
    static int fn_name(const struct mcp_request *req,                          \
                       struct mcp_response *res)                               \
    {                                                                          \
        (void)req;                                                             \
        char *out = mcp_node_rpc(rpc_method, NULL);                            \
        if (!out) {                                                            \
            res->error = MCP_ERR_HANDLER_FAILED;                               \
            snprintf(res->error_message, sizeof(res->error_message),           \
                     "RPC %s returned null", rpc_method);                      \
            LOG_ERR(log_tag, "RPC %s returned null", rpc_method);              \
        }                                                                      \
        res->body = out;                                                       \
        return 0;                                                              \
    }

#endif
