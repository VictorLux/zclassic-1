# Worker Assignment — Phase 6b: Postmortem capsule (crash → seed.cap.gz)

**Worktree:** wt2 OR wt3 (either)
**Branch:** PUSH DIRECT TO MAIN
**Phase:** 6 (Determinism + simulator)
**Depends on:** Phase 6a (seed_tape primitive) ✅ — required for the
capsule's tape contents. **Status: IN PROGRESS (wt2)** — claimed
2026-05-24 after Phase 6a landed on main.
**Plan reference:** [`docs/architecture/phase6-determinism-and-simulator.md`](../architecture/phase6-determinism-and-simulator.md) § 6b

**Owns:**
- NEW `lib/sim/include/sim/postmortem.h`
- NEW `lib/sim/src/postmortem.c`
- NEW `lib/test/src/test_postmortem.c`
- EDIT `lib/util/src/abort.c` (or wherever the SIGSEGV/SIGABRT handler lives)
- EDIT `lib/util/src/boot_phase.c` — install postmortem hook AFTER seed_tape is open
- EDIT `tools/mcp/controllers/ops_controller.c` — `zcl_postmortem_list` + `zcl_postmortem_replay` MCP tools
- EDIT `lib/test/src/test.c`, `lib/test/src/test_parallel.c`, `lib/test/include/test/test_helpers.h`

**MUST NOT touch:**
- `seed_tape.{c,h}` (Phase 6a primitive; pure consumer here)
- Wave S, Phase 3, Phase 4, Phase 5 code paths
- `docs/REFACTOR_STATUS.md`, `docs/FRAMEWORK.md`, `CLAUDE.md`

---

## Why this matters

Phase 6a shipped the seed_tape primitive — the ability to install a
deterministic RNG/clock source under a 64-bit seed. But by itself, the
tape doesn't help debugging: someone has to know to install it BEFORE
a bug occurs.

**Postmortem capsules close the loop.** On every node boot, the boot
sequence:
1. Generates a fresh 64-bit seed from `/dev/urandom`.
2. Opens a tape with that seed and installs it.
3. Registers a SIGSEGV / SIGABRT / SIGFPE handler that, on fault:
   - Serializes the active tape to `~/.zclassic-c23/postmortems/<unix>-<seed>.cap.gz`.
   - Includes: tape bytes, last 1000 events from event log, last 100
     log lines, register dump, /proc/self/status, build info.
   - Returns from the handler so the default behavior (core-dump)
     still produces a corefile.

Result: **every production crash produces a replayable capsule.** Pair
it with the corefile via the unix timestamp, attach a debugger,
replay the seed deterministically until the crash reproduces, fix it,
verify with the same seed.

This is the structural answer to the chronic 22-min SEGV wedge:
instead of guessing, we get a deterministic reproducer for each crash.

---

## Capsule format

`seed.cap.gz` is a gzip-compressed tarball with the following layout:

```
capsule/
├── manifest.json        { "version": 1, "seed": "0x...",
│                          "crash_signal": 11, "crash_unix": 1717...,
│                          "build_id": "...", "git_sha": "...",
│                          "tape_size_bytes": ..., "event_count": ... }
├── tape.bin             (seed_tape_save output — see 6a)
├── events.bin           (last 1000 events from event_log, in order)
├── log.txt              (last 100 lines from node.log)
├── procstatus.txt       (/proc/self/status at crash time)
└── coremarker.txt       (timestamp + advice: "look for corefile near systemd
                          journal at <crash_unix>")
```

Capsule size: target < 1 MB per crash on the median. The 1000-event
window is enough to reproduce the seconds-leading-up-to-crash window.

---

## API

