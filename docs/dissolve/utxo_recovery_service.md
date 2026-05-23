# Dissolve plan: `utxo_recovery_service.c` → 1 Job + 1 Condition + small helpers

**Module:** `app/services/src/utxo_recovery_service.c` (1,241 LOC)
**Phase:** 3 (Dissolve mega-modules)
**Gated on:** Wave S cutover C-8 shipped (S-8 utxo_apply authoritative —
the saga owns the canonical UTXO write path before we touch recovery)

---

## Why this exists today

`utxo_recovery_service.c` handles **UTXO drift detection and repair**:

1. **Drift detection** — periodically compute SHA3 hash of our UTXO set
   and compare to trusted-peer hashes. Mismatch = drift.
2. **Wipe + reimport** — when drift is detected and severe, wipe the
   UTXO table and reimport from a trusted source (LevelDB snapshot,
   peer snapshot, or full chain replay).
3. **Reimport flag management** — durable flag that survives kill -9
   so a partial reimport resumes on next boot.
4. **Self-heal scan fallback** — when tx_index misses, scan blocks
   directly. (Confusingly named; this is a READ-path fallback, not a
   recovery action.)

After Wave S cutover C-8, the saga owns the canonical UTXO write path
and the "drift" can only happen from cosmic rays, disk corruption, or a
bug in the saga itself. The recovery primitive is still needed but
much smaller.

---

## The decomposition

### Replacement A — Condition `utxo_drift_detected` (~120 LOC)

- **detect** — SHA3 of local UTXO set ≠ trusted-peer SHA3 for the same
  height, computed every 1h via the existing utxo_audit machinery.
- **remedy** — call `utxo_repair_kick(level)` where level escalates:
  - level 1: rescan from current chainstate (cheap, fixes corruption
    in cache layer)
  - level 2: full UTXO wipe + reimport from latest SHA3 snapshot
  - level 3: full chain replay from genesis (last resort)
- **clear** — SHA3 matches trusted peer.
- **max_attempts**: 1 per level. After level 3 fails, page operator.

### Replacement B — `jobs/utxo_repair.c` (NEW, ~300 LOC)

The "do the repair" job. Cursor on disk so kill -9 resumes.

```c
struct utxo_repair_input {
    int level;                /* 1, 2, or 3 */
    const char *trigger_reason;
};

enum utxo_repair_result {
    REPAIR_IN_PROGRESS,
    REPAIR_COMPLETE,
    REPAIR_FAILED,
};

enum utxo_repair_result utxo_repair_job_step(struct utxo_repair_input *in);
```

The job invokes the existing primitives (`utxo_recovery_wipe`,
`utxo_recovery_import_ldb`, full chain replay) at the appropriate level,
but with cursor checkpointing every N blocks so it's resumable.

### Replacement C — `lib/storage/include/storage/utxo_reimport_flag.h`

Tiny module (~50 LOC) — the persistent reimport flag. Currently inside
`utxo_recovery_service.c`; promote to a primitive used by the job.

### Replacement D — Move "self-heal scan fallback" to `tx_index` reader

The fallback that scans blocks when tx_index misses is a READ-path
concern, not a recovery concern. Move to
`app/models/include/models/tx_index.h` as a transparent fallback.

---

## Migration sequence (4 PRs)

### PR-1: Extract `utxo_reimport_flag` primitive

Move the durable-flag logic to its own file. Existing service still
uses it.

### PR-2: Extract the "self-heal scan fallback" to tx_index reader

Pure code move. ~150 LOC out of utxo_recovery_service.c, into a small
helper in the tx_index model.

### PR-3: Implement `utxo_repair_job` + `utxo_drift_detected` condition

Wire them up. Existing `utxo_recovery_*` public functions become
thin shims that delegate to the job.

### PR-4: DELETE `utxo_recovery_service.{c,h}`

Update the 8-ish call sites. ~50 LOC delta.

Net: 1,241 LOC out, ~470 LOC in. **~770 LOC deletion.**

---

## Acceptance gates per PR

- `make test_parallel` PASS, including `test_utxo_audit.c` and
  `test_self_heal_scan_fallback.c`.
- Live smoke: trigger a simulated UTXO drift (modify one UTXO row in
  coins.db); condition fires; level-1 repair completes within 60s;
  drift counter clears.
- 24h soak: zero spurious drift detections.

---

## Status

DRAFT — actionable after Wave S cutover C-8 shipped.
