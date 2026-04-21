/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_RPC_HTTPSERVER_H
#define ZCL_RPC_HTTPSERVER_H

#include "rpc/server.h"
#include "json/json.h"
#include <stdbool.h>
#include <stdint.h>

bool rpc_http_start(const struct rpc_table *table, uint16_t port,
                     const char *rpc_user, const char *rpc_password,
                     const char *datadir);
void rpc_http_stop(void);
bool rpc_http_is_running(void);
bool rpc_http_tls_active(void);

/* Cookie rotation — call manually for testing; background thread calls
 * automatically every ZCL_RPC_COOKIE_ROTATE_SEC seconds (default 24h). */
void rpc_http_cookie_rotate(void);
int  rpc_http_cookie_rotate_sec(void);

/* P24.11 test surface: builds the standard JSON-RPC response envelope
 * used by the HTTP server. Safe to call on stack-dirtied / previously
 * uninitialized `response` storage. Production code also routes through
 * this helper to avoid reintroducing stack-init regressions in the HTTP
 * response path. */
bool rpc_http_test_build_response_envelope(bool rpc_ok,
                                           const char *method,
                                           struct json_value *rpc_result,
                                           const struct json_value *id,
                                           struct json_value *response);

#endif
