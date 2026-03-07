/* Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include <stddef.h>
#include <string.h>

#if HAVE_DECL_STRNLEN == 0
size_t strnlen(const char *start, size_t max_len)
{
    const char *end = (const char *)memchr(start, '\0', max_len);
    return end ? (size_t)(end - start) : max_len;
}
#endif
