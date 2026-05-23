/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_SERVICES_WALLET_VIEW_PROJECTION_H
#define ZCL_SERVICES_WALLET_VIEW_PROJECTION_H

#include <stddef.h>
#include <sqlite3.h>

struct wv_receive_address {
    char address[128];
};

struct wv_held_token {
    char token_id[65];
    char ticker[16];
    int decimals;
};

int wv_list_receive_addresses(sqlite3 *db, struct wv_receive_address *out,
                              size_t max);
int wv_list_held_tokens(sqlite3 *db, struct wv_held_token *out, size_t max);

#endif /* ZCL_SERVICES_WALLET_VIEW_PROJECTION_H */
