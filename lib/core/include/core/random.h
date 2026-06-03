/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef BITCOIN_RANDOM_H
#define BITCOIN_RANDOM_H

#include "core/uint256.h"
#include <stdbool.h>
#include <stdint.h>

void GetRandBytes(unsigned char *buf, size_t num);
uint64_t GetRand(uint64_t nMax);
int GetRandInt(int nMax);


extern uint32_t insecure_rand_Rz;
extern uint32_t insecure_rand_Rw;

static inline uint32_t insecure_rand(void)
{
    insecure_rand_Rz = 36969 * (insecure_rand_Rz & 65535) + (insecure_rand_Rz >> 16);
    insecure_rand_Rw = 18000 * (insecure_rand_Rw & 65535) + (insecure_rand_Rw >> 16);
    return (insecure_rand_Rw << 16) + insecure_rand_Rz;
}

#endif
