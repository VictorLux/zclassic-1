/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Async-signal-safe fatal-signal handler. See signal_handler.h. */

#define _GNU_SOURCE
#include "util/signal_handler.h"

#include <execinfo.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>

/* Async-signal-safe unsigned-decimal writer. Returns bytes written. */
static int write_uint(int fd, unsigned long v)
{
    char buf[32];
    int n = 0;
    if (v == 0) { buf[n++] = '0'; }
    while (v > 0) { buf[n++] = (char)('0' + (v % 10)); v /= 10; }
    /* reverse */
    for (int i = 0; i < n / 2; i++) {
        char t = buf[i]; buf[i] = buf[n - 1 - i]; buf[n - 1 - i] = t;
    }
    return (int)write(fd, buf, (size_t)n);
}

/* Async-signal-safe hex writer (lowercase, no 0x prefix). */
static int write_hex(int fd, unsigned long v)
{
    static const char H[] = "0123456789abcdef";
    char buf[18];
    int n = 0;
    if (v == 0) { buf[n++] = '0'; }
    while (v > 0) { buf[n++] = H[v & 0xF]; v >>= 4; }
    for (int i = 0; i < n / 2; i++) {
        char t = buf[i]; buf[i] = buf[n - 1 - i]; buf[n - 1 - i] = t;
    }
    return (int)write(fd, buf, (size_t)n);
}

static int write_s(int fd, const char *s)
{
    return (int)write(fd, s, strlen(s));
}

/* The handler itself. SA_SIGINFO style. */
static void fatal_handler(int sig, siginfo_t *info, void *ucontext)
{
    (void)ucontext;
    const int fd = STDERR_FILENO;

    /* Build & emit: [fatal-signal] sig=N code=M addr=0x... pid=P tid=T */
    write_s(fd, "[fatal-signal] sig=");
    write_uint(fd, (unsigned long)sig);
    write_s(fd, " code=");
    write_uint(fd, (unsigned long)(info ? info->si_code : 0));
    write_s(fd, " addr=0x");
    write_hex(fd, info ? (unsigned long)(uintptr_t)info->si_addr : 0UL);
    write_s(fd, " pid=");
    write_uint(fd, (unsigned long)getpid());
    write_s(fd, " tid=");
    write_uint(fd, (unsigned long)syscall(SYS_gettid));
    write_s(fd, "\n");

    /* Backtrace — up to 64 frames. backtrace + backtrace_symbols_fd are
     * documented async-signal-safe (glibc allocates internal buffers
     * lazily but uses mmap, not malloc, on the hot path). */
    void *frames[64];
    int n = backtrace(frames, 64);
    backtrace_symbols_fd(frames, n, fd);
    write_s(fd, "[fatal-signal] end\n");

    /* Restore default handler and re-raise so:
     *   - systemd still reports the original status code (e.g. 134),
     *   - the kernel still produces a core file if RLIMIT_CORE permits,
     *   - any parent process / debugger sees the real signal. */
    struct sigaction dfl;
    memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    sigaction(sig, &dfl, NULL);
    raise(sig);
}

int signal_handler_install(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = fatal_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);

    const int sigs[] = { SIGABRT, SIGSEGV, SIGBUS, SIGFPE };
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
        if (sigaction(sigs[i], &sa, NULL) != 0) return -1;
    }
    return 0;
}
