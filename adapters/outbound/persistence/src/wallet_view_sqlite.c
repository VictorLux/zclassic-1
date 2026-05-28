/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton
 *
 * wallet_view_sqlite — sqlite implementation of wallet_view_port.
 *
 * The two methods below are the raw queries that used to live inline in
 * app/services/src/wallet_view_projection.c, moved behind the port with
 * EXACT same SQL text and column order so the explorer / wallet UI
 * surface is byte-for-byte identical.
 */

#include "adapters/outbound/persistence/wallet_view_sqlite.h"

#include "util/ar_step_readonly.h"

#include <stdio.h>

/* `self` aliases the sqlite3* directly — there is no wrapper struct. */
static inline sqlite3 *db_of(void *self) { return (sqlite3 *)self; }

static int wv_list_receive_addresses_sqlite(
    void *self, struct wallet_view_receive_address *out, size_t max)
{
    sqlite3 *db = db_of(self);
    sqlite3_stmt *s = NULL;
    size_t count = 0;

    if (!db || !out || max == 0)
        return 0;

    if (sqlite3_prepare_v2(db,
            "SELECT address FROM wallet_sapling_keys "
            "WHERE address IS NOT NULL AND length(address) > 0 "
            "ORDER BY rowid",
            -1, &s, NULL) != SQLITE_OK || !s)
        return 0;

    while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW && count < max) {
        const char *raw = (const char *)sqlite3_column_text(s, 0);
        if (!raw || !raw[0])
            continue;
        snprintf(out[count].address, sizeof(out[count].address), "%s", raw);
        count++;
    }
    sqlite3_finalize(s);
    return (int)count;
}

static int wv_list_held_tokens_sqlite(
    void *self, struct wallet_view_held_token *out, size_t max)
{
    sqlite3 *db = db_of(self);
    sqlite3_stmt *s = NULL;
    size_t count = 0;

    if (!db || !out || max == 0)
        return 0;

    if (sqlite3_prepare_v2(db,
            "SELECT hex(t.token_id), t.ticker, t.decimals "
            "FROM zslp_tokens t "
            "JOIN zslp_transfers tr ON tr.token_id = t.token_id "
            "WHERE tr.to_addr IN (SELECT pubkey_hash FROM wallet_keys) "
            "  AND tr.tx_type IN ('GENESIS','MINT','SEND') "
            "GROUP BY t.token_id HAVING SUM(tr.amount) > 0 "
            "ORDER BY SUM(tr.amount) DESC LIMIT 10",
            -1, &s, NULL) != SQLITE_OK || !s)
        return 0;

    while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW && count < max) {
        const char *tid = (const char *)sqlite3_column_text(s, 0);
        const char *ticker = (const char *)sqlite3_column_text(s, 1);
        int decimals = sqlite3_column_int(s, 2);

        if (!tid || !ticker)
            continue;
        snprintf(out[count].token_id, sizeof(out[count].token_id), "%s", tid);
        snprintf(out[count].ticker, sizeof(out[count].ticker), "%s", ticker);
        out[count].decimals = decimals;
        count++;
    }
    sqlite3_finalize(s);
    return (int)count;
}

bool wallet_view_sqlite_bind(sqlite3 *db, struct wallet_view_port *out_port)
{
    if (!db || !out_port)
        return false;
    *out_port = (struct wallet_view_port){
        .self                   = db,
        .list_receive_addresses = wv_list_receive_addresses_sqlite,
        .list_held_tokens       = wv_list_held_tokens_sqlite,
    };
    return true;
}
