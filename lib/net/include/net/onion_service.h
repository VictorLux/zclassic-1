/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Onion service integration — bridges Tor dynhost to zclassic23 MVC.
 *
 * This is the glue between our Tor fork's dynhost and our app layer.
 * When a request arrives over a Tor circuit:
 *   dynhost → onion_service_handle_request → MVC controllers → response
 *
 * No SOCKS. No ports. No HTTP server. Just C function calls. */

#ifndef ZCL_NET_ONION_SERVICE_H
#define ZCL_NET_ONION_SERVICE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Initialize the onion service layer.
 * Called from app_init() after Tor is linked in.
 * datadir: path for persistent state
 * Returns the .onion address or NULL on failure. */
const char *onion_service_start(const char *datadir);

/* Stop the onion service. */
void onion_service_stop(void);

/* Get the .onion address (NULL if not started). */
const char *onion_service_get_address(void);

/* Set the .onion address (called by tor_integration after reading key). */
void onion_service_set_address(const char *address);

/* Handle an incoming request from dynhost.
 * This is the callback registered with Tor's dynhost webserver.
 *
 * method: "GET" or "POST"
 * path: URL path (e.g., "/", "/blog", "/search")
 * body: request body (NULL for GET)
 * body_len: body length
 * response: output buffer
 * response_max: max response size
 * Returns bytes written to response. */
size_t onion_service_handle_request(const char *method,
                                     const char *path,
                                     const uint8_t *body,
                                     size_t body_len,
                                     uint8_t *response,
                                     size_t response_max);

/* Register a .onion site on-chain via ZSLP.
 * title: human-readable site name
 * description: short description
 * Returns true if the ZSLP tx was broadcast. */
bool onion_service_register_site(const char *title,
                                  const char *description);

/* Search registered .onion sites by keyword.
 * Returns count of matches. */
struct onion_site {
    char address[68];     /* .onion address */
    char title[128];      /* site title */
    char description[256]; /* site description */
    int height;           /* registration block height */
};

int onion_service_search(const char *query,
                          struct onion_site *results, size_t max);

#endif
