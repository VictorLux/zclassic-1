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

#define MAX_ARGS   512
#define MAX_ARG_LEN 1024

struct arg_entry {
    char key[MAX_ARG_LEN];
    char value[MAX_ARG_LEN];
};

extern struct arg_entry g_args[MAX_ARGS];
extern int g_nargs;

void ParseParameters(int argc, const char *const argv[]);
const char *GetArg(const char *arg, const char *default_val);
int64_t GetArgInt(const char *arg, int64_t default_val);
bool GetBoolArg(const char *arg, bool default_val);
bool SoftSetArg(const char *arg, const char *value);

bool LogAcceptCategory(const char *category);
int LogPrintStr(const char *str);

void GetDefaultDataDir(char *out, size_t out_size);
void GetDataDir(bool fNetSpecific, char *out, size_t out_size);
void ClearDataDirCache(void);
void SetDataDir(const char *datadir);

void FileCommit(FILE *fileout);

void SetupEnvironment(void);
void RenameThread(const char *name);
int GetNumCores(void);

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
