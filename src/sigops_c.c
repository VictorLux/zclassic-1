/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "sigops_c.h"
#include "script/script_c.h"

uint64_t get_legacy_sig_op_count(const struct transaction *tx, uint32_t flags)
{
    uint64_t n = 0;
    for (size_t i = 0; i < tx->num_vin; i++)
        n += script_get_sig_op_count(&tx->vin[i].script_sig, flags, false);
    for (size_t i = 0; i < tx->num_vout; i++)
        n += script_get_sig_op_count(&tx->vout[i].script_pub_key, flags, false);
    return n;
}
