/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * ZSLP payment service — shielded payment helpers. */

#include "services/zslp_payment_service.h"
#include "services/zslp_service.h"
#include "models/wallet_tx.h"
#include "chain/chainparams.h"
#include "sapling/constants.h"
#include "sapling/address.h"
#include "wallet/wallet.h"
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
    struct node_db ndb;
    char db_path[1024];
    int64_t received = 0;

    if (!datadir || !z_addr)
        return 0;

    memset(&ndb, 0, sizeof(ndb));
    snprintf(db_path, sizeof(db_path), "%s/node.db", datadir);
    if (!node_db_open(&ndb, db_path))
        return 0;

    received = db_sapling_note_balance_for_address(&ndb, z_addr);

    if (received < min_amount && min_amount > 0) {
        received = db_sapling_note_balance_for_exact_value(&ndb, min_amount);
    }

    node_db_close(&ndb);
    return received;
}
