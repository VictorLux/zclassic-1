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

#endif
