/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

// one-result-type-ok:count-of-rows-projected — both public functions
// (wv_list_receive_addresses / wv_list_held_tokens) return int = the
// number of rows written into the caller's out-array, which IS the
// payload. An empty wallet legitimately returns 0; a prepare failure also
// returns 0 because there are simply no rows to project. The read-only
// projection has no mutating decision and nothing to carry beyond the
// count. Wrapping it in zcl_result would discard the row count.
//
// Storage is reached ONLY through wallet_view_port — the raw sqlite
// queries live in the sqlite adapter. This file is pure domain logic:
// it binds the default sqlite adapter and drives the port. The public
// functions still accept a sqlite3* so callers (wallet_view controllers,
// tests) are unchanged.

#include "services/wallet_view_projection.h"

#include "adapters/outbound/persistence/wallet_view_sqlite.h"
#include "ports/wallet_view_port.h"

#include <stddef.h>

/* struct wv_receive_address / struct wv_held_token (public service types)
 * and the port row types are deliberately kept layout-identical so the
 * port can fill the caller's buffer in one shot with no per-row copy.
 * These asserts fail the build if either drifts. */
_Static_assert(sizeof(struct wv_receive_address) ==
                   sizeof(struct wallet_view_receive_address),
               "wv_receive_address size must match port row");
_Static_assert(offsetof(struct wv_receive_address, address) ==
                   offsetof(struct wallet_view_receive_address, address),
               "wv_receive_address layout must match port row");
_Static_assert(sizeof(struct wv_held_token) ==
                   sizeof(struct wallet_view_held_token),
               "wv_held_token size must match port row");
_Static_assert(offsetof(struct wv_held_token, token_id) ==
                       offsetof(struct wallet_view_held_token, token_id) &&
                   offsetof(struct wv_held_token, ticker) ==
                       offsetof(struct wallet_view_held_token, ticker) &&
                   offsetof(struct wv_held_token, decimals) ==
                       offsetof(struct wallet_view_held_token, decimals),
               "wv_held_token layout must match port row");

int wv_list_receive_addresses(sqlite3 *db, struct wv_receive_address *out,
                              size_t max)
{
    if (!db || !out || max == 0)
        return 0;
    struct wallet_view_port port;
    if (!wallet_view_sqlite_bind(db, &port))
        return 0;
    /* Layout-identical to struct wallet_view_receive_address (asserted
     * above); project directly into the caller's buffer. */
    return port.list_receive_addresses(
        port.self, (struct wallet_view_receive_address *)out, max);
}

int wv_list_held_tokens(sqlite3 *db, struct wv_held_token *out, size_t max)
{
    if (!db || !out || max == 0)
        return 0;
    struct wallet_view_port port;
    if (!wallet_view_sqlite_bind(db, &port))
        return 0;
    /* Layout-identical to struct wallet_view_held_token (asserted above). */
    return port.list_held_tokens(
        port.self, (struct wallet_view_held_token *)out, max);
}
