/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton
 *
 * wallet_view_port — storage interface for the wallet "view" projection
 * (the receive-address list and the held-token summary the explorer /
 * wallet UI render).
 *
 * This is a read-ONLY, NON-CONSENSUS projection. It reads the wallet key
 * tables and the ZSLP token/transfer indices to produce two small,
 * bounded lists. wallet_view_projection.c is the only domain logic;
 * everything below this interface is storage.
 *
 * The seam exists so the service never names sqlite. The two methods
 * here capture exactly the queries the service issues:
 *
 *   list_receive_addresses(out,max)  the sapling receive-address list
 *                                    ("SELECT address FROM
 *                                     wallet_sapling_keys ...")
 *   list_held_tokens(out,max)        the top-10 held-token summary
 *                                    (the zslp_tokens JOIN zslp_transfers
 *                                     aggregate)
 *
 * No sqlite type appears in this header. The adapter under
 * adapters/outbound/persistence/ is the only thing that includes sqlite
 * for this subsystem.
 *
 * Threading: the live adapter wraps a single sqlite3* opened by boot.
 * Both methods are read-only and run on request threads; sqlite's own
 * locking serializes them against the wallet writer — the same
 * concurrency contract the raw code had before the seam.
 */

#ifndef ZCL_PORTS_WALLET_VIEW_PORT_H
#define ZCL_PORTS_WALLET_VIEW_PORT_H

#include <stddef.h>

/* One wallet receive address. Mirrors struct wv_receive_address in
 * services/wallet_view_projection.h field-for-field; declared here so
 * the port has no dependency on the service header. */
struct wallet_view_receive_address {
    char address[128];
};

/* One held-token summary row. Mirrors struct wv_held_token. */
struct wallet_view_held_token {
    char token_id[65];
    char ticker[16];
    int  decimals;
};

struct wallet_view_port {
    void *self;

    /* Project the wallet's sapling receive addresses into the
     * caller-owned `out` buffer in stable (rowid) order. Returns the
     * number of rows written (<= max). Returns 0 on storage error,
     * empty wallet, or bad args. */
    int (*list_receive_addresses)(void *self,
                                  struct wallet_view_receive_address *out,
                                  size_t max);

    /* Project the top-10 held-token summary (tokens with a positive
     * net balance to a wallet key) into the caller-owned `out` buffer,
     * highest balance first. Returns the number of rows written
     * (<= max). Returns 0 on storage error, no holdings, or bad args. */
    int (*list_held_tokens)(void *self,
                            struct wallet_view_held_token *out,
                            size_t max);
};

#endif /* ZCL_PORTS_WALLET_VIEW_PORT_H */
