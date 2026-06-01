/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Recovered-UTXO injection helper shared by missing-UTXO recovery sources.
 *
 * Recovery sources must verify their evidence before calling this helper. This
 * file only materializes the recovered transaction outputs into the live coins
 * cache and emits the diagnostic proving which source supplied them. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "coins/coins_view.h"

#include "process_block_internal.h"

bool process_block_inject_missing_utxo(
    struct coins_view_cache *coins_tip,
    const struct uint256 *txid,
    uint32_t missing_vout,
    const struct transaction *tx,
    int height,
    const char *source,
    int retry_no)
{
    if (!coins_tip || !txid || !tx || height < 0 || !source)
        return false;
    if (missing_vout >= tx->num_vout) {
        char hex[65];
        uint256_get_hex(txid, hex);
        fprintf(stderr, "[self-heal] %s found tx %s at h=%d but vout=%u "
                "is out of range (outputs=%zu)\n",
                source, hex, height, missing_vout, tx->num_vout);
        return false;
    }

    struct coins_cache_entry *entry =
        coins_view_cache_modify_new(coins_tip, txid);
    if (!entry)
        return false;

    coins_from_transaction(&entry->coins, tx, height);
    entry->flags = COINS_CACHE_DIRTY;

    char hex[65];
    uint256_get_hex(txid, hex);
    printf("[self-heal] Recovered UTXO %s:%u from %s h=%d — retry %d\n",
           hex, missing_vout, source, height, retry_no);
    fflush(stdout);
    return true;
}
