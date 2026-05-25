# Dissolve plan: `chain_advance_coordinator.c` → 1 Service + 1 Job + 1 Condition

**Module:** `app/services/src/chain_advance_coordinator.c` (1,716 LOC)
**Header:** `app/services/include/services/chain_advance_coordinator.h` (203 LOC)
**Phase:** 2 (Wave S → S-12 cutover) and Phase 3 (mega-module dissolution)
**Strategy:** strangler — fully replace after Wave S S-9 (tip_finalize) ships.
The CAC exists to coordinate the FOUR sync sources today; after Wave S
authoritative cutover, the saga IS the sync path and CAC's coordination
role evaporates.

---

## Why this exists today

CAC handles the question: "We could advance the chain from 4 different
sources (P2P live, snapshot, local import, zclassicd-mirror). Which one
do we use right now, and why?"

```c
enum cac_source {
    CAC_SOURCE_P2P,                  // live network peers
    CAC_SOURCE_SNAPSHOT,             // FlyClient + SHA3 UTXO snapshot
    CAC_SOURCE_LOCAL_IMPORT,         // -cold-import from local zclassicd
    CAC_SOURCE_ZCLASSICD_MIRROR,     // ongoing mirror from local zclassicd
};

enum cac_decision_result {
    CAC_DECISION_WAIT,           // not ready — no source healthy
    CAC_DECISION_USE_SOURCE,     // proceed with selected source
    CAC_DECISION_BLOCKED,        // policy blocks all sources
    CAC_DECISION_RECOVER,        // trigger recovery first
};
```

The coordinator runs as part of the chain-advance loop, picks a source
per tick, evaluates trust/health, decides whether to advance or wait,
and tracks per-source status for diagnostics.

---

## Why dissolve

- 1,716 LOC of decision-table logic that's hard to test in isolation.
- The 4-source coordination is **a Wave-S-era patch**. The saga
  authoritative path (Wave S S-2..S-9 fully cut over) only uses two
  sources: P2P live + cursor-stamp-bridged fast-sync. SNAPSHOT and
  LOCAL_IMPORT and ZCLASSICD_MIRROR become one-shot **bridges** that
  stamp the cursor and exit — not per-tick coordinated participants.
- Most of CAC's `force_mirror_promotion`, `mirror_repair_allowed`,
  `peer_floor_recovery_needed`, `local_header_refill_needed` predicates
  are SHOULD-BE Conditions, not coordinator decisions. Phase 3 PR-1/PR-2
  already moves several of these.

---

## The decomposition

### Replacement A — `services/sync/source_scorer.c` (NEW, ~300 LOC)

Pure functional layer that scores each available source for the next
tick. No side effects, no atomics.

```c
struct source_scoring_input {
    int64_t local_tip_height;
    int64_t peer_max_height;
    int     healthy_peer_count;
    int64_t last_snapshot_offer_unix;
    bool    legacy_attach_in_progress;
    int64_t now_unix;
};

enum source_id {
    SRC_P2P_LIVE,
    SRC_FAST_SYNC_BRIDGE,   /* unifies snapshot + cold-import + mirror */
};

struct source_score {
    enum source_id id;
    int score;              /* higher = better; 0 = unusable */
    const char *reason;     /* human-readable, stable suffix */
};

void source_scorer_evaluate(const struct source_scoring_input *in,
                            struct source_score out[2]);
```

That's it. Two sources max. Scoring is a 20-line switch.

### Replacement B — `jobs/tip_finalize.c` (NEW, will be S-9 Wave S stage)

