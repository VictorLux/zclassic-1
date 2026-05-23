# wt3 Assignment — Phase 2: S-8 utxo_apply shadow stage

**Worktree:** `~/github/zclassic23-3`
**Branch:** `wt3/phase2-utxo-apply-shadow`
**Phase:** 2 (Wave S → S-12 cutover)
**Depends on:** S-7 proof_validate merged

**Owns:**
- NEW `app/services/include/services/utxo_apply_stage.h`
- NEW `app/services/src/utxo_apply_stage.c`
- NEW `lib/test/src/test_utxo_apply_stage.c`
- Edits to `test.c`, `test_parallel.c`, `test_helpers.h`
- Boot path: after `proof_validate_stage_init`
- Schema migration for `utxo_apply_log` table

**MUST NOT touch:**
- Any prior Wave S stage (read-only)
- `app/conditions/`, `app/controllers/`, `tools/mcp/` (wt2 owns)
- `lib/coins/`, `lib/validation/` — READ ONLY; the existing UTXO
  update path (`update_coins.c`, `connect_block.c`) is your reference,
  not editable
- `lib/framework/`, `lib/util/`, `lib/platform/`
- `docs/REFACTOR_STATUS.md`, `docs/FRAMEWORK.md`, `CLAUDE.md`

---

## Why this matters

S-8 is the SECOND-MOST-CRITICAL Wave S stage. It computes the UTXO
delta for each block (new outputs created, prevouts spent). In shadow
mode it computes the delta and DIFFS it against the existing
`update_coins.c` path's actual write — the diff is what tells us the
saga is consistent with current consensus before we flip the cutover.

After S-8, only S-9 (tip_finalize) remains before Wave S cutover.
That's when `chain_advance_coordinator.c` (1,715 LOC) finally becomes
deletable.

---

## Behaviour spec

For each height H starting at `utxo_apply_cursor + 1`:

1. **Floor:** if `H > proof_validate_cursor` → STAGE_IDLE.
2. **Upstream log:** read `proof_validate_log[H]`. If `ok=0` (proofs
   failed): log `utxo_apply_log[H] = (status='upstream_failed',
   ok=0, ...)`, advance cursor.
3. **Block read:** load the block body at H. If unavailable: STAGE_IDLE.
4. **Compute the UTXO delta in a TEMP overlay:**
   - For each tx in the block:
     - For each input (skip coinbase): record `(prevout_txid,
       prevout_vout)` as SPENT.
     - For each output: record `(txid, vout, value, script, height,
       is_coinbase)` as ADDED.
   - The temp overlay is a `struct utxo_delta` — just two arrays,
     no DB writes. Compute, don't apply.
5. **Verify the delta:**
   - Every SPENT prevout must exist in the current UTXO snapshot
     (read via a projection from Phase 1b — `chain_projection_*`).
     Mismatch → `status='spend_unknown_utxo', ok=0`,
     witness includes the first missing prevout.
   - Every ADDED output must NOT already exist (collisions = bug).
     Mismatch → `status='utxo_collision', ok=0`.
   - Sum of input values >= sum of output values + fee (standard
     consensus rule). Mismatch → `status='value_overflow', ok=0`.
6. **Compare delta to the live path:**
   - For shadow mode: query the live `coins.db` for the UTXO state at
     H+1 and compute the expected delta from `coins_view`. The
     computed-vs-live delta should be identical.
   - Mismatch → `status='delta_diverged', ok=0`, witness includes the
     first differing entry. This is the SHADOW-VS-LIVE DIFF that
     gates cutover.
7. **Success:** log `(status='verified', ok=1, spent_count=N,
   added_count=M, total_value_delta=...)`, advance cursor.
8. **Failure paths:** log with appropriate status, advance cursor
   anyway. No mutation.

---

## Architecture reference

- Pattern: `app/services/src/proof_validate_stage.c` (just shipped) and
  `script_validate_stage.c`. Read both first.
- UTXO read primitive: `chain_projection_*` from Phase 1b
  (`app/controllers/include/controllers/chain_projection.h`). Add new
  functions if needed (e.g.,
  `chain_projection_utxo_exists(txid, vout)`).
- Existing UTXO update logic: `lib/validation/src/update_coins.c`. READ
  to understand the canonical delta computation — don't modify.
- `coins_view` interface: `lib/coins/include/coins/coins_view.h`.

---

## Tasks (in order)

### Task 1: Schema migration `utxo_apply_log`

```sql
CREATE TABLE IF NOT EXISTS utxo_apply_log (
    height                INTEGER PRIMARY KEY,
    status                TEXT    NOT NULL,
    ok                    INTEGER NOT NULL,
    spent_count           INTEGER NOT NULL,
    added_count           INTEGER NOT NULL,
    total_value_delta     INTEGER NOT NULL,   -- in zatoshis, can be negative
    first_failure_kind    TEXT,
    first_failure_detail  BLOB,               -- 36 bytes: txid (32) + vout (4)
    applied_at            INTEGER NOT NULL
);
```

Bump migration version.

### Task 2: `utxo_apply_stage.h`

Standard 3-function public API.

### Task 3: `utxo_apply_stage.c`

Mirror `proof_validate_stage.c` structure. Counters:
- `g_verified_total`, `g_spend_unknown_total`, `g_utxo_collision_total`,
  `g_value_overflow_total`, `g_delta_diverged_total`,
  `g_upstream_failed_total`, `g_internal_error_total`
- `g_total_outputs_added`, `g_total_outputs_spent`

Add new chain_projection helpers if needed (in
`app/controllers/include/controllers/chain_projection.h`):
- `bool chain_projection_utxo_exists(const uint8_t txid[32], int vout)`
- `int64_t chain_projection_utxo_value(const uint8_t txid[32], int vout)`

These open ephemeral projections — cheap because WAL readers don't lock.

Discipline:
- `platform_time_wall_unix()`, AR_* macros, no raw SQL/clock
- Wire dump_state_json, register in diagnostics dispatcher + ops enum_csv

### Task 4: Boot wiring

After `proof_validate_stage_init(ms)`, add `utxo_apply_stage_init(ms)`.

### Task 5: Test `test_utxo_apply_stage.c`

Mirror `test_proof_validate_stage.c`. Test cases:
- Happy path: 3 blocks → all `ok=1, status='verified'`.
- Spend unknown UTXO: tx with input referencing non-existent prevout
  → `status='spend_unknown_utxo'`, witness has the prevout.
- UTXO collision: simulate (set up DB so an output already exists)
  → `status='utxo_collision'`.
- Value overflow: tx where outputs > inputs → `status='value_overflow'`.
- Shadow-vs-live diff: feed a block, mutate `coins.db` to differ from
  what computed delta says → `status='delta_diverged'`.
- Upstream failure propagates.
- Idle.

### Task 6: Final verify + push

```bash
make -j$(nproc)
make lint
./test_parallel --jobs=$(nproc)
git push origin wt3/phase2-utxo-apply-shadow
```

Append Completion section.

---

## Live observability note

After this stage runs on a live node, orchestrator will use
`zcl_state subsystem=utxo_apply` to watch the cursor advance and
specifically the `g_delta_diverged_total` counter. **Zero divergences
for 24h is the cutover gate** — the saga's delta computation matches
production's. Any divergence is investigated before S-9 cutover work
begins.

---

## Commit cadence

One commit per task. Push after tasks 2, 4, 5.

---

## Status

**READY** — gated on S-7 merge. Start when human invokes you in
`~/github/zclassic23-3` AFTER S-7 is merged.

<!-- Worker: append a Completion section below when done. -->
