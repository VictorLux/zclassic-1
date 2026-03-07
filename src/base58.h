/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef BITCOIN_BASE58_H
#define BITCOIN_BASE58_H

#include <stdbool.h>
#include <stddef.h>

bool base58_encode(const unsigned char *data, size_t data_len,
                   char *out, size_t out_size, size_t *out_len);

bool base58_decode(const char *str,
                   unsigned char *out, size_t out_size, size_t *out_len);

bool base58check_encode(const unsigned char *data, size_t data_len,
                        char *out, size_t out_size, size_t *out_len);

bool base58check_decode(const char *str,
                        unsigned char *out, size_t out_size, size_t *out_len);

#endif
