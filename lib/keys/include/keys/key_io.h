/* Copyright (c) 2014-2016 The Bitcoin Core developers
 * Copyright (c) 2016-2018 The Zcash developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_KEY_IO_H
#define ZCL_KEY_IO_H

#include "domain/encoding/base58.h"
#include "script/standard.h"
#include "keys/key.h"
#include <stdbool.h>

bool encode_destination(const struct tx_destination *dest,
                        const unsigned char *pubkey_prefix, size_t pfx_len,
                        const unsigned char *script_prefix, size_t spfx_len,
                        char *out, size_t outsize);

bool decode_destination(const char *str,
                        const unsigned char *pubkey_prefix, size_t pfx_len,
                        const unsigned char *script_prefix, size_t spfx_len,
                        struct tx_destination *dest);

bool encode_secret(const struct privkey *key,
                   const unsigned char *prefix, size_t pfx_len,
                   char *out, size_t outsize);

bool decode_secret(const char *str,
                   const unsigned char *prefix, size_t pfx_len,
                   struct privkey *key);

#endif