```c
/* lib/sim/include/sim/postmortem.h */
#ifndef ZCL_SIM_POSTMORTEM_H
#define ZCL_SIM_POSTMORTEM_H

#include <stdbool.h>
#include <stdint.h>

struct seed_tape;

/* Install the postmortem handler. Catches SIGSEGV/SIGABRT/SIGFPE/SIGBUS
 * and writes a capsule to `dir` before chaining to the default handler.
 *
 * `dir` should typically be `<datadir>/postmortems/`. The handler
 * will create it on first crash if missing.
 *
 * `tape` is the currently-installed seed tape (typically the
 * boot-time one). The handler takes a non-owning reference; the
 * caller is responsible for keeping it alive for the process
 * lifetime.
 *
 * Returns 0 on success, -1 if signal handler install fails. */
int postmortem_install(struct seed_tape *tape, const char *dir);

/* Uninstall the handler (restores default). Mostly for tests. */
void postmortem_uninstall(void);

/* List existing capsules in `dir`. Returns -1 on error, otherwise
 * number of capsules found. Caller-allocated `out` is filled with at
 * most `out_cap` capsule descriptors in newest-first order. */
struct postmortem_summary {
    char    path[256];        /* absolute path to .cap.gz */
    int64_t crash_unix;
    uint64_t seed;
    int     crash_signal;
    size_t  capsule_bytes;
};
int postmortem_list(const char *dir,
                    struct postmortem_summary *out, size_t out_cap);

/* Replay a capsule: opens the tape, installs it, returns the tape so
 * the caller can step it forward / call test code under it. Caller
 * frees with seed_tape_close(). Returns NULL on parse failure. */
struct seed_tape *postmortem_load(const char *path);

#endif
```

---

## Tasks (in order)

### Task 1: Capsule writer (signal-safe core)

EDIT `lib/sim/src/postmortem.c`. The hard constraint: the signal
handler must be **async-signal-safe** — no malloc, no printf, no
locks except trywait, no unbounded loops. The strategy:

1. At install time, pre-allocate a fixed buffer for the capsule path
   (`<dir>/<unix>-<seed>.cap.gz`) and a 1 MB scratch buffer.
2. In the handler:
   - Append a marker event to the event log (`EV_POSTMORTEM_CAPTURED`).
   - Snapshot the tape into the scratch buffer via
     `seed_tape_save_to_memory()` (a new variant of `seed_tape_save`
     that writes to a caller-provided buffer instead of opening a file).
   - Open the capsule file via `open(O_CREAT|O_WRONLY|O_TRUNC)`.
   - Write a minimal uncompressed tarball using a tiny inline writer
     (no libarchive — too heavy). Gzip is OPTIONAL; if we can't do
     it async-signal-safely (zlib uses malloc internally), skip
     compression and accept ~3x larger capsules.
   - close(), then chain to the default signal handler (re-raise the
     signal with default disposition).
3. The boot handler at the next start can detect any uncompressed
   capsule and gzip-compress it (asynchronously, not in the signal
   path).

**Decision:** write capsules uncompressed in the signal handler. The
boot-time compressor handles gzip later. This is safer and simpler.

**Acceptance:** unit test installs the handler, raises SIGABRT via
`raise()`, verifies a capsule was written + the process aborts as
expected. (Use a child process via `fork()` so the test runner
survives.)

### Task 2: Capsule reader + replay

In `postmortem.c`, implement `postmortem_list` (scan directory,
parse filenames + minimal header) and `postmortem_load` (parse the
tarball, extract `tape.bin`, call `seed_tape_load_from_memory()`).

Add `seed_tape_save_to_memory()` and `seed_tape_load_from_memory()`
to `lib/sim/src/seed_tape.c` — small variants of the existing file
I/O. Both are needed: the capsule wants to write the tape into the
tarball buffer (not a separate file), and the replay wants to load
from a buffer (not a separate file).

**Acceptance:** round-trip test — install handler in child, crash
child, parent reads capsule, replays tape, verifies first 100 RNG
values match what the child would have produced.

### Task 3: Boot integration

EDIT `lib/util/src/boot_phase.c`. After the seed_tape is opened (Phase
6a hook), call `postmortem_install(tape, <datadir>/postmortems/)`.

Boot-time async tasks (run on a background thread):
- Compress any uncompressed capsules from prior crashes.
- Prune capsules older than 30 days (keep last 100 most recent).

