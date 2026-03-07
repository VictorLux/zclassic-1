/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "primitives/transaction_c.h"
#include "hash.h"
#include <stdlib.h>
#include <stdio.h>

void transaction_init(struct transaction *tx)
{
    tx->overwintered = false;
    tx->version = 1;
    tx->version_group_id = 0;
    tx->vin = NULL;
    tx->num_vin = 0;
    tx->vout = NULL;
    tx->num_vout = 0;
    tx->lock_time = 0;
    tx->expiry_height = 0;
    tx->value_balance = 0;
    uint256_set_null(&tx->hash);
}

void transaction_free(struct transaction *tx)
{
    free(tx->vin);
    free(tx->vout);
    tx->vin = NULL;
    tx->vout = NULL;
    tx->num_vin = 0;
    tx->num_vout = 0;
}

bool transaction_alloc(struct transaction *tx, size_t num_vin, size_t num_vout)
{
    if (num_vin > MAX_TX_INPUTS || num_vout > MAX_TX_OUTPUTS)
        return false;

    tx->vin = calloc(num_vin, sizeof(struct tx_in));
    tx->vout = calloc(num_vout, sizeof(struct tx_out));
    if ((num_vin && !tx->vin) || (num_vout && !tx->vout)) {
        free(tx->vin);
        free(tx->vout);
        tx->vin = NULL;
        tx->vout = NULL;
        return false;
    }
    tx->num_vin = num_vin;
    tx->num_vout = num_vout;

    for (size_t i = 0; i < num_vin; i++)
        tx_in_init(&tx->vin[i]);
    for (size_t i = 0; i < num_vout; i++)
        tx_out_set_null(&tx->vout[i]);

    return true;
}

int64_t transaction_get_value_out(const struct transaction *tx)
{
    int64_t total = 0;
    for (size_t i = 0; i < tx->num_vout; i++) {
        total += tx->vout[i].value;
        if (!MoneyRange(tx->vout[i].value) || !MoneyRange(total))
            return -1;
    }
    return total;
}

void outpoint_to_string(const struct outpoint *op, char *buf, size_t buflen)
{
    char hex[65];
    uint256_get_hex(&op->hash, hex);
    snprintf(buf, buflen, "%s:%u", hex, op->n);
}
