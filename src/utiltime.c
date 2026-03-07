#define _POSIX_C_SOURCE 200809L
/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "utiltime.h"
#include <time.h>

static int64_t nMockTime = 0;

int64_t GetTime(void)
{
    if (nMockTime) return nMockTime;
    return (int64_t)time(NULL);
}

void SetMockTime(int64_t nMockTimeIn)
{
    nMockTime = nMockTimeIn;
}

int64_t GetTimeMillis(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int64_t GetTimeMicros(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

void MilliSleep(int64_t n)
{
    struct timespec ts;
    ts.tv_sec = n / 1000;
    ts.tv_nsec = (n % 1000) * 1000000;
    nanosleep(&ts, NULL);
}

void DateTimeStrFormat(char *out, size_t out_size, const char *fmt, int64_t nTime)
{
    time_t t = (time_t)nTime;
    struct tm tm_buf;
    (void)gmtime_r(&t, &tm_buf);
    strftime(out, out_size, fmt, &tm_buf);
}