EDIT `config/src/boot_services.c` to wire the boot init + cleanup
hooks.

**Acceptance:** boot a fresh node with the handler installed, kill -SEGV
the process, restart, verify `postmortem_list` reports the prior
crash capsule.

### Task 4: MCP tools

EDIT `tools/mcp/controllers/ops_controller.c` to add:

- `zcl_postmortem_list` — returns JSON array of capsule summaries.
- `zcl_postmortem_replay` — takes a capsule path, opens it, returns
  the first 1000 events from the tape's recorded event stream as
  JSON. Useful for "what happened in the last seconds before crash"
  without firing up a debugger.

Both tools are read-only; no `destructive` flag.

**Acceptance:** `zcl_postmortem_list` returns the current capsule
inventory. `zcl_postmortem_replay` returns the event sequence.

**Progress 2026-05-25:** implemented both read-only ops tools against
the public `postmortem_list`/`postmortem_load` APIs. Added an MCP
controller dispatch test that writes a real capsule, verifies inventory
JSON, and replays the recorded injected event stream.

### Task 5: Test suite

EDIT `lib/test/src/test_postmortem.c`. Test cases:

1. `install_uninstall` — install, uninstall, no leaks; default
   signal handler restored.
2. `handler_writes_capsule` — fork child, install in child, raise
   SIGABRT, parent reads the capsule file, verifies structure.
3. `capsule_contains_tape` — verify the `tape.bin` member is
   present and decodable.
4. `list_finds_capsules` — write 3 fake capsules, list returns 3
   in newest-first order.
5. `load_replays_correctly` — capsule writer + reader round-trip
   produces identical RNG values.
6. `corrupt_capsule_rejected` — flip a byte in the tarball, load
   returns NULL.

Add `failures += test_postmortem();` to `test.c` + `test_parallel.c`
TEST_LIST + helpers header.

**Acceptance:** `./test_parallel --jobs=$(nproc)` PASS, including
`postmortem: 6 passed, 0 failed`.

### Task 6: Final verify + push

```bash
make -j$(nproc)
make lint
./test_parallel --jobs=$(nproc)
git pull --rebase origin main
git push origin main
```

Append Completion section.

---

## Live verification (post-merge)

After this PR ships, the orchestrator:

1. Lets the production node run normally.
2. After the next SEGV (~22 min based on current cadence), calls
   `zcl_postmortem_list` — should report 1 capsule.
3. Calls `zcl_postmortem_replay` on it — should return the event
   stream from the seconds before the crash.
4. If both work: phase 6 is real and we have a path to fix the
   chronic wedge deterministically.

---

## What this does NOT do

- Does NOT run a chaos harness in CI (6c).
- Does NOT auto-replay capsules under a debugger (operator does
  that manually; future tooling may automate).
- Does NOT capture every source of non-determinism (file I/O timing,
  thread scheduling — Phase 6c may revisit).
- Does NOT alter the existing core-dump path. Crashes still produce
  corefiles via systemd-coredump; the capsule is additive context.

---

## Risk + rollback

The postmortem handler runs INSIDE the SIGSEGV path. A bug in the
handler turns "crash + capsule" into "crash + worse crash" (handler
faults during fault). Mitigations:

- Pre-allocate ALL buffers at install time.
- Use only async-signal-safe syscalls (open, write, close, _exit).
- The handler's last act is to chain to the default disposition —
  if our handler dies, the kernel's default takes over and a corefile
  is still produced.
- Test with `kill -SEGV $(pidof zclassic23)` BEFORE shipping.

Rollback: revert the PR. Existing capsules become orphaned but
harmless (the prune step at next boot cleans them up after 30 days).

---

## Commit cadence

One commit per task. Push after tasks 2, 3, 5.

---

## Status

**IN PROGRESS (wt2)** — claimed 2026-05-24 after Phase 6a
`seed_tape_save` / `seed_tape_load` landed on main. Current slice is
the capsule API plus the non-signal save/list/load path.

