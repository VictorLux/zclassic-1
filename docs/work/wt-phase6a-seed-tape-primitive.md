# Worker Assignment — Phase 6a: seed_tape primitive

**Worktree:** wt2 OR wt3 (either)
**Branch:** PUSH DIRECT TO MAIN
**Phase:** 6 (Determinism + simulator)
**Depends on:** Phase 1c (platform.clock + platform.rng adopted)
✅ already done. Independent of Phase 4 — but Phase 6b/6c are more
powerful once event log is the durable input source.
**Plan reference:** [`docs/architecture/phase6-determinism-and-simulator.md`](../architecture/phase6-determinism-and-simulator.md) § 6a

**Owns:**
- NEW `lib/sim/include/sim/seed_tape.h`
- NEW `lib/sim/src/seed_tape.c`
- NEW `lib/test/src/test_seed_tape.c`
- EDIT `lib/platform/include/platform/rng.h` — add the **install-hook** API (`platform_rng_set_source`) so tape can intercept
- EDIT `lib/platform/src/rng.c` — wire the install-hook (default behavior unchanged)
- EDIT `lib/platform/include/platform/clock.h` — add `platform_clock_set_source`
- EDIT `lib/platform/src/clock.c` — wire the install-hook (default behavior unchanged)
- EDIT `lib/test/src/test.c`, `lib/test/src/test_parallel.c`, `lib/test/include/test/test_helpers.h`
- EDIT build system (Makefile picks up new sources via wildcard)

**MUST NOT touch:**
- Any existing rng/clock CALLER (they all use `platform_rng_*` and `platform_clock_*` — the install-hook is invisible to them)
- Wave S, Phase 4, Phase 5 code paths
- `docs/REFACTOR_STATUS.md`, `docs/FRAMEWORK.md`, `CLAUDE.md`

---

## Why this matters

Phase 6 is the deepest payoff of the refactor: **every bug becomes a
64-bit seed**. Production node crashes → emit seed → replay
deterministically with debugger attached → fix → verify with same
seed. This eliminates the "can't reproduce" class of bugs entirely.

But Phase 6 only works if all non-determinism in the node is under
seed control. Phase 1c made clock + rng pluggable (every caller goes
through `platform_clock_*` / `platform_rng_*`). What's still missing:
a way to **install** a tape-driven source for both, so a simulated
run reads from the tape instead of system time / `/dev/urandom`.

This PR ships the smallest possible piece: the tape primitive itself
+ the install hooks. NO callers get rewired (they don't need to —
the hooks intercept transparently). NO simulator harness yet (Phase 6c).

After this lands:
- `seed_tape_open(0xdeadbeef)` + `seed_tape_install()` makes every
  `platform_rng_u64()` deterministic.
- Existing tests can opt into determinism by calling these in setup.
- Phase 6b (postmortem capsules) and Phase 6c (simulator harness)
  build on top.

---

## API

