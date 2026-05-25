/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * signal_handler — capture fatal signals (SIGABRT/SIGSEGV/SIGBUS/SIGFPE)
 * with an async-signal-safe backtrace.
 *
 * Without this, a SIGABRT exits the process with status=134 and nothing
 * in node.log explains where the abort came from — every crash becomes
 * a fresh archaeology task. The handler:
 *
 *   1. Writes one `[fatal-signal]` marker line to stderr (which systemd
 *      routes to ~/.zclassic-c23/node.log).
 *   2. Writes a `backtrace_symbols_fd` dump of up to 64 frames.
 *   3. Restores the default handler and re-raises so systemd still
 *      sees the original exit status and (if LimitCORE permits) the
 *      kernel still drops a core file.
 *
 * Must be installed BEFORE any pthread is spawned so the handler is
 * inherited by all threads (sigaction defaults are process-wide).
 *
 * Async-signal-safe only: no printf, no malloc, no mutex. Build with
 * -rdynamic (already in our LDFLAGS) so backtrace_symbols resolves
 * function names rather than just addresses. */

#ifndef ZCL_SIGNAL_HANDLER_H
#define ZCL_SIGNAL_HANDLER_H

#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*signal_handler_crash_hook_fn)(int sig,
                                             siginfo_t *info,
                                             void *ucontext,
                                             void *ctx);

/* Install handlers for SIGABRT, SIGSEGV, SIGBUS, SIGFPE.
 * Idempotent. Returns 0 on success, -1 on sigaction failure. */
int signal_handler_install(void);

/* Register one best-effort callback to run before the fatal handler
 * emits diagnostics or re-raises. The callback executes inside the
 * signal path, so it must avoid locks and other unsafe process-global
 * state. */
void signal_handler_set_crash_hook(signal_handler_crash_hook_fn fn,
                                   void *ctx);
void signal_handler_clear_crash_hook(void);

/* Shared hook entry point for other fatal handlers that own the active
 * sigaction chain, such as the event-log crash dumper. */
void signal_handler_run_crash_hook(int sig, siginfo_t *info, void *ucontext);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_SIGNAL_HANDLER_H */
