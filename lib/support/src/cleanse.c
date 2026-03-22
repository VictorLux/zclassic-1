/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2015 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "support/cleanse.h"
#include <string.h>

void memory_cleanse(void *ptr, size_t len)
{
#if defined(_WIN32)
    SecureZeroMemory(ptr, len);
#elif defined(__GLIBC__) && defined(_DEFAULT_SOURCE)
    /* POSIX explicit_bzero: guaranteed not to be optimized away by the compiler */
    explicit_bzero(ptr, len);
#elif defined(__GNUC__) || defined(__clang__)
    memset(ptr, 0, len);
    __asm__ __volatile__("" : : "r"(ptr) : "memory");
#else
    volatile unsigned char *p = (volatile unsigned char *)ptr;
    while (len--) *p++ = 0;
#endif
}
