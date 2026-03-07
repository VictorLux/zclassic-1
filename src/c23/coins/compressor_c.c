/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "coins/compressor_c.h"
#include <assert.h>
#include <string.h>

bool script_compress(const struct script *s, unsigned char *out, size_t *out_len)
{
    /* P2PKH: OP_DUP OP_HASH160 <20> <hash> OP_EQUALVERIFY OP_CHECKSIG */
    if (s->size == 25 && s->data[0] == OP_DUP && s->data[1] == OP_HASH160 &&
        s->data[2] == 20 && s->data[23] == OP_EQUALVERIFY &&
        s->data[24] == OP_CHECKSIG) {
        *out_len = 21;
        out[0] = 0x00;
        memcpy(out + 1, s->data + 3, 20);
        return true;
    }
    /* P2SH: OP_HASH160 <20> <hash> OP_EQUAL */
    if (s->size == 23 && s->data[0] == OP_HASH160 && s->data[1] == 20 &&
        s->data[22] == OP_EQUAL) {
        *out_len = 21;
        out[0] = 0x01;
        memcpy(out + 1, s->data + 2, 20);
        return true;
    }
    /* P2PK compressed: <33> <compressed pubkey> OP_CHECKSIG */
    if (s->size == 35 && s->data[0] == 33 && s->data[34] == OP_CHECKSIG &&
        (s->data[1] == 0x02 || s->data[1] == 0x03)) {
        *out_len = 33;
        out[0] = s->data[1];
        memcpy(out + 1, s->data + 2, 32);
        return true;
    }
    /* P2PK uncompressed: <65> <04 pubkey> OP_CHECKSIG */
    if (s->size == 67 && s->data[0] == 65 && s->data[66] == OP_CHECKSIG &&
        s->data[1] == 0x04) {
        struct pubkey pk;
        pubkey_set(&pk, s->data + 1, 65);
        if (!pubkey_is_fully_valid(&pk))
            return false;
        *out_len = 33;
        memcpy(out + 1, s->data + 2, 32);
        out[0] = 0x04 | (s->data[64] & 0x01);
        return true;
    }
    return false;
}

bool script_decompress(struct script *s, unsigned int nSize,
                       const unsigned char *in, size_t in_len)
{
    (void)in_len;
    switch (nSize) {
    case 0x00: /* P2PKH */
        s->size = 25;
        s->data[0] = OP_DUP;
        s->data[1] = OP_HASH160;
        s->data[2] = 20;
        memcpy(s->data + 3, in, 20);
        s->data[23] = OP_EQUALVERIFY;
        s->data[24] = OP_CHECKSIG;
        return true;
    case 0x01: /* P2SH */
        s->size = 23;
        s->data[0] = OP_HASH160;
        s->data[1] = 20;
        memcpy(s->data + 2, in, 20);
        s->data[22] = OP_EQUAL;
        return true;
    case 0x02: /* compressed pubkey */
    case 0x03:
        s->size = 35;
        s->data[0] = 33;
        s->data[1] = (unsigned char)nSize;
        memcpy(s->data + 2, in, 32);
        s->data[34] = OP_CHECKSIG;
        return true;
    case 0x04: /* uncompressed pubkey */
    case 0x05: {
        unsigned char vch[33] = {0};
        vch[0] = (unsigned char)(nSize - 2);
        memcpy(vch + 1, in, 32);
        struct pubkey pk;
        pubkey_set(&pk, vch, 33);
        if (!pubkey_decompress(&pk))
            return false;
        assert(pk.size == 65);
        s->size = 67;
        s->data[0] = 65;
        memcpy(s->data + 1, pk.vch, 65);
        s->data[66] = OP_CHECKSIG;
        return true;
    }
    }
    return false;
}

unsigned int script_compress_special_size(unsigned int nSize)
{
    if (nSize == 0 || nSize == 1) return 20;
    if (nSize == 2 || nSize == 3 || nSize == 4 || nSize == 5) return 32;
    return 0;
}

uint64_t compress_amount(uint64_t n)
{
    if (n == 0) return 0;
    int e = 0;
    while (((n % 10) == 0) && e < 9) { n /= 10; e++; }
    if (e < 9) {
        int d = (int)(n % 10);
        assert(d >= 1 && d <= 9);
        n /= 10;
        return 1 + (n * 9 + (uint64_t)d - 1) * 10 + (uint64_t)e;
    }
    return 1 + (n - 1) * 10 + 9;
}

uint64_t decompress_amount(uint64_t x)
{
    if (x == 0) return 0;
    x--;
    int e = (int)(x % 10);
    x /= 10;
    uint64_t n = 0;
    if (e < 9) {
        int d = (int)(x % 9) + 1;
        x /= 9;
        n = x * 10 + (uint64_t)d;
    } else {
        n = x + 1;
    }
    while (e) { n *= 10; e--; }
    return n;
}
