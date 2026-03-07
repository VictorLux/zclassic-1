/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef BITCOIN_UTIL_H
#define BITCOIN_UTIL_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define DEFAULT_LOGTIMEMICROS  false
#define DEFAULT_LOGIPS         false
#define DEFAULT_LOGTIMESTAMPS  true

#define MAX_ARGS   512
#define MAX_ARG_LEN 1024

struct arg_entry {
    char key[MAX_ARG_LEN];
    char value[MAX_ARG_LEN];
};

extern struct arg_entry g_args[MAX_ARGS];
extern int g_nargs;

extern bool fDebug;
extern bool fPrintToConsole;
extern bool fPrintToDebugLog;
extern bool fServer;
extern bool fLogTimestamps;
extern bool fLogIPs;

int64_t GetStartupTime(void);

void ParseParameters(int argc, const char *const argv[]);
const char *GetArg(const char *arg, const char *default_val);
int64_t GetArgInt(const char *arg, int64_t default_val);
bool GetBoolArg(const char *arg, bool default_val);
bool SoftSetArg(const char *arg, const char *value);
bool SoftSetBoolArg(const char *arg, bool value);

bool LogAcceptCategory(const char *category);
int LogPrintStr(const char *str);

void GetDefaultDataDir(char *out, size_t out_size);
void GetDataDir(bool fNetSpecific, char *out, size_t out_size);
void ClearDatadirCache(void);

void OpenDebugLog(void);
void ShrinkDebugFile(void);
void FileCommit(FILE *fileout);
bool TruncateFile(FILE *file, unsigned int length);
int RaiseFileDescriptorLimit(int nMinFD);
void AllocateFileRange(FILE *file, unsigned int offset, unsigned int length);

bool RenameOver(const char *src, const char *dest);
bool TryCreateDirectory(const char *path);

void SetupEnvironment(void);
bool SetupNetworking(void);
void RenameThread(const char *name);
int GetNumCores(void);

void HelpMessageGroup(const char *message, char *out, size_t out_size);
void HelpMessageOpt(const char *option, const char *message, char *out, size_t out_size);

static inline bool IsSwitchChar(char c)
{
#ifdef _WIN32
    return c == '-' || c == '/';
#else
    return c == '-';
#endif
}

#define LogPrintf(...) do { \
    if (LogAcceptCategory(NULL)) { \
        char _logbuf[4096]; \
        snprintf(_logbuf, sizeof(_logbuf), __VA_ARGS__); \
        LogPrintStr(_logbuf); \
    } \
} while(0)

#define LogPrint(category, ...) do { \
    if (LogAcceptCategory(category)) { \
        char _logbuf[4096]; \
        snprintf(_logbuf, sizeof(_logbuf), __VA_ARGS__); \
        LogPrintStr(_logbuf); \
    } \
} while(0)

#endif
