/* Copyright (c) 2017 Pieter Wuille
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef BITCOIN_BECH32_H
#define BITCOIN_BECH32_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool bech32_encode(char *out, size_t out_size,
                   const char *hrp, const uint8_t *values, size_t values_len);

bool bech32_decode(char *hrp_out, size_t hrp_size,
                   uint8_t *data_out, size_t data_size, size_t *data_len,
                   const char *str);

#endif
