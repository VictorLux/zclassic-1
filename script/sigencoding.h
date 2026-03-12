/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2016 The Bitcoin Core developers
 * Copyright (c) 2017-2018 The Bitcoin developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_SCRIPT_SIGENCODING_H
#define ZCL_SCRIPT_SIGENCODING_H

#include "script/script_error.h"
#include "script/sighashtype.h"
#include "script/script_flags.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

static inline struct sighash_type sig_get_hash_type(const unsigned char *sig,
                                                    size_t siglen)
{
    if (siglen == 0)
        return (struct sighash_type){0};
    return (struct sighash_type){sig[siglen - 1]};
}

bool check_data_signature_encoding(const unsigned char *sig, size_t siglen,
                                   uint32_t flags, ScriptError *serror);

bool check_transaction_signature_encoding(const unsigned char *sig,
                                          size_t siglen, uint32_t flags,
                                          ScriptError *serror);

bool check_pubkey_encoding(const unsigned char *pubkey, size_t len,
                           uint32_t flags, ScriptError *serror);

#endif
