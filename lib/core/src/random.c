/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "core/random.h"
#include <string.h>

#include <fcntl.h>
#include <unistd.h>

void GetRandBytes(unsigned char *buf, size_t num)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        memset(buf, 0, num);
        return;
    }
    size_t got = 0;
    while (got < num) {
        ssize_t r = read(fd, buf + got, num - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
}

uint64_t GetRand(uint64_t nMax)
{
    if (nMax == 0) return 0;
    uint64_t nRange = (UINT64_MAX / nMax) * nMax;
    uint64_t nRand = 0;
    do {
        GetRandBytes((unsigned char *)&nRand, sizeof(nRand));
    } while (nRand >= nRange);
    return nRand % nMax;
}

int GetRandInt(int nMax)
{
    return (int)GetRand((uint64_t)nMax);
}

void GetRandHash(struct uint256 *hash)
{
    GetRandBytes(hash->data, 32);
}

uint32_t insecure_rand_Rz = 11;
uint32_t insecure_rand_Rw = 11;

void seed_insecure_rand(bool deterministic)
{
    if (deterministic) {
        insecure_rand_Rz = insecure_rand_Rw = 11;
    } else {
        uint32_t tmp;
        do {
            GetRandBytes((unsigned char *)&tmp, 4);
        } while (tmp == 0 || tmp == 0x9068ffffU);
        insecure_rand_Rz = tmp;
        do {
            GetRandBytes((unsigned char *)&tmp, 4);
        } while (tmp == 0 || tmp == 0x464fffffU);
        insecure_rand_Rw = tmp;
    }
}
