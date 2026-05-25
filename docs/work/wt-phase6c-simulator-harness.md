# Worker Assignment — Phase 6c: Simulator harness (chaos CI)

**Worktree:** wt2 OR wt3 (either)
**Branch:** PUSH DIRECT TO MAIN
**Phase:** 6 (Determinism + simulator)
**Depends on:** Phase 6a (seed_tape) ✅ + Phase 6b (postmortem capsule) ✅.
**Status: IN PROGRESS (wt2)** — claimed 2026-05-25 after 6a + 6b merged.
**Plan reference:** [`docs/architecture/phase6-determinism-and-simulator.md`](../architecture/phase6-determinism-and-simulator.md) § 6c

**Owns:**
- NEW `tools/sim/chaos.c` — single binary that boots a node under a
  seed_tape with injected adversarial events
- NEW `tools/sim/scenarios/*.scenario` — declarative chaos scenarios
- NEW `lib/test/src/test_chaos_harness.c`
- EDIT `Makefile` — `make chaos` target
- EDIT `.github/workflows/ci.yml` (if exists) — nightly chaos run
- NEW `docs/CHAOS_HARNESS.md` — scenario authoring guide

**MUST NOT touch:**
- `seed_tape.{c,h}` (6a primitive)
- `postmortem.{c,h}` (6b primitive)
- Production node code paths — purely an off-line testing harness
- `docs/REFACTOR_STATUS.md`, `docs/FRAMEWORK.md`, `CLAUDE.md`

---

## Why this matters

Phases 6a + 6b enable: "production crashes produce replayable
capsules." That's reactive.

Phase 6c flips it to **proactive**: a chaos harness that, on every
CI run, boots N simulated nodes under different seeds, injects
adversarial events (peer disconnects, malformed blocks, clock skew,
disk-full conditions, OOM at allocation boundaries), and asserts the
node stays healthy. Each crashing scenario yields a seed + capsule
that's checked into the repo as a regression fixture.

This is the structural answer to "we shipped a bug, it took 3 days
to reproduce." With chaos in CI, the bug would have been caught at
PR time + a reproducible seed attached to the failure.

---

## Scenario format

Plain-text declarative scenarios live in `tools/sim/scenarios/`.
Each scenario is one file:

```
# scenarios/peer_disconnect_at_height_500.scenario
# Comments start with #. Blank lines ignored. Order matters.

seed              0xdeadbeef00000001
boot_phase        idb_complete
peer_count        4

at_event 500      kill_peer       0
at_event 1000     kill_peer       1
at_event 1500     send_malformed_block  peer=2  type=invalid_pow
at_event 2000     advance_clock   +3600s
at_event 2500     trigger_oom_at  utxo_apply

expect            no_crash
expect            tip_height >= 2500
expect            reorg_count <= 1
expect            consensus_rejects == 0
```

The parser is dumb (no eval, no nested expressions); each line is
one command. New commands are added by extending a small dispatch
table in `chaos.c`.

---

## Commands (initial set)

| Command | Args | Effect |
|---|---|---|
| `seed` | hex | Open seed_tape with this seed (else random) |
| `boot_phase` | enum | Boot the node up to this phase (idb_complete, listening, mempool_open) |
| `peer_count` | N | Spawn N simulated peers |
| `kill_peer` | id | Drop peer connection |
| `send_block` | peer=I, file=PATH | Inject a recorded block from a fixture |
| `send_malformed_block` | peer=I, type=ENUM | Inject a known-bad block (8 invalid types) |
| `advance_clock` | DURATION | Move simulated wall clock forward (s, m, h, d) |
| `trigger_oom_at` | site=ENUM | Force the next zcl_malloc at `<site>` to return NULL |
| `partition_network` | for=DURATION | Drop all peer messages for the duration |
| `expect` | assertion | Assert on tape state at end (no_crash, tip_height >= N, ...) |

The OOM and partition primitives require small hooks into
`safe_alloc.h` + `net_io.c` (a fault-injection mode gated by a
runtime flag — zero overhead when disabled).

---

## Tasks (in order)

### Task 1: Scenario parser + dispatcher skeleton

NEW `tools/sim/chaos.c`. Single binary `zclassic23-chaos` that:
1. Takes `--scenario=PATH` arg.
2. Parses scenario file (one line = one command).
3. For each command, calls a handler from a static dispatch table.
4. After all commands, runs `expect` assertions and prints PASS/FAIL.
5. Exit 0 on PASS, 1 on FAIL.

Implement parsers + handlers for: `seed`, `boot_phase`,
`peer_count`, `expect`. Stubs for the rest.

**Acceptance:** `./zclassic23-chaos --scenario=scenarios/smoke.scenario`
(a minimal "seed + boot + expect no_crash") returns 0.

### Task 2: Fault injection hooks

EDIT `lib/util/include/util/safe_alloc.h` + `lib/util/src/safe_alloc.c`:
add a runtime flag `g_alloc_fault_site` (atomic). When `zcl_malloc`
is called with a matching `label`, return NULL once + clear the flag.

EDIT `lib/net/src/net_io.c` similarly: a `g_net_partition_until_unix`
atomic that, when set, drops all incoming messages until clock passes.
This repository does not currently have `lib/net/src/net_io.c`; wt2 mapped
the primitive onto `lib/net/src/msgprocessor.c` and keeps the atomic flag in
`lib/net/src/net_fault.c` so the standalone chaos binary can link it without
pulling in the full message processor.

Both flags default to inactive (zero overhead in production).

**Acceptance:** chaos harness commands `trigger_oom_at` and
`partition_network` work — assertions about node behavior under
these conditions pass/fail correctly.

### Task 3: Simulated peers

