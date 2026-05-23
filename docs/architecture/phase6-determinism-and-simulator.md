# Phase 6 — Determinism + simulator (replay any bug from a seed)

**Status:** PLAN (draft 2026-05-23)
**Phase:** 6 (after Phase 4 storage unification + Phase 5 crypto registry)
**Estimated scope:** 3 sub-phases, ~2,000 LOC added (simulator + chaos harness)

> "Every bug becomes a 64-bit seed."

---

## What this delivers

A bug in production is a horrible thing to chase. Today: read logs,
guess what happened, try to reproduce locally with synthetic inputs,
fail, ship a patch that might not actually fix the cause.

After Phase 6: the production node emits a **seed** with every crash
or anomaly. Feeding the seed to the simulator deterministically
replays the exact sequence of inputs (RNG values, clock advances, peer
messages, disk events) that produced the bug. Step through with a
debugger. Fix the bug. Verify by re-running the seed.

This is the deepest payoff of the framework refactor:
- **Phase 1c** made the clock deterministic (`platform.clock` adopted everywhere).
- **Phase 1c** made the RNG deterministic (`platform.rng` adopted everywhere).
- **Phase 4** made the storage deterministic (one event log, replayable).
- **Phase 6** ties them together with a simulator harness.

---

## Sub-phases

### 6a — Deterministic seed-based input tape

NEW: `lib/sim/include/sim/seed_tape.h` + `seed_tape.c`.

```c
typedef struct seed_tape seed_tape_t;

/* Open a new tape with a 64-bit seed. The seed deterministically
 * produces all subsequent RNG values + clock values. */
seed_tape_t *seed_tape_open(uint64_t seed);

/* Hook: replace platform.rng + platform.clock with the tape's values.
 * After install, every platform_rng_u64() comes from the tape;
 * every platform_clock_monotonic_us() returns the tape's simulated time. */
void seed_tape_install(seed_tape_t *tape);

/* Advance simulated time. The tape records this; replay reproduces it. */
void seed_tape_advance_clock(int64_t microseconds);

/* Inject a synthetic external event (peer message, disk write completion,
 * timer fire). The tape records it. */
void seed_tape_inject_event(enum sim_event_type type,
                            const void *payload, size_t len);

/* Serialize the tape to a file so a bug from the field becomes
 * "send us the tape file, we'll replay." */
int seed_tape_save(seed_tape_t *tape, const char *path);

/* Load a tape and replay it from start. */
seed_tape_t *seed_tape_load(const char *path);
```

After 6a: tests can use `seed_tape_open(0xdeadbeef)` and get the same
result every run. Flaky tests become impossible because the only
non-determinism (clock, rng) is under the tape's control.

### 6b — Crash → seed dump

Production node, when a wedge or panic happens:
1. Save the current seed tape's tail (last N events).
2. Save the relevant projection state hashes.
3. Save the event log offset.
4. Bundle as a "post-mortem capsule."

NEW: `lib/sim/include/sim/postmortem.h`.

```c
struct postmortem_capsule {
    uint64_t seed;
    int64_t  wall_unix;
    int64_t  monotonic_us;
    int64_t  event_log_offset;
    uint8_t  projections_fingerprint[32];
    char     trigger_reason[256];
    /* The seed tape's tail — bounded size, e.g., last 10000 events. */
    void    *tape_tail;
    size_t   tape_tail_len;
};

int postmortem_capture(struct postmortem_capsule *out, const char *trigger);
int postmortem_save(const struct postmortem_capsule *cap, const char *path);
int postmortem_load(struct postmortem_capsule *out, const char *path);
```

Wire into:
- `condition_engine`'s EV_OPERATOR_NEEDED emit (capture postmortem)
- Crash handler (capture on SIGSEGV/SIGABRT)
- Optional manual capture via MCP tool `zcl_capture_postmortem`

### 6c — Simulator harness + chaos in CI

NEW: `tools/sim/replay.c` — single binary that loads a postmortem
capsule and re-runs the node deterministically up to the failure
point. Drop-in debugger target.

NEW: `tools/sim/chaos.c` — continuous random-seed driver. Picks a
seed, injects deterministic chaos (random peer messages, random
clock skews, random disk delays). Runs the node for N simulated
hours. On a crash, saves the postmortem capsule for the failing
seed. CI runs this nightly.

After 6c lands: any new bug shows up in chaos with a seed attached
within hours, NOT after a customer reports it weeks later.

---

## Acceptance gates

- 6a: 100 unit tests with `seed_tape_open(N)` produce identical
  results across 10 runs each.
- 6b: synthetic panic in a test produces a valid capsule;
  `postmortem_load` round-trips it.
- 6c: CI chaos run discovers at least one historical bug
  (synthetic — inject a known race condition; chaos must find it
  via random seeds within 24h simulated time).

---

## Risk + mitigations

- **Performance overhead of recording the tape.** Mitigation: the
  tape is bounded (last 10000 events) and ring-buffered; recording
  is one struct write per RNG/clock call. Benchmarked in 6a.
- **Tape size for long-running nodes.** Mitigation: tail-only, not
  full history. Full history would need event log integration
  (Phase 4).
- **Non-determinism creeps in via subprocess (Tor, etc.).** Mitigation:
  Phase 6 only claims determinism for IN-PROCESS code. External
  subsystems (Tor) are treated as injected events from the tape's
  perspective.

---

## What this unlocks

- **MTBF measurement that means something.** Today's "5.5 day MTBF"
  number is unverifiable. With chaos, we can compute MTBF as
  "average simulated time to first crash across N seeds."
- **Regression tests for any bug.** A bug fix isn't done until a
  test using the bug's seed reproduces the crash on the OLD code
  and passes on the new code.
- **Multi-node simulation.** With seed tapes, we can simulate a
  network of N zclassic23 nodes consensus-converging from random
  starting states. Catches Byzantine bugs that single-node tests
  can't.

---

## Status

DRAFT — actionable after Phase 4 (storage unification) ships.
Phase 4 provides the event log that Phase 6 builds on.
