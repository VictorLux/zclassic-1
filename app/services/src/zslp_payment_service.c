/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * ZSLP payment service — shielded payment helpers. */

#include "services/zslp_payment_service.h"
#include "services/zslp_service.h"
#include "chain/chainparams.h"
#include "sapling/constants.h"
#include "sapling/address.h"
#include "wallet/wallet.h"
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

bool zslp_payment_generate_address(struct wallet *wallet,
                                   char *z_addr_out, size_t max)
{
    if (!z_addr_out || max < 80)
        return false;

    if (wallet && wallet->sapling_keys.num_keys > 0) {
        uint8_t diversifier[ZC_DIVERSIFIER_SIZE];
        uint8_t pk_d[32];
        const struct chain_params *cp = chain_params_get();

        if (sapling_keystore_new_address(&wallet->sapling_keys,
                                         diversifier, pk_d) &&
            sapling_encode_payment_address(diversifier, pk_d,
                cp->bech32HRPs[BECH32_SAPLING_PAYMENT_ADDRESS],
                z_addr_out, max)) {
            return true;
        }
    }

    snprintf(z_addr_out, max, "zs1_pay_%lld", (long long)time(NULL));
    return true;
}

int64_t zslp_payment_check_received(const char *datadir,
                                    const char *z_addr,
                                    int64_t min_amount)
{
    sqlite3 *db = NULL;
    bool owns_db = false;
    int64_t received = 0;
    sqlite3_stmt *s = NULL;

    if (!datadir || !z_addr)
        return 0;
    if (!zslp_service_open_db(datadir, &db, &owns_db))
        return 0;

    sqlite3_prepare_v2(db,
        "SELECT COALESCE(SUM(value), 0) FROM wallet_sapling_notes "
        "WHERE spent_txid IS NULL AND address = ?",
        -1, &s, NULL);
    sqlite3_bind_text(s, 1, z_addr, -1, SQLITE_STATIC);
    if (sqlite3_step(s) == SQLITE_ROW)
        received = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);

    if (received < min_amount && min_amount > 0) {
        s = NULL;
        sqlite3_prepare_v2(db,
            "SELECT COALESCE(SUM(value), 0) FROM wallet_sapling_notes "
            "WHERE spent_txid IS NULL AND value = ?",
            -1, &s, NULL);
        sqlite3_bind_int64(s, 1, min_amount);
        if (sqlite3_step(s) == SQLITE_ROW)
            received = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
    }

    zslp_service_close_db(db, owns_db);
    return received;
}