The "advance the tip by one block" step that CAC currently dispatches.
Becomes the canonical Wave S stage S-9 (already planned). Reads from
`utxo_apply_log` (S-8's output) and writes the final consensus tip
forward.

### Replacement C — Existing conditions absorb the recovery predicates

The "should we recover?" decision branches map to Conditions already
in flight via the `sync_watchdog_service.c` dissolve:

| CAC predicate | Becomes |
|---|---|
| `mirror_repair_allowed` | `condition: legacy_mirror_stuck` |
| `peer_floor_recovery_needed` | `condition: peer_floor_violated` (Phase 3 PR-3) |
| `local_header_refill_needed` | `condition: local_header_refill_needed` (Phase 3 PR-2) |
| `snapshot_offer_allowed` | NEW `condition: snapshot_offer_ready` — fires when peer offers a fresh snapshot and `local_tip < snapshot_height - 1000` |
| `force_mirror_promotion` | DELETE — was an escape hatch for the halts Phase 0 conditions now auto-heal |

### Replacement D — `chain_advance_coordinator_plan` → Saga driver

The per-tick `plan()` call gets replaced by the supervisor loop driving
each Wave S stage. There's no central coordinator deciding "use source
X this tick" — each stage advances at its own cursor pace, idempotently.
The supervisor just runs `step_once()` on each stage in turn.

---

## Migration sequence (5 PRs, after Wave S S-9 ships)

### PR-1: Extract `source_scorer.c` from CAC; gate CAC plan on it

Reroute `chain_advance_coordinator_plan` internals through
`source_scorer_evaluate`. Same external behavior, cleaner internal.

### PR-2: Migrate the recovery predicates to Conditions

`mirror_repair_allowed`, `peer_floor_recovery_needed`,
`local_header_refill_needed`, `snapshot_offer_allowed` — all become
Conditions (some already migrated by the watchdog dissolve PRs).
CAC's predicates become thin shims that read the Condition engine's
state instead of computing fresh.

### PR-3: Delete `CAC_SOURCE_SNAPSHOT` / `LOCAL_IMPORT` / `ZCLASSICD_MIRROR`

After Wave S S-9 is authoritative, these are one-shot cursor-stamp
bridges, not per-tick sources. Delete the per-tick coordination for
them; keep only the bridge entry points (in their own service files).

CAC is now down to ~700 LOC.

### PR-4: Delete `chain_advance_coordinator_plan`

The per-tick plan call is replaced by the supervisor walking each
Wave S stage independently. CAC's `plan()` is invoked from exactly
one place today — replace that call with the new loop.

CAC drops to ~300 LOC of just diagnostics.

### PR-5: DELETE `chain_advance_coordinator.{c,h}`

Move remaining diagnostics into the supervisor's `dump_state_json`.
File deleted. Header deleted. ~1,919 LOC gone.

Replaced by:
- `services/sync/source_scorer.c` (~300 LOC)
- `jobs/tip_finalize.c` (Wave S S-9 — already scheduled, no extra LOC here)
- 1-2 new conditions (~100 LOC each)

**Net deletion: ~1,400 LOC.**

---

## Acceptance gates per PR

- `make test_parallel` PASS, including new unit tests for each new
  primitive.
- Live node smoke: cold sync from peers, with each PR landing, shows
  the same tip-advance behavior. Per-stage `zcl_state subsystem=...`
  shows cursor advancing.
- No new operator pages in the 24h smoke window.

---

## Risk + mitigations

- **`force_mirror_promotion` was load-bearing for the old halt
  recovery path.** The remaining activation-no-progress mirror symptom is
  owned by `legacy_mirror_stuck`; stale-tip-with-data is owned by
  `block_failed_mask_at_tip`. After Wave S cutover, force_mirror_promotion has
  nothing distinct to promote — it's safe to delete.
- **Diagnostics regression.** Several MCP tools (`zcl_state
  subsystem=chain_advance_coordinator`, `zcl_admin`) read CAC's
  dump_state. Mitigation: PR-5 moves the relevant fields into the
  supervisor's dump_state so the MCP surface keeps the data, just at
  a new key.

---

## Status

DRAFT — actionable after Wave S S-9 (tip_finalize) ships. The first PR
of this dissolve will be `wt?-phaseX-cac-dissolve-pr1.md` and is
gated on the S-9 merge.
