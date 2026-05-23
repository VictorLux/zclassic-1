# wt2 Assignment — Phase 1a: Mailbox Adoption (header_probe → header_admit)

**Worktree:** `~/github/zclassic23-2`
**Branch:** `wt2/phase1-mailbox-adoption`
**Phase:** 1 (adopt unused primitives)
**Depends on:** Phase 0 (merged into main as of `4e0ea3382`)

**Owns (no other worker may touch):**
- `lib/framework/include/framework/mailbox.h` (new — thin re-export of `lib/util/include/util/mailbox.h` with typed-message macros)
- `app/services/include/services/header_admit_inbox.h` (new — public mailbox API for header_admit)
- Edits to `app/services/src/header_admit_stage.c` (add mailbox consumption path)
- Edits to `app/services/src/header_probe_service.c` (replace direct call to header_admit with mailbox push)
- New unit test: `lib/test/src/test_mailbox_adoption.c`
- Edits to `lib/test/src/test.c` + `lib/test/include/test/test_helpers.h` (test wiring)
- Edits to `config/src/boot_services.c` (one-line init of the inbox, only if needed)

**MUST NOT touch:**
- `app/jobs/`, `app/conditions/`, `app/supervisors/` (wt3 owns Phase 1c)
- `tools/lint/` (wt3 owns)
- `lib/util/src/mailbox.c` (the kernel primitive — don't modify; only adopt)
- `docs/REFACTOR_STATUS.md` (orchestrator only)
- `docs/FRAMEWORK.md`
- `CLAUDE.md`

---

## Why this matters

Mailbox (F-3, `lib/util/include/util/mailbox.h`, ~483 LOC) shipped 2026-05-22
with ZERO production callers. Phase 1 forces adoption of the four
shipped-but-unused kernel primitives (mailbox, projection, platform.clock,
platform.rng) so they don't bitrot.

This assignment wires **one** real actor-to-actor handoff via mailbox:
`header_probe_service` (peer-facing) discovers a new header → pushes it
to `header_admit_stage`'s inbox instead of calling directly. This:

1. Proves the mailbox primitive works in production.
2. Decouples header probing from admission (different threads).
3. Establishes the pattern for the other actor handoffs in Phases 2-3.

---

## Architecture reference

- [`docs/FRAMEWORK.md`](../FRAMEWORK.md) § 1 (the two pipelines) — note
  that mailbox is the "Communication between actors" primitive listed there.
- [`docs/FRAMEWORK.md`](../FRAMEWORK.md) § 3.4 (Job shape) — header_admit
  IS a Job; this assignment makes it consume from a mailbox instead of
  being polled by a callback.
- Existing canonical mailbox primitive: `lib/util/include/util/mailbox.h`,
  implementation `lib/util/src/mailbox.c`. Bounded MPSC ring with
  drop-policy on overflow.
- Existing canonical stage adopter: `app/services/src/header_admit_stage.c`.
  Read this file to understand the current shape before changing.
- Existing header probe: `app/services/src/header_probe_service.c`. Find
  where it calls into header_admit today (likely via direct function call
  or event emission).

---

## Tasks (in order)

### Task 1: `lib/framework/include/framework/mailbox.h`

Thin re-export wrapper over `util/mailbox.h`. Define:

```c
/* lib/framework/include/framework/mailbox.h
 *
 * Framework mailbox — thin re-export of util/mailbox with typed-message
 * macros for in-tree adopters.  See docs/FRAMEWORK.md § 1.
 */
#ifndef ZCL_FRAMEWORK_MAILBOX_H
#define ZCL_FRAMEWORK_MAILBOX_H

#include "util/mailbox.h"

/* MAILBOX_DECLARE(name, T)
 *   declares a typed inbox for messages of type T.
 *
 * MAILBOX_DEFINE(name, T, capacity)
 *   defines the storage + bookkeeping (in a .c file).
 *
 * mailbox_<name>_push(const T *msg) -> bool
 *   non-blocking push; returns false on full (caller decides retry/drop).
 *
 * mailbox_<name>_drain(void (*handler)(const T *)) -> size_t
 *   drains all queued messages, calls handler per message, returns count.
 */
#define MAILBOX_DECLARE(name, T) \
    bool mailbox_##name##_push(const T *msg); \
    size_t mailbox_##name##_drain(void (*handler)(const T *msg))

#define MAILBOX_DEFINE(name, T, capacity) \
    static struct mailbox g_mbox_##name; \
    static T g_mbox_##name##_buf[capacity]; \
    static void mailbox_##name##_init_once(void) { \
        static atomic_bool init_done = false; \
        if (!atomic_load(&init_done)) { \
            mailbox_init(&g_mbox_##name, g_mbox_##name##_buf, \
                         capacity, sizeof(T)); \
            atomic_store(&init_done, true); \
        } \
    } \
    bool mailbox_##name##_push(const T *msg) { \
        mailbox_##name##_init_once(); \
        return mailbox_push(&g_mbox_##name, msg); \
    } \
    size_t mailbox_##name##_drain(void (*handler)(const T *msg)) { \
        mailbox_##name##_init_once(); \
        T tmp; \
        size_t n = 0; \
        while (mailbox_pop(&g_mbox_##name, &tmp)) { \
            handler(&tmp); \
            n++; \
        } \
        return n; \
    }

#endif /* ZCL_FRAMEWORK_MAILBOX_H */
```

Adjust to match the actual `mailbox_init` / `mailbox_push` / `mailbox_pop`
signatures in `lib/util/include/util/mailbox.h` — read that header first.

**Acceptance:** compiles standalone; macros expand to legal C.

### Task 2: `app/services/include/services/header_admit_inbox.h`

Declare a typed inbox for header-discovery messages:

```c
/* app/services/include/services/header_admit_inbox.h
 *
 * Mailbox inbox for header_admit_stage.  Header-probing callers push
 * one of these per discovered header; header_admit_stage drains them
 * on each tick.
 */
#ifndef ZCL_SERVICES_HEADER_ADMIT_INBOX_H
#define ZCL_SERVICES_HEADER_ADMIT_INBOX_H

#include "framework/mailbox.h"
#include "primitives/uint256.h"   /* or wherever bytes32 is defined */
#include <stdint.h>

struct header_admit_msg {
    int64_t height;            /* hint; admit can verify */
    bytes32 hash;
    uint32_t peer_id;          /* who told us */
    int64_t observed_unix;
};

MAILBOX_DECLARE(header_admit, struct header_admit_msg);

#endif
```

Find the actual `bytes32` typedef in tree — likely `uint256` in
`lib/primitives/include/primitives/uint256.h`. Use whatever's idiomatic.

**Acceptance:** compiles; header is included cleanly from both probe + admit.

### Task 3: Wire mailbox storage in header_admit_stage.c

In `app/services/src/header_admit_stage.c`:
- At top, add `MAILBOX_DEFINE(header_admit, struct header_admit_msg, 1024);`
- In the stage's tick function (already exists), call
  `mailbox_header_admit_drain(handle_msg)` BEFORE doing the existing
  scan work.
- Implement `handle_msg(const struct header_admit_msg *m)` that does
  whatever the current "we have a header to admit" path does — but
  driven by the message instead of polling.

If the current admit path doesn't have an obvious "we have a header N"
entrypoint, this task may need to refactor — keep it small. If it'd
be more than 100 LOC of refactor, append a `BLOCKED: stage refactor too
large for this assignment, propose splitting` note and stop.

**Acceptance:**
- Stage still ticks correctly (no regression in `make test_parallel`
  for `header_admit_stage` tests).
- Stage now drains the mailbox in addition to its existing scan.

### Task 4: Wire mailbox push in header_probe_service.c

In `app/services/src/header_probe_service.c`:
- Find where a newly-discovered header is currently handed off to
  admit logic.
- Replace (or augment) that with `mailbox_header_admit_push(&msg)`.
- If push returns false (full), use existing observability path to log
  drop with `obs-ok:` marker.

Keep the existing direct path WORKING (don't delete it yet — the
mailbox path is additive in Phase 1; we delete the direct path in
Phase 2 when both have been observed equivalent for a week).

**Acceptance:** Compiles. Header_probe pushes to mailbox on new header
discovery in addition to its existing handoff.

### Task 5: Unit test `lib/test/src/test_mailbox_adoption.c`

Two test cases:
1. **`header_admit_inbox_push_drain`** — push 3 messages; drain; verify
   all 3 received, in order; verify drain returns 3; subsequent drain
   returns 0.
2. **`header_admit_inbox_full_returns_false`** — push capacity+1 messages;
   verify the overflow push returns false; no crash.

Add `failures += test_mailbox_adoption();` to `lib/test/src/test.c` and
declaration to `lib/test/include/test/test_helpers.h`.

**Acceptance:** `make test_parallel` passes; new tests visible in output.

### Task 6: Verify no regression

```
make -j$(nproc)             # build clean
make lint                    # all 20 gates pass (3 in WARN mode)
./test_parallel --jobs=$(nproc)   # 0 failures
```

If any regression: investigate. The mailbox path is ADDITIVE — direct
path stays — so existing tests should not change behavior.

**Acceptance:** all green.

---

## Commit cadence

One commit per task. After tasks 3, 5: `git push origin wt2/phase1-mailbox-adoption`.

Each commit ends with:
```
Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
```

---

## Push final + completion

```bash
make test_parallel --jobs=$(nproc)
make lint
git push origin wt2/phase1-mailbox-adoption
```

Append completion section to this doc per `docs/work/agent-protocol.md`.

---

## Status

**READY** — start when human invokes you in `~/github/zclassic23-2`.

<!-- Worker: append a Completion section below when done. -->
