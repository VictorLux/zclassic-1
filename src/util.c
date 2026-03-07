#ifndef _WIN32
#define _POSIX_C_SOURCE 200112L
#endif
/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "util.h"
#include "chainparamsbase.h"
#include "utiltime.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

#ifdef __linux__
#include <sched.h>
#endif

#ifdef HAVE_SYS_PRCTL_H
#include <sys/prctl.h>
#endif

struct arg_entry g_args[MAX_ARGS];
int g_nargs = 0;

bool fDebug = false;
bool fPrintToConsole = false;
bool fPrintToDebugLog = false;
bool fServer = false;
bool fLogTimestamps = true;
bool fLogIPs = false;

static int64_t nStartupTime = 0;
static FILE *fileout = NULL;
static char cachedDataDir[4096] = "";
static char cachedDataDirNet[4096] = "";

int64_t GetStartupTime(void)
{
    if (nStartupTime == 0)
        nStartupTime = GetTime();
    return nStartupTime;
}

static int find_arg(const char *key)
{
    for (int i = 0; i < g_nargs; i++) {
        if (strcmp(g_args[i].key, key) == 0)
            return i;
    }
    return -1;
}

void ParseParameters(int argc, const char *const argv[])
{
    g_nargs = 0;
    for (int i = 1; i < argc && g_nargs < MAX_ARGS; i++) {
        const char *arg = argv[i];
        const char *eq = strchr(arg, '=');
        char key[MAX_ARG_LEN];
        char value[MAX_ARG_LEN];

        if (eq) {
            size_t klen = (size_t)(eq - arg);
            if (klen >= MAX_ARG_LEN) klen = MAX_ARG_LEN - 1;
            memcpy(key, arg, klen);
            key[klen] = '\0';
            snprintf(value, MAX_ARG_LEN, "%s", eq + 1);
        } else {
            snprintf(key, MAX_ARG_LEN, "%s", arg);
            value[0] = '\0';
        }

        if (key[0] != '-') break;

        /* --foo → -foo */
        const char *k = key;
        if (k[0] == '-' && k[1] == '-')
            k++;

        int idx = find_arg(k);
        if (idx >= 0) {
            snprintf(g_args[idx].value, MAX_ARG_LEN, "%s", value);
        } else {
            snprintf(g_args[g_nargs].key, MAX_ARG_LEN, "%s", k);
            snprintf(g_args[g_nargs].value, MAX_ARG_LEN, "%s", value);
            g_nargs++;
        }
    }

    /* Interpret -nofoo as -foo=0 */
    for (int i = 0; i < g_nargs; i++) {
        if (strncmp(g_args[i].key, "-no", 3) == 0) {
            char positive[MAX_ARG_LEN];
            snprintf(positive, sizeof(positive), "-%s", g_args[i].key + 3);
            if (find_arg(positive) < 0 && g_nargs < MAX_ARGS) {
                bool val = !GetBoolArg(g_args[i].key, false);
                snprintf(g_args[g_nargs].key, MAX_ARG_LEN, "%s", positive);
                snprintf(g_args[g_nargs].value, MAX_ARG_LEN, "%d", val ? 1 : 0);
                g_nargs++;
            }
        }
    }
}

const char *GetArg(const char *arg, const char *default_val)
{
    int idx = find_arg(arg);
    if (idx >= 0)
        return g_args[idx].value;
    return default_val;
}

int64_t GetArgInt(const char *arg, int64_t default_val)
{
    int idx = find_arg(arg);
    if (idx >= 0)
        return strtoll(g_args[idx].value, NULL, 10);
    return default_val;
}

bool GetBoolArg(const char *arg, bool default_val)
{
    int idx = find_arg(arg);
    if (idx >= 0) {
        if (g_args[idx].value[0] == '\0')
            return true;
        return atoi(g_args[idx].value) != 0;
    }
    return default_val;
}

bool SoftSetArg(const char *arg, const char *value)
{
    if (find_arg(arg) >= 0)
        return false;
    if (g_nargs >= MAX_ARGS) return false;
    snprintf(g_args[g_nargs].key, MAX_ARG_LEN, "%s", arg);
    snprintf(g_args[g_nargs].value, MAX_ARG_LEN, "%s", value);
    g_nargs++;
    return true;
}

bool SoftSetBoolArg(const char *arg, bool value)
{
    return SoftSetArg(arg, value ? "1" : "0");
}