### Progress (wt2, 2026-05-24)

- Added `sim/postmortem.h` and `lib/sim/src/postmortem.c`.
- Current primitive writes an unpacked `.cap` directory containing
  `manifest.json`, `tape.bin`, `procstatus.txt`, `log.txt`, and
  `coremarker.txt`. This keeps the durable capsule contract testable before
  the async-signal-safe install path is wired; `.cap.gz` packaging remains for
  the signal/boot integration slice.
- Added focused `test_postmortem` coverage for capture, manifest fields,
  log-tail copy, list, tape load, corruption rejection, and bad argument
  handling.
- Added the Task 2 seed tape memory codec
  (`seed_tape_save_to_memory` / `seed_tape_load_from_memory`) and routed
  postmortem tape capture/replay through it. The current implementation still
  writes unpacked `.cap` directories, but the tape member now uses the same
  caller-owned buffer path needed by the future signal-safe capsule writer.
- Tightened capsule listing for the reader/MCP path: `.cap` directory scans now
  return newest-first entries, keep the newest entries when the caller's buffer
  is smaller than the inventory, and parse manifest signal/tape-size summary
  fields.
- Added the public `postmortem_list` / `postmortem_load` API surface expected by
  the assignment, backed by the current unpacked capsule reader and summary
  byte accounting. Existing `postmortem_capsule_*` helpers remain as lower-level
  compatibility wrappers.

### Progress (wt2, 2026-05-25)

- Added read-only MCP access to the postmortem reader path:
  `zcl_postmortem_list` summarizes capsule inventories and
  `zcl_postmortem_replay` returns seed-tape events with hex payloads for quick
  operator inspection. The remaining Phase 6b work is the signal handler and
  boot integration path.
- Added a fatal-signal crash-hook bridge plus `postmortem_install` /
  `postmortem_uninstall`; forked test coverage now proves a child that raises
  `SIGABRT` leaves a listed, replayable capsule before terminating. Boot-time
  seed-tape creation and install wiring remain open.
- Added boot-time postmortem wiring after datadir lock: each boot now owns a
  seed tape for crash capture and registers capsules under
  `<datadir>/postmortems`, with shutdown cleanup and forked test coverage.
  Production platform RNG/clock takeover remains deferred until the simulator
  clock advancement path is ready.
- Added boot-time postmortem retention: unpacked `.cap` directories are pruned
  by age and newest-count limit during postmortem setup. Compression of
  unpacked capsules remains the remaining boot maintenance item.
- Added boot-time postmortem compression: prior unpacked `.cap` directories are
  archived as `.cap.gz` before retention pruning, and the reader/list/load
  paths now understand both unpacked and compressed capsules. Remaining work is
  deeper crash-path hardening and live production SEGV verification.
- Added restart-path coverage for the production shape: a boot-installed child
  raises `SIGSEGV`, the parent verifies the prior unpacked capsule is visible,
  and a second boot compresses it to `.cap.gz` while preserving replayability.
- Hardened the fatal-signal crash hook so it no longer calls the general
  stdio/allocation capsule writer from the signal path. `postmortem_install`
  now preallocates a fixed tape scratch buffer, and the hook writes the minimal
  unpacked capsule (`manifest.json`, `tape.bin`, empty log/proc placeholders,
  and `coremarker.txt`) with raw `mkdir`/`open`/`write`/`close` syscalls before
  the existing fatal handler re-raises. Remaining work is live production SEGV
  verification and any follow-up needed from that evidence.
- Hardened `postmortem_install` to create and validate the capsule directory
  before registering the fatal-signal hook, with focused coverage for
  auto-created directories and non-directory rejection.
- Filled in the signal-path `procstatus.txt` member using a bounded
  `/proc/self/status` copy through raw `open`/`read`/`write`/`close`, with
  forked SIGABRT/SIGSEGV coverage proving the captured capsule contains the
  process status payload.

<!-- Worker: append a Completion section below when done. -->
