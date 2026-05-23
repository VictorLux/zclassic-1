/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "../controllers.h"
#include "../router.h"
#include "../rpc_client.h"

#include "util/log_macros.h"

static int h_zcl_conditions(const struct mcp_request *req,
                            struct mcp_response *res)
{
    (void)req;
    return mcp_return_rpc_body(res,
                               mcp_node_rpc("dumpstate",
                                            "[\"condition_engine\"]"),
                               "dumpstate", "mcp.conditions");
}

static const struct mcp_tool_route k_routes[] = {
    { "zcl_conditions", "ops",
      "Self-heal condition engine state: registered conditions, active "
      "flags, remedy attempts, outcomes, clear counts, and thresholds.",
      NULL, 0, h_zcl_conditions, 0, NULL },
};

void mcp_register_conditions(void)
{
    for (size_t i = 0; i < PARAM_COUNT(k_routes); i++)
        mcp_router_register(&k_routes[i]);
}