bool LogAcceptCategory(const char *category)
{
    if (category != NULL) {
        if (!fDebug) return false;
        /* Check if -debug includes this category */
        for (int i = 0; i < g_nargs; i++) {
            if (strcmp(g_args[i].key, "-debug") == 0) {
                if (g_args[i].value[0] == '\0' ||
                    strcmp(g_args[i].value, "1") == 0 ||
                    strcmp(g_args[i].value, category) == 0)
                    return true;
            }
        }
        return false;
    }
    return true;
}

int LogPrintStr(const char *str)
{
    if (fPrintToConsole) {
        int ret = (int)fwrite(str, 1, strlen(str), stdout);
        fflush(stdout);
        return ret;
    } else if (fPrintToDebugLog && fileout) {
        if (fLogTimestamps) {
            char ts[64];
            DateTimeStrFormat(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", GetTime());
            fprintf(fileout, "%s %s", ts, str);
        } else {
            fputs(str, fileout);
        }
        fflush(fileout);
        return (int)strlen(str);
    }
    return 0;
}

void GetDefaultDataDir(char *out, size_t out_size)
{
#ifdef _WIN32
    char path[MAX_PATH];
    if (SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, path) == S_OK)
        snprintf(out, out_size, "%s\\ZClassic", path);
    else
        snprintf(out, out_size, "ZClassic");
#elif defined(__APPLE__)
    const char *home = getenv("HOME");
    if (home && home[0])
        snprintf(out, out_size, "%s/Library/Application Support/ZClassic", home);
    else
        snprintf(out, out_size, "/ZClassic");
#else
    const char *home = getenv("HOME");
    if (home && home[0])
        snprintf(out, out_size, "%s/.zclassic", home);
    else
        snprintf(out, out_size, "/.zclassic");
#endif
}

void GetDataDir(bool fNetSpecific, char *out, size_t out_size)
{
    char *cached = fNetSpecific ? cachedDataDirNet : cachedDataDir;
    if (cached[0]) {
        snprintf(out, out_size, "%s", cached);
        return;
    }

    int idx = find_arg("-datadir");
    if (idx >= 0 && g_args[idx].value[0]) {
        snprintf(out, out_size, "%s", g_args[idx].value);
    } else {
        GetDefaultDataDir(out, out_size);
    }

    if (fNetSpecific) {
        const struct base_chain_params *bp = BaseParams();
        if (bp->strDataDir[0]) {
            size_t len = strlen(out);
#ifdef _WIN32
            snprintf(out + len, out_size - len, "\\%s", bp->strDataDir);
#else
            snprintf(out + len, out_size - len, "/%s", bp->strDataDir);
#endif
        }
    }

#ifdef _WIN32
    CreateDirectoryA(out, NULL);
#else
    mkdir(out, 0700);
#endif

    snprintf(cached, 4096, "%s", out);
}

void ClearDatadirCache(void)
{
    cachedDataDir[0] = '\0';
    cachedDataDirNet[0] = '\0';
}

void OpenDebugLog(void)
{
    char datadir[4096];
    GetDataDir(true, datadir, sizeof(datadir));
    char path[4116];
#ifdef _WIN32
    snprintf(path, sizeof(path), "%s\\debug.log", datadir);
#else
    snprintf(path, sizeof(path), "%s/debug.log", datadir);
#endif
    fileout = fopen(path, "a");
    if (fileout)
        setbuf(fileout, NULL);
}

void ShrinkDebugFile(void)
{
    char datadir[4096];
    GetDataDir(true, datadir, sizeof(datadir));
    char path[4116];
#ifdef _WIN32
    snprintf(path, sizeof(path), "%s\\debug.log", datadir);
#else
    snprintf(path, sizeof(path), "%s/debug.log", datadir);
#endif

    FILE *file = fopen(path, "r");
    if (!file) return;

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    if (size > 10 * 1000000) {
        char buf[200000];
        fseek(file, -((long)sizeof(buf)), SEEK_END);
        int nBytes = (int)fread(buf, 1, sizeof(buf), file);
        fclose(file);
        file = fopen(path, "w");
        if (file) {
            fwrite(buf, 1, (size_t)nBytes, file);
            fclose(file);
        }
    } else {
        fclose(file);
    }
}

