/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "validation/sigops.h"
#include "script/script.h"
#include "script/script_flags.h"
#include "coins/coins_view.h"
#include "primitives/transaction.h"

uint64_t get_legacy_sig_op_count(const struct transaction *tx, uint32_t flags)
{
    uint64_t n = 0;
    for (size_t i = 0; i < tx->num_vin; i++)
        n += script_get_sig_op_count(&tx->vin[i].script_sig, flags, false);
    for (size_t i = 0; i < tx->num_vout; i++)
        n += script_get_sig_op_count(&tx->vout[i].script_pub_key, flags, false);
    return n;
}

uint64_t get_p2sh_sig_op_count(const struct transaction *tx,
                                struct coins_view_cache *view,
                                uint32_t flags)
{
    /* Coinbase has no real inputs.  The P2SH flag gates the whole
     * counter — pre-P2SH blocks historically had no P2SH concept. */
    if ((flags & SCRIPT_VERIFY_P2SH) == 0)
        return 0;
    if (transaction_is_coinbase(tx))
        return 0;

    uint64_t n = 0;
    for (size_t i = 0; i < tx->num_vin; i++) {
        const struct tx_out *prevout =
            coins_view_cache_get_output_for(view, &tx->vin[i]);
        if (!prevout)
            continue;  /* Missing-prevout is reported elsewhere in connect_block. */
        if (!script_is_pay_to_script_hash(&prevout->script_pub_key))
            continue;
        n += script_get_sig_op_count_p2sh(&prevout->script_pub_key,
                                           &tx->vin[i].script_sig, flags);
    }
    return n;
}
