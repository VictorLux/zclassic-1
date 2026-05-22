/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Injectable RNG interface (Wave F-5b).
 *
 * Why:
 *   Determinism. Today, every part of the node that wants randomness
 *   reaches for `getrandom(2)` (or worse, `random()` / `time(NULL)`).
 *   That makes the future deterministic simulator (Wave T) impossible
 *   to seed — same simulation seed, different protocol-level outcomes,
 *   no way to bisect a failure.
 *
 *   This header offers a one-pointer abstraction: production resolves
 *   to a static vtable that wraps `getrandom(2)` (with `/dev/urandom`
 *   as a fallback for ancient kernels). Tests and the simulator can
 *   install an injected RNG with `rng_set_default(...)`. Same call
 *   sites, deterministic behavior under a seeded harness.
 *
 *   Crucially, the production path is unchanged — `rng_fill` still
 *   reads from the kernel CSPRNG. The abstraction has zero security
 *   cost; it only adds an indirect call.
 *
 * Scope of Wave F-5b:
 *   - Add the abstraction alongside today's direct getrandom callers.
 *   - Do NOT rewire existing call sites; rewiring is Wave T.
 *
 * Thread safety:
 *   `rng_set_default` / `rng_reset_default` are atomic pointer swaps.
 *   The default vtable is reentrant — `getrandom(2)` is thread-safe.
 *   A test-injected RNG must be reentrant too if multiple threads use
 *   `rng_fill` concurrently.
 */

#ifndef ZCL_PLATFORM_RNG_H
#define ZCL_PLATFORM_RNG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rng_iface rng_iface_t;

struct rng_iface {
    /* Fill `len` bytes of `out` with random data.
     * Returns true on success, false on hard failure (entropy source
     * unavailable). Production code should treat failure as fatal —
     * a node that cannot produce random keys must not run. */
    bool (*fill)(void *self, uint8_t *out, size_t len);
    void *self;
};

/* Returns the process default vtable. Always non-NULL. */
const rng_iface_t *rng_default(void);

/* Convenience: fill out[0..len) using the default. */
bool rng_fill(uint8_t *out, size_t len);

/* Return a uniformly random uint64. Aborts via LOG_FAIL on
 * underlying entropy failure (this should never happen on a Linux
 * system with a functioning kernel CSPRNG). Use when the caller
 * cannot tolerate a NULL/error return — key generation, nonces,
 * etc. */
uint64_t rng_u64(void);

/* Uniform sample in [lo, hi). Uses rejection sampling so the result
 * is unbiased even when (hi - lo) is not a power of two. If lo >= hi
 * the call returns lo (degenerate range — caller bug). */
uint32_t rng_u32_range(uint32_t lo, uint32_t hi);

/* Swap the process-wide default. Intended for tests and the
 * deterministic simulator only — production code never calls this.
 *
 * `iface` must outlive every concurrent reader (typically static
 * storage). Passing NULL is rejected (no-op). */
void rng_set_default(const rng_iface_t *iface);

/* Restore the real-syscall default. Safe to call any number of times. */
void rng_reset_default(void);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_PLATFORM_RNG_H */