void FileCommit(FILE *fp)
{
    fflush(fp);
#ifdef _WIN32
    HANDLE h = (HANDLE)_get_osfhandle(_fileno(fp));
    FlushFileBuffers(h);
#elif defined(__linux__) || defined(__NetBSD__)
    fdatasync(fileno(fp));
#elif defined(__APPLE__)
    fcntl(fileno(fp), F_FULLFSYNC, 0);
#else
    fsync(fileno(fp));
#endif
}

bool TruncateFile(FILE *file, unsigned int length)
{
#ifdef _WIN32
    return _chsize(_fileno(file), length) == 0;
#else
    return ftruncate(fileno(file), length) == 0;
#endif
}

int RaiseFileDescriptorLimit(int nMinFD)
{
#ifdef _WIN32
    return 2048;
#else
    struct rlimit limitFD;
    if (getrlimit(RLIMIT_NOFILE, &limitFD) != -1) {
        if (limitFD.rlim_cur < (rlim_t)nMinFD) {
            limitFD.rlim_cur = nMinFD;
            if (limitFD.rlim_cur > limitFD.rlim_max)
                limitFD.rlim_cur = limitFD.rlim_max;
            setrlimit(RLIMIT_NOFILE, &limitFD);
            getrlimit(RLIMIT_NOFILE, &limitFD);
        }
        return (int)limitFD.rlim_cur;
    }
    return nMinFD;
#endif
}

void AllocateFileRange(FILE *file, unsigned int offset, unsigned int length)
{
#ifdef _WIN32
    HANDLE h = (HANDLE)_get_osfhandle(_fileno(file));
    LARGE_INTEGER nFileSize;
    int64_t nEndPos = (int64_t)offset + length;
    nFileSize.u.LowPart = (DWORD)(nEndPos & 0xFFFFFFFF);
    nFileSize.u.HighPart = (LONG)(nEndPos >> 32);
    SetFilePointerEx(h, nFileSize, 0, FILE_BEGIN);
    SetEndOfFile(h);
#elif defined(__APPLE__)
    fstore_t fst;
    fst.fst_flags = F_ALLOCATECONTIG;
    fst.fst_posmode = F_PEOFPOSMODE;
    fst.fst_offset = 0;
    fst.fst_length = (off_t)offset + length;
    fst.fst_bytesalloc = 0;
    if (fcntl(fileno(file), F_PREALLOCATE, &fst) == -1) {
        fst.fst_flags = F_ALLOCATEALL;
        fcntl(fileno(file), F_PREALLOCATE, &fst);
    }
    ftruncate(fileno(file), fst.fst_length);
#elif defined(__linux__)
    off_t nEndPos = (off_t)offset + length;
    posix_fallocate(fileno(file), 0, nEndPos);
#else
    static const char buf[65536] = {0};
    fseek(file, (long)offset, SEEK_SET);
    while (length > 0) {
        unsigned int now = 65536;
        if (length < now) now = length;
        fwrite(buf, 1, now, file);
        length -= now;
    }
#endif
}

bool RenameOver(const char *src, const char *dest)
{
#ifdef _WIN32
    return MoveFileExA(src, dest, MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return rename(src, dest) == 0;
#endif
}

bool TryCreateDirectory(const char *path)
{
#ifdef _WIN32
    return CreateDirectoryA(path, NULL) != 0;
#else
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
        return false;
    return mkdir(path, 0700) == 0;
#endif
}

void SetupEnvironment(void)
{
#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__FreeBSD__) && !defined(__OpenBSD__)
    setenv("LC_ALL", "C", 0);
#endif
}

bool SetupNetworking(void)
{
#ifdef _WIN32
    WSADATA wsadata;
    int ret = WSAStartup(MAKEWORD(2, 2), &wsadata);
    if (ret != 0) return false;
#endif
    return true;
}

void RenameThread(const char *name)
{
#if defined(PR_SET_NAME)
    prctl(PR_SET_NAME, name, 0, 0, 0);
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
    pthread_set_name_np(pthread_self(), name);
#elif defined(__APPLE__)
    pthread_setname_np(name);
#else
    (void)name;
#endif
}

int GetNumCores(void)
{
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
#elif defined(_SC_NPROCESSORS_ONLN)
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#else
    return 1;
#endif
}

void HelpMessageGroup(const char *message, char *out, size_t out_size)
{
    snprintf(out, out_size, "%s\n\n", message);
}

void HelpMessageOpt(const char *option, const char *message, char *out, size_t out_size)
{
    snprintf(out, out_size, "  %s\n       %s\n\n", option, message);
}
