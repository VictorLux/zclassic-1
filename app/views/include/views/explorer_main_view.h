/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Explorer "main" view — the self-contained /wallet HTML page (its own
 * HTML+CSS+JS; the page fetches /api/wallet client-side). View shape: the
 * controller routes the request, this file assembles the page. Moved out of
 * explorer_controller.c so the controller stops building HTML inline. Output
 * is byte-identical to the prior controller implementation — this was a
 * move, not a redesign. */

#ifndef ZCL_VIEWS_EXPLORER_MAIN_VIEW_H
#define ZCL_VIEWS_EXPLORER_MAIN_VIEW_H

#include <stddef.h>
#include <stdint.h>

/* Render the full /wallet page (HTTP response, including headers).
 * Returns the number of bytes written into `r`. */
size_t explorer_view_wallet_page(uint8_t *r, size_t max);

#endif