```c
/* lib/sim/include/sim/seed_tape.h */
#ifndef ZCL_SIM_SEED_TAPE_H
#define ZCL_SIM_SEED_TAPE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct seed_tape seed_tape_t;

/* Open a fresh tape seeded with `seed`. The tape derives:
 *   - A deterministic RNG stream (xoshiro256++ seeded from `seed`).
 *   - A simulated wall clock starting at `start_wall_unix` (or 0 if
 *     caller passes 0).
 *   - A simulated monotonic clock starting at 0.
 *
 * Returns NULL on allocation failure. */
seed_tape_t *seed_tape_open(uint64_t seed, int64_t start_wall_unix);

/* Free the tape. After this, install_hooks should have been removed
 * via seed_tape_uninstall(); otherwise platform_rng/clock will be
 * pointing at freed memory. */
void seed_tape_close(seed_tape_t *tape);

/* Install this tape as the active source for platform.rng + platform.clock.
 * After this, every subsequent platform_rng_u64() comes from the tape,
 * and every platform_clock_monotonic_us() / platform_clock_wall_unix()
 * returns the tape's simulated time.
 *
 * Only ONE tape can be installed at a time. Re-installing overwrites
 * the previous. */
void seed_tape_install(seed_tape_t *tape);

/* Remove the tape; restore the system rng/clock sources. */
void seed_tape_uninstall(void);

/* Advance the tape's simulated monotonic clock by `microseconds`.
 * Wall clock advances by the same delta in microseconds (as if no
 * time skew). Tape records the advance for replay. */
void seed_tape_advance(seed_tape_t *tape, int64_t microseconds);

/* Inject a simulated external event (e.g., synthetic peer message).
 * The tape records the (type, payload) pair so replay reproduces it.
 *
 * Free-form payload — caller defines the type taxonomy. Common types
 * (0..127 reserved): 1=PEER_MESSAGE, 2=BLOCK_RECEIVED, 3=DISK_DELAY,
 * 4=TIMER_FIRE. Caller types 128..255 are application-defined. */
int seed_tape_inject(seed_tape_t *tape, uint8_t type,
                     const void *payload, size_t len);

/* Counters (for introspection) */
uint64_t seed_tape_rng_count(const seed_tape_t *tape);
uint64_t seed_tape_clock_advance_count(const seed_tape_t *tape);
uint64_t seed_tape_inject_count(const seed_tape_t *tape);

/* Tape size in bytes (useful for postmortem capsules in 6b). */
size_t seed_tape_size_bytes(const seed_tape_t *tape);

/* Serialize the tape to a file. The format is versioned + self-describing
 * (magic + version + checksum) so future readers can detect tape format
 * drift. */
int seed_tape_save(const seed_tape_t *tape, const char *path);

/* Load a tape from disk. The returned tape is in "replay mode" — its
 * RNG and clock advance through the recorded values; calls to
 * seed_tape_advance / seed_tape_inject FAIL (the recording is read-only).
 */
seed_tape_t *seed_tape_load(const char *path);

/* In replay mode, returns the next injected event (if any). Caller's
 * simulator dispatches it to the appropriate handler. */
int seed_tape_next_event(seed_tape_t *tape,
                         uint8_t *type_out, void *payload_out,
                         size_t payload_cap, size_t *payload_len_out);

#endif
```

---

## Install-hook API on platform.{rng,clock}

The hook is invisible to existing callers. They keep calling
`platform_rng_u64()` and `platform_clock_monotonic_us()`. Internally
those functions check an `_Atomic` source pointer; if set (by
seed_tape_install), they call the source's vtable; otherwise they
fall through to the default implementation.

```c
/* lib/platform/include/platform/rng.h additions */
struct platform_rng_source {
    uint64_t (*u64)(void *user);
    void *user;
};

void platform_rng_set_source(struct platform_rng_source *src);
void platform_rng_clear_source(void);  /* restore default */

/* lib/platform/include/platform/clock.h additions */
struct platform_clock_source {
    int64_t (*monotonic_us)(void *user);
    int64_t (*wall_unix)(void *user);
    void *user;
};

void platform_clock_set_source(struct platform_clock_source *src);
void platform_clock_clear_source(void);
```

The default (when no source installed) is the existing `clock_gettime`
+ `getrandom` paths. Zero overhead in production (one atomic_load +
predictable branch).

---

## Tasks (in order)

### Task 1: Add install-hook API to platform.rng

EDIT `lib/platform/include/platform/rng.h` — add `struct
platform_rng_source` + `set_source` / `clear_source` declarations.

EDIT `lib/platform/src/rng.c` — internally store an
`_Atomic(struct platform_rng_source *)` pointer. `platform_rng_u64`
checks it; if non-NULL, calls `src->u64(src->user)`; otherwise calls
the existing impl.

**Acceptance:** existing rng tests pass (default behavior unchanged).
NEW unit test: install a constant-returning source, verify
`platform_rng_u64()` returns the constant.

### Task 2: Add install-hook API to platform.clock

Same pattern as Task 1, for clock. Two methods on the source vtable
(`monotonic_us`, `wall_unix`).

**Acceptance:** existing clock tests pass. NEW unit test: install a
source that returns `42`, verify both clock methods return `42`.

### Task 3: seed_tape.h + skeleton .c

Per the API above. Use xoshiro256++ for the deterministic RNG (PD impl,
~30 LOC, no deps). Use a simple `_Atomic int64_t` for simulated clocks
(thread-safe under tape install + concurrent reads).

