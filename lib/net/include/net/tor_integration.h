/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tor integration for zclassic23.
 *
 * Architecture: dynhost runs inside our modified Tor binary.
 * When a .onion request arrives, dynhost calls our handler directly.
 * No ports. No sockets. No HTTP. Just C function calls over Tor circuits.
 *
 * Usage:
 *   tor_integration_set_handler(my_handler, my_ctx);
 *   tor_integration_start(datadir, p2p_port);
 *   // .onion address printed to log
 *   tor_integration_stop(); */

#ifndef ZCL_NET_TOR_INTEGRATION_H
#define ZCL_NET_TOR_INTEGRATION_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Request handler callback — dynhost calls this directly.
 * path: URL path (e.g., "/", "/blog/post1")
 * request_data: raw request body (NULL for GET)
 * request_len: body length
 * response: output buffer (caller allocates)
 * response_max: max response size
 * Returns bytes written to response, or 0 for 404. */
typedef size_t (*tor_request_handler_fn)(const char *path,
                                          const uint8_t *request_data,
                                          size_t request_len,
                                          uint8_t *response,
                                          size_t response_max,
                                          void *ctx);

/* Set the request handler before starting Tor. */
void tor_integration_set_handler(tor_request_handler_fn handler, void *ctx);

/* Start Tor with dynhost. Creates .onion, no ports exposed. */
bool tor_integration_start(const char *datadir, uint16_t p2p_port);

/* Stop Tor. */
void tor_integration_stop(void);

/* Get .onion address (NULL if not ready). */
const char *tor_integration_get_onion_address(void);

/* Check if Tor is bootstrapped. */
bool tor_integration_is_ready(void);

#endif
