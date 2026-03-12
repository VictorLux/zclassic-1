/* Copyright (c) 2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "encoding/base58.h"
#include "core/hash.h"
#include <ctype.h>
#include <string.h>
#include <assert.h>

static const char base58_chars[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

bool base58_encode(const unsigned char *data, size_t data_len,
                   char *out, size_t out_size, size_t *out_len)
{
    const unsigned char *pbegin = data;
    const unsigned char *pend = data + data_len;

    int zeroes = 0;
    while (pbegin != pend && *pbegin == 0) {
        pbegin++;
        zeroes++;
    }

    size_t b58_size = (pend - pbegin) * 138 / 100 + 1;
    unsigned char b58[b58_size];
    memset(b58, 0, b58_size);

    while (pbegin != pend) {
        int carry = *pbegin;
        for (size_t i = b58_size; i > 0; i--) {
            carry += 256 * b58[i - 1];
            b58[i - 1] = carry % 58;
            carry /= 58;
        }
        assert(carry == 0);
        pbegin++;
    }

    size_t skip = 0;
    while (skip < b58_size && b58[skip] == 0)
        skip++;

    size_t result_len = zeroes + (b58_size - skip);
    if (out_len)
        *out_len = result_len;
    if (result_len + 1 > out_size)
        return false;

    for (int i = 0; i < zeroes; i++)
        out[i] = '1';
    for (size_t i = skip; i < b58_size; i++)
        out[zeroes + (i - skip)] = base58_chars[b58[i]];
    out[result_len] = '\0';
    return true;
}

bool base58_decode(const char *psz,
                   unsigned char *out, size_t out_size, size_t *out_len)
{
    while (*psz && isspace((unsigned char)*psz))
        psz++;

    int zeroes = 0;
    while (*psz == '1') {
        zeroes++;
        psz++;
    }

    size_t input_len = strlen(psz);
    size_t b256_size = input_len * 733 / 1000 + 1;
    unsigned char b256[b256_size];
    memset(b256, 0, b256_size);

    const char *p = psz;
    while (*p && !isspace((unsigned char)*p)) {
        const char *ch = strchr(base58_chars, *p);
        if (ch == NULL)
            return false;
        int carry = (int)(ch - base58_chars);
        for (size_t i = b256_size; i > 0; i--) {
            carry += 58 * b256[i - 1];
            b256[i - 1] = carry % 256;
            carry /= 256;
        }
        assert(carry == 0);
        p++;
    }

    while (isspace((unsigned char)*p))
        p++;
    if (*p != 0)
        return false;

    size_t skip = 0;
    while (skip < b256_size && b256[skip] == 0)
        skip++;

    size_t result_len = zeroes + (b256_size - skip);
    if (out_len)
        *out_len = result_len;
    if (result_len > out_size)
        return false;

    memset(out, 0, zeroes);
    memcpy(out + zeroes, b256 + skip, b256_size - skip);
    return true;
}

bool base58check_encode(const unsigned char *data, size_t data_len,
                        char *out, size_t out_size, size_t *out_len)
{
    unsigned char buf[data_len + 4];
    memcpy(buf, data, data_len);
    unsigned char hash[32];
    hash256(data, data_len, hash);
    memcpy(buf + data_len, hash, 4);
    return base58_encode(buf, data_len + 4, out, out_size, out_len);
}

bool base58check_decode(const char *str,
                        unsigned char *out, size_t out_size, size_t *out_len)
{
    unsigned char tmp[256];
    size_t tmp_len = 0;
    if (!base58_decode(str, tmp, sizeof(tmp), &tmp_len))
        return false;
    if (tmp_len < 4)
        return false;

    unsigned char hash[32];
    hash256(tmp, tmp_len - 4, hash);
    if (memcmp(hash, tmp + tmp_len - 4, 4) != 0)
        return false;

    size_t result_len = tmp_len - 4;
    if (out_len)
        *out_len = result_len;
    if (result_len > out_size)
        return false;

    memcpy(out, tmp, result_len);
    return true;
}
