# Dissolve plan: `legacy_mirror_sync_service.c` → 1 Service + 1 Job + 1 Condition

**Module:** `app/services/src/legacy_mirror_sync_service.c` (1,411 LOC)
**Header:** `app/services/include/services/legacy_mirror_sync_service.h` (135 LOC)
**Phase:** 2 (Wave S → S-12 cutover) — the "S-12" stage of Wave S
**Strategy:** strangler. Module exists to mirror the local `zclassicd`
node's chain into ours. After Wave S authoritative cutover, mirroring
is a one-shot **bridge** (cursor-stamp at H+1) plus a periodic polling
**job**, not a long-running service.

---

## Why this exists today

The local `zclassicd` (the C++ reference node) syncs to tip in ~5s
because it's been syncing for years. The `legacy_mirror_sync_service`
polls `zclassicd`'s RPC and ingests new blocks into our chain ahead of
peer-network sync. This is part of the "personal sovereignty stack"
play — your two local nodes converge faster than network rollout.

But it's also 1,411 LOC of:
- polling-loop state machine (waiting/observing/catching_up/healthy/blocked)
- per-block ingest path that BYPASSES the chain-advance saga
- "unsafe override" gates (audit reasons captured in `unsafe_overrides_total`)
- gating policy (`gated_by_local_retries`, `legacy_advisory_gated`, etc.)
- a separate dump_state with ~30 fields

The complexity is because today it's a parallel chain-advance path. With
Wave S authoritative, mirror becomes a SOURCE that stamps the cursor at
H+1 and lets the saga ingest from there — no parallel ingest path needed.

---

## The decomposition

### Replacement A — `services/sync/legacy_bridge.c` (NEW, ~200 LOC)

One-shot ingest from a local `zclassicd` snapshot, cursor-stamping at
H+1.

```c
struct legacy_bridge_input {
    const char *zclassicd_datadir;    /* e.g., ~/.zclassic */
    int64_t target_height;            /* sync up to this; 0 = tip */
};

struct legacy_bridge_result {
    int64_t imported_until_height;
    int64_t cursor_stamped_at;
    bool    ok;
    char    error[128];
};

int legacy_bridge_import(const struct legacy_bridge_input *in,
                         struct legacy_bridge_result *out);
```

That's the entire ingest API. Internally:
1. Read `zclassicd`'s LevelDB block index (`chainstate_legacy_reader.c`
   exists for this — re-use).
2. For each block H from current local tip+1 to target_height:
   - Read body, write to event log (Phase 4) OR directly to canonical
     storage (pre-Phase-4 fallback).
3. Atomic cursor stamp: write
   `progress_store: stage_cursor[*] = imported_until_height` so the
   saga picks up cleanly at H+1.
4. Return.

No threading. No polling. Caller (e.g., `-legacy-attach` flag, or a
periodic Job) decides when to invoke.

### Replacement B — `jobs/legacy_poll.c` (NEW, ~100 LOC)

A Job (Wave S sense) that:
- Runs every 60s (configurable).
- Calls `zclassicd_rpc("getblockcount")` to see if the local zclassicd
  is ahead.
- If `zclassicd_tip > local_tip + 10`, calls `legacy_bridge_import` to
  catch up.
- Otherwise idle.

Job cursor lives in `progress.kv` like every other Job.

### Replacement C — Condition: `legacy_mirror_drift` (~80 LOC)

- **detect** — local tip lagging zclassicd's tip by >100 blocks for
  >300s, AND zclassicd is healthy (`getblockcount` returns
  successfully).
- **remedy** — explicit `legacy_bridge_import(target=zclassicd_tip)`
  call. Bigger than the periodic job's 10-block trigger, but same
  primitive.
- **clear** — local tip catches up to within 10 blocks of zclassicd.
- **max_attempts**: 3 over 30 minutes, then operator page (means
  ingest is broken, not just slow).

### What gets DELETED outright

- The 5-state polling FSM (waiting/observing/catching_up/healthy/blocked).
  Replaced by "Job runs, or it doesn't." Plus the condition for drift.
- The `unsafe_overrides_total` counter and the override gates. These
  exist because mirror's parallel ingest path could RACE the chain
  advance; with mirror as a cursor-stamp bridge, no race is possible.
- The `gated_by_local_retries` / `legacy_advisory_gated_by_native_retries`
  policy. Mirror is no longer "advisory" — it's an ingest source like
  P2P. The Wave S saga decides what to validate.
- All the `legacy_mirror_*` MCP tools' specific fields move to a
  smaller `legacy_bridge` dump_state.

---

## Migration sequence (3 PRs, after Wave S S-9 ships)

### PR-1: Extract `legacy_bridge.c` from the existing mirror code

The mirror's per-block ingest path becomes the body of
`legacy_bridge_import`. Existing service still calls this internally
for now — no behavior change.

Ship gate: existing mirror still works. New `legacy_bridge_import` has
a unit test that imports 100 blocks from a fixture.

### PR-2: Replace the polling FSM with a Job + Condition

Delete the 5-state FSM. Wire the Job (60s tick) + Condition
(drift detector). The Job invokes `legacy_bridge_import` for small
catch-ups; the Condition invokes it for large catch-ups with operator
paging.

Existing `legacy_mirror_sync_*` public functions become thin wrappers
that delegate to the Job/Condition state. Stats fields are migrated to
the Job's dump_state.

### PR-3: DELETE `legacy_mirror_sync_service.{c,h}`

After PR-2 has soaked for 24h on a live node with the Job + Condition
running, delete the file. Update the 6 call sites that import the old
header to use the new entry points (`legacy_bridge_*`,
`legacy_poll_job_*`).

Update MCP tool registrations:
- `zcl_state subsystem=legacy_mirror` → `legacy_bridge`
- The 30-field dump becomes a ~10-field dump.

Net deletion: 1,546 LOC out, ~380 LOC in. **~1,166 LOC deletion.**

---

## Acceptance gates per PR

- `make test_parallel` PASS.
- Live smoke: with local zclassicd running, the local zclassic23 node
  stays within 100 blocks of zclassicd over a 24h soak. The condition
  fires no more than once per day.
- The `-legacy-attach` flag still works (one-shot import).
- The `-cold-import` flag still works (one-shot import).

---

## Risk + mitigations

- **Race between mirror ingest and saga ingest.** Today: mirror has
  the "unsafe override" complexity to handle this. After dissolve:
  mirror only stamps the cursor; saga ingests from cursor. Atomic by
  construction (one writer per cursor). Risk: gone.
- **Mirror ingest writes invalid block.** Today: the override gates
  try to detect. After: bridge writes to event log (Phase 4) which is
  audit-trailed and replayable; if a bad block lands, projection
  rebuilds catch it within minutes. Pre-Phase-4: bridge calls the same
  consensus validators that the saga does — invalid blocks rejected at
  ingest, no override possible.
- **Performance regression.** Today: mirror polls every 5s and may
  ingest >1 block per tick. After: Job polls every 60s. If 60s is too
  slow, lower it to 15s — still cheaper than the current 5s + FSM
  overhead.

---

## Status

DRAFT — actionable after Wave S S-9 (tip_finalize) ships. The first PR
of this dissolve will be `wt?-phaseX-legacy-mirror-dissolve-pr1.md` and
is gated on the S-9 merge.