NEW `tools/sim/sim_peer.c`. An in-process peer simulation that:
- Connects to the chaos-driven node via the existing P2P stack.
- Reads fixture blocks from `tests/fixtures/blocks/`.
- Sends them on schedule, responds to GETDATA, etc.

Hooked into the harness via the `peer_count` + `send_block` /
`send_malformed_block` commands.

**Acceptance:** scenario can drive a full IBD from N simulated
peers, with one peer sending a malformed block — the harness
asserts the bad block is rejected and the good peers continue.

### Task 4: Initial scenarios

Author 5 starter scenarios in `tools/sim/scenarios/`:

1. `smoke.scenario` — boot, idle 60s, expect no_crash.
2. `peer_churn.scenario` — connect 8 peers, kill 4 at random
   intervals over 5 min, expect tip_height advances.
3. `malformed_blocks.scenario` — 1 peer sends 8 different
   malformed block types, expect all rejected.
4. `clock_skew.scenario` — advance clock +1h, expect mempool
   pruning fires correctly.
5. `oom_at_utxo_apply.scenario` — trigger OOM during utxo_apply,
   expect graceful shutdown (no consensus violation).

Each scenario should be < 30 lines and well-commented.

**Acceptance:** `make chaos` runs all 5, all PASS.

### Task 5: `make chaos` target

EDIT `Makefile`:

```makefile
.PHONY: chaos chaos-clean
chaos: zclassic23-chaos
	@for s in tools/sim/scenarios/*.scenario; do \
	    echo "==> $$s"; \
	    ./zclassic23-chaos --scenario="$$s" || exit 1; \
	done
	@echo "==> All chaos scenarios PASSED"

zclassic23-chaos: tools/sim/chaos.c $(LIB_OBJS) ...
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

chaos-clean:
	@rm -f zclassic23-chaos
	@rm -rf chaos-output/
```

**Acceptance:** target exists, builds the binary, runs all
scenarios.

### Task 6: CI integration

If `.github/workflows/ci.yml` exists, add a chaos step to a nightly
job. On failure, upload the seed + scenario + capsule as artifacts.

If no CI workflow, skip — operator runs `make chaos` manually
before releases.

**Acceptance:** workflow yaml present (if applicable), runs green
on a stable revision.

### Task 7: Test harness coverage

EDIT `lib/test/src/test_chaos_harness.c`. Test the harness itself:
- Scenario parser handles edge cases (empty file, comment-only,
  unknown commands).
- Expect assertions fire correctly (pass/fail paths both covered).
- Fault injection flags work (synthetic test, not full node).

Add to `test_parallel.c` TEST_LIST + helpers header.

**Acceptance:** `./test_parallel --jobs=$(nproc)` includes
`chaos_harness: N passed, 0 failed`.

### Task 8: `docs/CHAOS_HARNESS.md`

Authoring guide for new scenarios. Cover:
1. The 10 built-in commands + their semantics.
2. How to add a new fault injection point.
3. How to convert a postmortem capsule (from 6b) into a
   regression scenario.
4. How to debug a failing scenario (use `--verbose` + capsule).

**Acceptance:** doc exists, < 200 lines, walks through one
example end-to-end.

### Task 9: Final verify + push

```bash
make -j$(nproc)
make lint
./test_parallel --jobs=$(nproc)
make chaos
git pull --rebase origin main
git push origin main
```

Append Completion section.

---

## Live verification (post-merge)

After this PR ships:
1. CI's nightly chaos run becomes the standing regression gate.
2. Whenever a production crash occurs (with 6b's capsule), the
   capsule's tape gets converted into a scenario and checked in.
   Future builds will reproduce the bug from a fresh seed each
   nightly run.
3. The number of `tools/sim/scenarios/*.scenario` files monotonically
   increases over time — each is a permanent regression test.

---

## What this does NOT do

- Does NOT run mutation testing — that's a different harness
  (potential future Phase 7).
- Does NOT auto-bisect a failing scenario to find the offending
  commit. The seed + capsule make manual bisect trivial; auto-bisect
  is future tooling.
- Does NOT inject CPU scheduling chaos (thread interleavings). The
  seed_tape pins RNG + clock but not scheduling. A future PR could
  add this with `LD_PRELOAD` of pthread shims.
- Does NOT touch consensus logic. The harness is fault injection,
  not protocol modification.

---

## Risk + rollback

The fault injection hooks add a single atomic_load on the hot path
(zcl_malloc, net_io). Measured overhead: ~1 ns per call when flag
is inactive. Acceptable.

If the harness has a bug, scenarios fail spuriously — fix the
harness, no production impact.

The chaos binary is built separately from the production node, so
release builds aren't affected.

---

## Commit cadence

One commit per task. Push after tasks 2, 4, 7.

---

## Status

**IN PROGRESS (wt2)** — claimed 2026-05-25 after 6a + 6b merged.

### Progress (wt2, 2026-05-25)

- Started Task 1 with a standalone `zclassic23-chaos` parser/dispatcher
  skeleton, a `tools/sim/scenarios/smoke.scenario` fixture, and `make chaos`.
  The initial command set handles `seed`, `boot_phase`, `peer_count`, and
  `expect`; later Phase 6c injection commands are recognized stubs.
- Added initial `test_chaos_harness` coverage for parser success, empty
  scenarios, unknown commands, recognized-but-unimplemented commands, bad
  seeds, and failing `expect` assertions.
- Continued Task 2 fault injection: `safe_alloc` now has a one-shot label
  hook, and `trigger_oom_at` arms and verifies it in the chaos harness.
- Added the network partition primitive as `net_fault.{c,h}`, wired the drop
  check into `msg_process_messages`, and made `partition_network for=DURATION`
  arm and verify the hook in the chaos harness.

<!-- Worker: append a Completion section below when done. -->
