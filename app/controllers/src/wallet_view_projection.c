/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "controllers/wallet_view_internal.h"
#include "util/log_macros.h"

int wv_list_receive_addresses(sqlite3 *db, struct wv_receive_address *out,
                              size_t max)
{
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

    while (sqlite3_step(s) == SQLITE_ROW && count < max) {  // raw-sql-ok: a3
        const char *raw = (const char *)sqlite3_column_text(s, 0);
        if (!raw || !raw[0])
            continue;
        snprintf(out[count].address, sizeof(out[count].address), "%s", raw);
        count++;
    }
    sqlite3_finalize(s);
    return (int)count;
}

int wv_list_held_tokens(sqlite3 *db, struct wv_held_token *out, size_t max)
{
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

    while (sqlite3_step(s) == SQLITE_ROW && count < max) {  // raw-sql-ok: a3
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
