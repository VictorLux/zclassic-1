/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Injectable clock interface (Wave F-5a).
 *
 * Why:
 *   Half the bugs in a node like this hide behind "real time". The
 *   watchdog stall window, the IBD lag SLO, retry backoffs, fee-rate
 *   smoothing — all read `clock_gettime` directly. Once a test or the
 *   deterministic simulator (Wave T) wants to fast-forward, every one
 *   of those call sites has to be touched.
 *
 *   This header offers a one-pointer abstraction: production resolves
 *   to a static vtable that wraps `clock_gettime` (the same syscalls
 *   today's call sites use), and tests/simulator can install a virtual
 *   clock with `clock_set_default(...)`.
 *
 * Scope of Wave F-5a:
 *   - Add the abstraction alongside today's direct `clock_gettime`
 *     callers. Do NOT rewire existing call sites; rewiring is a later
 *     campaign (Wave T).
 *   - The simulator wires its own `clock_iface_t` whose `now_*` cell
 *     reads from a virtual clock advanced by the scheduler.
 *
 * Thread safety:
 *   `clock_set_default` / `clock_reset_default` are atomic pointer
 *   swaps, so callers can swap the default from a test thread while
 *   another thread reads `clock_now_monotonic_ns()`. Production code
 *   never calls the swappers.
 */

#ifndef ZCL_PLATFORM_CLOCK_H
#define ZCL_PLATFORM_CLOCK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct clock_iface clock_iface_t;

struct clock_iface {
    int64_t (*now_monotonic_ns)(void *self);
    int64_t (*now_wall_ms)(void *self);
    void    *self;
};

/* Returns the process default vtable. Always non-NULL.
 *
 * The default wraps real syscalls (`clock_gettime(CLOCK_MONOTONIC,...)`
 * + `clock_gettime(CLOCK_REALTIME,...)`). The pointer itself may be
 * swapped by `clock_set_default`; this returns whatever is current. */
const clock_iface_t *clock_default(void);

/* Convenience readers — equivalent to
 *   clock_default()->now_monotonic_ns(clock_default()->self).
 * Use these for one-shot reads; cache `clock_default()` in a local
 * only when you genuinely call it in a hot inner loop. */
int64_t clock_now_monotonic_ns(void);
int64_t clock_now_wall_ms(void);

/* Swap the process-wide default. Intended for tests and the
 * deterministic simulator only — production code never calls this.
 *
 * `iface` must outlive every concurrent reader (typically static
 * storage). Passing NULL is rejected (no-op). */
void clock_set_default(const clock_iface_t *iface);

/* Restore the real-syscall default. Safe to call any number of times. */
void clock_reset_default(void);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_PLATFORM_CLOCK_H */
