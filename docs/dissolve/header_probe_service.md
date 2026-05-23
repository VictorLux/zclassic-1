# Dissolve plan: `header_probe_service.c` → smaller service + mailbox + 1 Condition

**Module:** `app/services/src/header_probe_service.c` (1,286 LOC)
**Header:** `app/services/include/services/header_probe_service.h`
**Phase:** 3 (Dissolve mega-modules)
**Gated on:** Wave S cutover C-2 + C-3 shipped (S-2 header_admit and
S-3 validate_headers authoritative — they own the ingest side of headers)

---

## Why this exists today

`header_probe_service.c` is responsible for **fetching headers from peers
ahead of body sync**. It:

1. Selects peers to probe based on their reported tip + recent latency.
2. Issues `msg_getheaders` to the chosen peer(s).
3. Receives header batches, publishes them into the admit pipeline.
4. Tracks peer behavior (timeouts, malformed batches, etc.) for
   scoring.
5. Coordinates with `sync_watchdog_service.c` on stalls
   (the `local_header_refill_needed` predicate).

After Phase 1a (mailbox adoption), header_probe ALREADY publishes
accepted headers into `header_admit_inbox` — that part is clean.

After Phase 3 watchdog dissolve PR-2, the `local_header_refill_needed`
predicate becomes a condition that calls `header_probe_kick_for_height`.

What remains in header_probe_service.c after those two events is:
- Peer selection logic
- The msg_getheaders request/response loop
- Peer scoring
- A lot of stats + dump_state surface

---

## The decomposition

### Replacement A — `services/network/header_probe.c` (NEW, smaller, ~400 LOC)

The minimal core: peer selection + getheaders dispatch + response parse.
Publishes into `header_admit_inbox` (already wired). Same public API
shape but ~3× smaller.

### Replacement B — `jobs/header_probe_poll.c` (NEW, ~80 LOC)

A Job (Wave S sense) that periodically calls
`header_probe_pull_range(local_tip+1, 2000)` on a 30s cadence. Replaces
the current background thread in header_probe_service.c.

### Replacement C — Peer scoring moves to `lib/net/src/peer_scoring.c`

The header-specific scoring (timeouts, malformed batches) is already
handled in `peer_scoring.c`'s reject-counter pattern. Migrate the
header-specific reject codes there. ~50 LOC migration.

### What gets DELETED outright

- The background thread loop (replaced by the Job).
- The `header_probe_pull_range_blocking` variant (no caller needs the
  blocking variant after Wave S cutover — the saga is async).
- Most of the dump_state surface (per-condition metrics live in the
  condition engine after watchdog dissolve).
- The peer-selection FSM if peer_scoring's existing rotation covers it.

---

## Migration sequence (3 PRs)

### PR-1: Extract `header_probe_poll` Job

The Job invokes the existing service's `pull_range` function. Service
keeps doing the work for now. Just moves the SCHEDULING from a
background thread to the supervisor.

Ship gate: existing tests pass, polling cadence unchanged.

### PR-2: Shrink `header_probe_service` to the core

Inline the public functions that are no longer needed externally.
Move peer-specific scoring to `peer_scoring.c`. Trim dump_state to
just the core stats.

Target after PR-2: ~400 LOC.

### PR-3: Rename and split

`header_probe_service.c` → `services/network/header_probe.c` (smaller).
Delete the old file. Update the 6 call sites.

Net: 1,286 LOC out, ~480 LOC in. **~800 LOC deletion.**

---

## Acceptance gates per PR

- `make test_parallel` PASS.
- Live smoke: header sync velocity unchanged (blocks_per_sec).
- 24h live soak with stable peer rotation.

---

## Status

DRAFT — actionable after Wave S cutover C-2 + C-3 shipped.
