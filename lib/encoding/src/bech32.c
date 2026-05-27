/* Copyright (c) 2017 Pieter Wuille
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * Thin wrapper around the pure codec in domain/encoding/bech32.{c,h}.
 * Byte-for-byte external behavior is preserved.
 */

#include "encoding/bech32.h"
#include "domain/encoding/bech32.h"

bool bech32_encode(char *out, size_t out_size,
                   const char *hrp, const uint8_t *values, size_t values_len)
{
    return domain_encoding_bech32_encode(out, out_size, hrp, values, values_len);
}

bool bech32_decode(char *hrp_out, size_t hrp_size,
                   uint8_t *data_out, size_t data_size, size_t *data_len,
                   const char *str)
{
    return domain_encoding_bech32_decode(hrp_out, hrp_size, data_out, data_size, data_len, str);
}
