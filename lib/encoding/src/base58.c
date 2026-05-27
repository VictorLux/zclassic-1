/* Copyright (c) 2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * Thin wrapper around the pure codec in domain/encoding/base58.{c,h}.
 * Byte-for-byte external behavior is preserved — the wrappers just
 * forward to the domain functions. New callers should prefer the
 * domain symbols directly; this header survives as the stable public
 * API for the rest of the codebase.
 */

#include "encoding/base58.h"
#include "domain/encoding/base58.h"

bool base58_encode(const unsigned char *data, size_t data_len,
                   char *out, size_t out_size, size_t *out_len)
{
    return domain_encoding_base58_encode(data, data_len, out, out_size, out_len);
}

bool base58_decode(const char *psz,
                   unsigned char *out, size_t out_size, size_t *out_len)
{
    return domain_encoding_base58_decode(psz, out, out_size, out_len);
}

bool base58check_encode(const unsigned char *data, size_t data_len,
                        char *out, size_t out_size, size_t *out_len)
{
    return domain_encoding_base58check_encode(data, data_len, out, out_size, out_len);
}

bool base58check_decode(const char *str,
                        unsigned char *out, size_t out_size, size_t *out_len)
{
    return domain_encoding_base58check_decode(str, out, out_size, out_len);
}