The tape stores a linked list of "actions" (advance, inject events) in
order. Recording mode appends; replay mode pops in order.

**Acceptance:** open + close cleanly. `seed_tape_open(0)` gives a
predictable first RNG draw matching xoshiro256++ test vectors.

### Task 4: install / uninstall + RNG hook integration

`seed_tape_install` builds a `platform_rng_source` whose `u64` callback
pulls the next value from the tape's xoshiro state. Same for clock.

`seed_tape_uninstall` clears both sources.

**Acceptance:** after install, `platform_rng_u64()` returns tape values
(verify against direct xoshiro output). After uninstall, returns
system values.

### Task 5: advance + inject + counters

`seed_tape_advance` bumps the simulated clock, appends an advance
record. `seed_tape_inject` appends an event record. Counters
increment.

**Acceptance:** call advance(1_000_000), verify monotonic clock
returns +1_000_000. Inject 3 events, verify `inject_count() == 3`.

### Task 6: save + load + replay

Serialize: 16-byte header (magic `ZCLTAPE!` + version + flags) +
seed (8B) + start_wall_unix (8B) + action_count (8B) + action records.

Each action record: 1B type (advance=1, inject=2), then type-specific
data. Length-prefixed for inject payloads.

Add a CRC32C at the end so corruption is detectable.

Load: parse header, deserialize actions into linked list, set tape
to replay mode.

`seed_tape_next_event` pops from the action list (filtering for
inject types); recording-mode operations on a loaded tape return
`EROFS`.

**Acceptance:** record a tape (5 advances + 3 injects), save, load,
verify the loaded tape produces the same RNG sequence + the same
3 events in order.

### Task 7: test_seed_tape.c

Test cases:
1. **`open_close_clean`** — open, close, no leaks (run under
   AddressSanitizer if available; otherwise just visually verify).
2. **`rng_deterministic`** — two tapes with same seed produce same
   first 1000 u64 values.
3. **`rng_different_seeds_diverge`** — two tapes with different
   seeds produce different sequences within first 10 values.
4. **`install_hooks_rng`** — install + verify platform_rng_u64
   returns tape value; uninstall + verify returns system value
   (or at least, NOT the tape value).
5. **`install_hooks_clock`** — install + verify both clock methods
   return tape values.
6. **`advance_clock`** — call advance(N) twice, verify clock = 2N.
7. **`inject_event`** — inject 3 events, verify inject_count.
8. **`save_load_roundtrip`** — record 10-action tape, save to a
   temp file, load, verify identical replay.
9. **`replay_rejects_writes`** — loaded tape rejects advance/inject
   with an error.
10. **`corruption_detected`** — save tape, flip a byte, load,
    verify load returns NULL (CRC mismatch detected).

Add `failures += test_seed_tape();` to `test.c` + declaration to
`test_helpers.h` + `seed_tape` entry to TEST_LIST in
`test_parallel.c`.

**Acceptance:** `./test_parallel --jobs=$(nproc)` PASSES with
`seed_tape: 10 passed, 0 failed` in the output.

### Task 8: Final verify + push

```bash
make -j$(nproc)
make lint
./test_parallel --jobs=$(nproc)
git pull --rebase origin main
git push origin main
```

Append Completion section.

---

## What this does NOT do

- Does NOT capture a postmortem on crash (6b).
- Does NOT run a chaos harness in CI (6c).
- Does NOT change any existing test's behavior — tests that don't
  opt-in to the tape see the same flaky non-determinism they always did.
- Does NOT eliminate any source of non-determinism the rng/clock
  abstractions don't cover (e.g., file I/O timing, thread scheduling).
  Phase 6c may revisit if needed.

This is the FOUNDATION. The next 2 PRs (6b + 6c) build the production
crash → seed → replay loop.

---

## Commit cadence

One commit per task. Push after tasks 2, 4, 7.

---

## Status

**READY** — Phase 1c adoption already complete (`feedback_use_test_parallel`
captures the 167-file rewire). No conflicts with Phase 2/3/4/5 work.

Any worker may claim by marking IN PROGRESS (wt<N>).

<!-- Worker: append a Completion section below when done. -->
