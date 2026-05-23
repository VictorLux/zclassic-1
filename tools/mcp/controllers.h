/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Entry points for the MCP domain controllers.  Each function
 * registers all tools in one domain with the router.  Call from
 * mcp_server_main() after mcp_router_reset(). */

#ifndef ZCL_MCP_CONTROLLERS_H
#define ZCL_MCP_CONTROLLERS_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "json/json.h"
#include "router.h"
#include "rpc_client.h"
#include "util/log_macros.h"

void mcp_register_ops(void);      /* zcl_status, zcl_health, zcl_events, zcl_rpc, ... */
void mcp_register_diagnostics(void); /* zcl_sql, zcl_state, zcl_node_log, zcl_profile, ... */
void mcp_register_conditions(void);  /* zcl_conditions */
void mcp_register_chain(void);    /* zcl_getblock, zcl_mmb, zcl_syncstate, ...        */
void mcp_register_net(void);      /* zcl_peers, zcl_addnode, zcl_pingpeer, ...        */
void mcp_register_wallet(void);   /* zcl_balance, zcl_send, zcl_getnewaddress, ...    */
void mcp_register_app(void);      /* zcl_name_*, zcl_msg_*, zcl_market_*, zcl_swap_*  */
void mcp_register_meta(void);     /* zcl_tools_list, zcl_self_test, zcl_logtail       */

/* Compile-time element count for a statically-sized array. Used by the
 * route + param-spec tables in every controller, and by the register
 * loops. Replaces the `sizeof(arr) / sizeof(arr[0])` boilerplate that
 * was repeated ~50 times across the MCP layer. */
#define PARAM_COUNT(arr) (sizeof(arr) / sizeof((arr)[0]))

/* ── Defaulted JSON accessors ─────────────────────────────────────
 *
 * Optional MCP parameters with handler-side defaults used to look like:
 *
 *   const struct json_value *mc = json_get(req->args, "minconf");
 *   int64_t v = mc ? json_get_int(mc) : 1;
 *
 * Three inline helpers collapse the two-line pattern to one line and
 * make the default visible inline at the call site. Repeated ~30
 * times across the controllers prior to adoption.
 */
static inline int64_t
json_get_int_or(const struct json_value *obj, const char *key, int64_t dflt)
{
    const struct json_value *v = json_get(obj, key);
    return v ? json_get_int(v) : dflt;
}

static inline bool
json_get_bool_or(const struct json_value *obj, const char *key, bool dflt)
{
    const struct json_value *v = json_get(obj, key);
    return v ? json_get_bool(v) : dflt;
}

static inline const char *
json_get_str_or(const struct json_value *obj, const char *key, const char *dflt)
{
    const struct json_value *v = json_get(obj, key);
    return v ? json_get_str(v) : dflt;
}

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
        return mcp_return_rpc_body(res, mcp_node_rpc(rpc_method, NULL),        \
                                    rpc_method, log_tag);                      \
    }

/* ── Shared handler tail ──────────────────────────────────────────
 *
 * Many handlers do the same thing after calling mcp_node_rpc(): if the
 * returned body is non-NULL hand it to the response; otherwise set
 * MCP_ERR_HANDLER_FAILED, format a "RPC X returned null" error message
 * (visible in the error envelope), and emit a LOG_ERR with the same
 * text so check-silent-errors stays green.
 *
 * This helper captures that ~7-line tail in one call so handlers that
 * need a custom params construction don't have to duplicate the rest.
 * Used by DEFINE_PT and by any handler that hand-constructs params.
 *
 * Required includes (see DEFINE_PT block):
 *   "../router.h", "../rpc_client.h", "util/log_macros.h", <stdio.h>
 */
static inline int mcp_return_rpc_body(struct mcp_response *res,
                                       char *body_or_null,
                                       const char *rpc_method,
                                       const char *log_tag)
{
    if (!body_or_null) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "RPC %s returned null", rpc_method);
        LOG_ERR(log_tag, "RPC %s returned null", rpc_method);
    }
    res->body = body_or_null;
    return 0;
}

/* Same as mcp_return_rpc_body but with a printf-style parameter context
 * appended to the error message and log line. Use when the failure
 * message benefits from caller-visible inputs (e.g., "name=foo",
 * "peer_id=5"). On success ctx_fmt is ignored. */
__attribute__((format(printf, 5, 6)))
static inline int mcp_return_rpc_body_ctx(struct mcp_response *res,
                                          char *body_or_null,
                                          const char *rpc_method,
                                          const char *log_tag,
                                          const char *ctx_fmt, ...)
{
    if (!body_or_null) {
        char ctx[192];
        va_list ap;
        va_start(ap, ctx_fmt);
        vsnprintf(ctx, sizeof(ctx), ctx_fmt, ap);
        va_end(ap);
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "RPC %s failed: %s", rpc_method, ctx);
        LOG_ERR(log_tag, "%s failed: %s", rpc_method, ctx);
    }
    res->body = body_or_null;
    return 0;
}

#endif
