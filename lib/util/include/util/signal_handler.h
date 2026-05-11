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

#ifdef __cplusplus
extern "C" {
#endif

/* Install handlers for SIGABRT, SIGSEGV, SIGBUS, SIGFPE.
 * Idempotent. Returns 0 on success, -1 on sigaction failure. */
int signal_handler_install(void);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_SIGNAL_HANDLER_H */
