/* Copyright (c) 2012-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "util/clientversion.h"
#include <stdio.h>
#include <string.h>

const char CLIENT_NAME[] = "MagicBean";

#define CLIENT_VERSION_SUFFIX ""

#ifdef HAVE_BUILD_INFO
#include "util/build.h"
#endif

#ifdef GIT_ARCHIVE
#define GIT_COMMIT_ID "$Format:%h$"
#define GIT_COMMIT_DATE "$Format:%cD$"
#endif

/* Build version string without Boost preprocessor */
#if CLIENT_VERSION_BUILD < 25
#define RENDER_BUILD "-beta" DO_STRINGIZE(CLIENT_VERSION_BUILD)
#elif CLIENT_VERSION_BUILD < 50
#define _RC_NUM (CLIENT_VERSION_BUILD - 24)
#define RENDER_BUILD "-rc" DO_STRINGIZE(_RC_NUM)
#elif CLIENT_VERSION_BUILD == 50
#define RENDER_BUILD ""
#else
#define _DEV_NUM (CLIENT_VERSION_BUILD - 50)
#define RENDER_BUILD "-" DO_STRINGIZE(_DEV_NUM)
#endif

#define BUILD_DESC_FROM_UNKNOWN \
    "v" DO_STRINGIZE(CLIENT_VERSION_MAJOR) "." DO_STRINGIZE(CLIENT_VERSION_MINOR) \
    "." DO_STRINGIZE(CLIENT_VERSION_REVISION) RENDER_BUILD

#ifndef BUILD_DESC
#ifdef GIT_COMMIT_ID
#define BUILD_DESC BUILD_DESC_FROM_UNKNOWN "-g" GIT_COMMIT_ID
#else
#define BUILD_DESC BUILD_DESC_FROM_UNKNOWN "-unk"
#endif
#endif

#ifndef BUILD_DATE
#ifdef GIT_COMMIT_DATE
#define BUILD_DATE GIT_COMMIT_DATE
#else
#define BUILD_DATE __DATE__ ", " __TIME__
#endif
#endif

const char CLIENT_BUILD[] = BUILD_DESC CLIENT_VERSION_SUFFIX;
const char CLIENT_DATE[] = BUILD_DATE;

void FormatVersion(int nVersion, char *out, size_t out_size)
{
    int major = nVersion / 1000000;
    int minor = (nVersion / 10000) % 100;
    int rev = (nVersion / 100) % 100;
    int build = nVersion % 100;

    if (build < 25)
        snprintf(out, out_size, "%d.%d.%d-beta%d", major, minor, rev, build + 1);
    else if (build < 50)
        snprintf(out, out_size, "%d.%d.%d-rc%d", major, minor, rev, build - 24);
    else if (build == 50)
        snprintf(out, out_size, "%d.%d.%d", major, minor, rev);
    else
        snprintf(out, out_size, "%d.%d.%d-%d", major, minor, rev, build - 50);
}

void FormatFullVersion(char *out, size_t out_size)
{
    snprintf(out, out_size, "%s", CLIENT_BUILD);
}

void FormatSubVersion(const char *name, int nClientVersion,
                      const char *const *comments, int ncomments,
                      char *out, size_t out_size)
{
    char ver[64];
    FormatVersion(nClientVersion, ver, sizeof(ver));

    size_t pos = 0;
    pos += (size_t)snprintf(out + pos, out_size - pos, "/%s:%s", name, ver);
    if (ncomments > 0 && pos < out_size) {
        pos += (size_t)snprintf(out + pos, out_size - pos, "(%s", comments[0]);
        for (int i = 1; i < ncomments && pos < out_size; i++)
            pos += (size_t)snprintf(out + pos, out_size - pos, "; %s", comments[i]);
        if (pos < out_size)
            pos += (size_t)snprintf(out + pos, out_size - pos, ")");
    }
    if (pos < out_size)
        snprintf(out + pos, out_size - pos, "/");
}
