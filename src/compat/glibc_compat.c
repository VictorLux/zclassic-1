/* Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include <stddef.h>

#ifdef __linux__
#include <sys/select.h>

void __chk_fail(void) __attribute__((__noreturn__));

long __fdelt_warn(long a)
{
    if (a >= FD_SETSIZE)
        __chk_fail();
    return a / __NFDBITS;
}

long __fdelt_chk(long) __attribute__((weak, alias("__fdelt_warn")));
#endif
