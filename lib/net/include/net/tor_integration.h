/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tor integration for zclassic23.
 * Embeds Tor with dynhost to serve a blockchain explorer and wallet UI
 * over .onion addresses. No ports exposed, all traffic over Tor.
 *
 * Architecture:
 *   zclassic23 main thread → starts Tor in background thread
 *   Tor creates ephemeral .onion → dynhost intercepts connections
 *   dynhost_webserver → calls zclassic23 MVC controllers
 *   Controllers access chain data via global state pointers
 *   HTML responses flow back through Tor circuits to client
 *
 * Usage:
 *   tor_integration_start(datadir, p2p_port);
 *   // .onion address printed to log
 *   // Accessible via Tor Browser at http://xxxxx.onion/
 *   tor_integration_stop(); */

#ifndef ZCL_NET_TOR_INTEGRATION_H
#define ZCL_NET_TOR_INTEGRATION_H

#include <stdbool.h>
#include <stdint.h>

/* Start Tor in a background thread with dynhost enabled.
 * Creates an ephemeral .onion address for the node.
 * datadir: path to store Tor state (e.g., ~/.zclassic-c23/tor/)
 * Returns true if Tor started successfully. */
bool tor_integration_start(const char *datadir, uint16_t p2p_port);

/* Stop Tor gracefully. */
void tor_integration_stop(void);

/* Get the .onion address (NULL if not started). */
const char *tor_integration_get_onion_address(void);

/* Check if Tor is running and bootstrapped. */
bool tor_integration_is_ready(void);

#endif
