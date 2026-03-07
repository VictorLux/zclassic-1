/* Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include <stdbool.h>
#include <string.h>

#ifdef __linux__
#include <sys/select.h>
#endif

static bool sanity_test_memcpy(void)
{
    unsigned int test[1025];
    unsigned int verify[1025];
    memset(verify, 0, sizeof(verify));
    for (unsigned int i = 0; i < 1025; i++)
        test[i] = i;
    memcpy(verify, test, sizeof(test));
    for (unsigned int i = 0; i < 1025; i++) {
        if (verify[i] != i)
            return false;
    }
    return true;
}

#ifdef __linux__
static bool sanity_test_fdelt(void)
{
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(0, &fds);
    return FD_ISSET(0, &fds);
}
#endif

bool glibc_sanity_test(void)
{
#ifdef __linux__
    if (!sanity_test_fdelt())
        return false;
#endif
    return sanity_test_memcpy();
}
