#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "utiltime.h"
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

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
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    t -= 116444736000000000ULL;
    return (int64_t)(t / 10000);
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

int64_t GetTimeMicros(void)
{
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    t -= 116444736000000000ULL;
    return (int64_t)(t / 10);
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
#endif
}

void MilliSleep(int64_t n)
{
#ifdef _WIN32
    Sleep((DWORD)n);
#else
    struct timespec ts;
    ts.tv_sec = n / 1000;
    ts.tv_nsec = (n % 1000) * 1000000;
    nanosleep(&ts, NULL);
#endif
}

void DateTimeStrFormat(char *out, size_t out_size, const char *fmt, int64_t nTime)
{
    time_t t = (time_t)nTime;
    struct tm tm_buf;
#ifdef _WIN32
    gmtime_s(&tm_buf, &t);
#else
    gmtime_r(&t, &tm_buf);
#endif
    strftime(out, out_size, fmt, &tm_buf);
}
