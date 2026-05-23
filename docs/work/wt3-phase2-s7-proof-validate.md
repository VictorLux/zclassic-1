# wt3 Assignment — Phase 2: S-7 proof_validate shadow stage

**Worktree:** `~/github/zclassic23-3`
**Branch:** `wt3/phase2-proof-validate-shadow`
**Phase:** 2 (Wave S → S-12 cutover)
**Depends on:** S-6 script_validate (wt3-phase2-s6-script-validate, must be merged first)

**Owns:**
- NEW `app/services/include/services/proof_validate_stage.h`
- NEW `app/services/src/proof_validate_stage.c`
- NEW `lib/test/src/test_proof_validate_stage.c`
- Edits to `test.c`, `test_parallel.c`, `test_helpers.h`
- Edits to boot path (after `script_validate_stage_init`)
- Schema migration for `proof_validate_log` table

**MUST NOT touch:**
- `app/services/src/script_validate_stage.c` (just shipped)
- `app/services/src/body_persist_stage.c`, `body_fetch_stage.c`, `validate_headers_stage.c`
- `app/conditions/`, `app/controllers/`, `tools/mcp/` (wt2 owns)
- `lib/sapling/` — READ ONLY; you may NOT modify Sapling/Groth16/PHGR13 code
- `lib/framework/`, `lib/util/`, `lib/platform/`
- `docs/REFACTOR_STATUS.md`, `docs/FRAMEWORK.md`, `CLAUDE.md`

---

## Why this matters

S-7 is the second-to-last shadow stage before S-8 utxo_apply and
S-9 tip_finalize. After S-7, the chain-advance saga has 7 of 9 stages
verifying their step on every height — enough confidence in the saga
that we can begin the cutover (flipping the shadow stages to
authoritative).

S-7 verifies **zero-knowledge proofs**: Sapling Spend proofs (Groth16),
Sapling Output proofs (Groth16), Sprout JoinSplit proofs (Groth16 or
PHGR13 depending on activation height), and Sapling binding signatures.

---

## Behaviour spec

For each height H starting at `proof_validate_cursor + 1`:

1. **Floor:** if `H > script_validate_cursor` → STAGE_IDLE.
2. **Upstream log:** read `script_validate_log[H]`. If `ok=0`:
   log `proof_validate_log[H] = (status='upstream_failed', ok=0, ...)`,
   advance cursor.
3. **Block read:** look up the block at H. If body unavailable: STAGE_IDLE.
4. **For each shielded transaction in the block:**
   - For each Sapling spend: `groth16_verify_spend(...)`. Count verified, failed.
   - For each Sapling output: `groth16_verify_output(...)`. Count verified, failed.
   - For each Sprout JoinSplit: dispatch to Groth16 OR PHGR13 verify
     based on the activation height at H. Count verified, failed.
   - Verify the Sapling binding signature.
5. **Aggregate result:**
   - All proofs valid → log
     `(status='verified', ok=1, sapling_spends_n, sapling_outputs_n,
       sprout_joinsplits_n)`.
   - Any proof invalid → log
     `(status='proof_invalid', ok=0, ..., first_failure_txid,
       first_failure_proof_type)`, emit ERROR event.
   - Internal error (deserialize, missing key) → log
     `(status='internal_error', ok=0, ...)`, emit WARN event.
6. **Advance cursor** regardless of ok/fail (shadow records, doesn't act).

---

## Architecture reference

- Pattern: `app/services/src/script_validate_stage.c` (just shipped).
  Read it cover-to-cover first.
- Crypto entry points (READ-ONLY):
  - `lib/sapling/include/sapling/sapling.h` —
    `sapling_verify_spend`, `sapling_verify_output`,
    `sapling_verify_binding_sig` (or similar — grep to confirm signatures)
  - `lib/sapling/include/sapling/sprout.h` —
    `sprout_verify_groth16_joinsplit`, `sprout_verify_phgr13_joinsplit`
  - `lib/consensus/include/consensus/upgrades.h` —
    `consensus_uses_phgr13_at_height(H)` (or similar predicate)
- Workpool: use `lib/util/include/util/workpool.h` if the existing
  code paths already use it (grep). Proofs parallelize naturally but
  the SHADOW stage doesn't need to — single-threaded is fine.

---

## Tasks (in order)

### Task 1: Schema migration `proof_validate_log`

```sql
CREATE TABLE IF NOT EXISTS proof_validate_log (
    height                   INTEGER PRIMARY KEY,
    status                   TEXT    NOT NULL,
    ok                       INTEGER NOT NULL,
    sapling_spends_total     INTEGER NOT NULL,
    sapling_outputs_total    INTEGER NOT NULL,
    sprout_joinsplits_total  INTEGER NOT NULL,
    first_failure_txid       BLOB,
    first_failure_proof_type TEXT,    -- 'sapling_spend' | 'sapling_output'
                                       -- | 'sprout_groth16' | 'sprout_phgr13'
                                       -- | 'binding_sig'
    validated_at             INTEGER NOT NULL
);
```

Bump the migration version. Add to whatever file S-5/S-6 used.

### Task 2: `proof_validate_stage.h`

```c
bool proof_validate_stage_init(struct main_state *ms);
void proof_validate_stage_shutdown(void);
bool proof_validate_dump_state_json(struct json_value *out, const char *key);
```

### Task 3: `proof_validate_stage.c`

Mirror `script_validate_stage.c` structure. Counters:
- `g_verified_total`, `g_proof_invalid_total`, `g_internal_error_total`,
  `g_upstream_failed_total`
- Per-proof-type totals:
  `g_sapling_spends_verified_total`, `g_sapling_spends_failed_total`,
  `g_sapling_outputs_verified_total`, `g_sapling_outputs_failed_total`,
  `g_sprout_groth16_verified_total`, `g_sprout_groth16_failed_total`,
  `g_sprout_phgr13_verified_total`, `g_sprout_phgr13_failed_total`,
  `g_binding_sig_verified_total`, `g_binding_sig_failed_total`

Discipline reminders:
- `platform_time_wall_unix()`, no raw clock
- AR_* macros for sqlite_step in app code
- Wire `proof_validate_dump_state_json` into `diagnostics_controller.c:g_dumpers`
- Add `proof_validate` to `zcl_state` enum_csv in
  `tools/mcp/controllers/ops_controller.c` and to the assertion in
  `lib/test/src/test_mcp_controllers.c`

### Task 4: Wire init in boot path

After `script_validate_stage_init(ms)`, add `proof_validate_stage_init(ms)`.

### Task 5: Test `test_proof_validate_stage.c`

Mirror `test_script_validate_stage.c`. Test cases:
- Happy path: 2 blocks with valid Sapling + Sprout txs → all `ok=1`.
- Sapling spend invalid: tamper a spend proof → `status='proof_invalid',
  first_failure_proof_type='sapling_spend'`.
- Sapling output invalid: same for output.
- Sprout JoinSplit invalid (Groth16 path).
- Sprout JoinSplit invalid (PHGR13 path, pre-activation height).
- Binding sig invalid.
- Upstream failure propagates.
- Internal error path.
- Idle: H > script_validate_cursor.

Generating real proofs for tests is expensive — use fixed test vectors
from `lib/test/data/` if they exist, OR use the existing `test_sapling.c`
fixtures.

### Task 6: Final verify + push

```bash
make -j$(nproc)
make lint
./test_parallel --jobs=$(nproc)
git push origin wt3/phase2-proof-validate-shadow
```

Append Completion section.

---

## Commit cadence

One commit per task. Push after tasks 2, 4, 5.

---

## Status

**READY** — gated on S-6 merge. Start when human invokes you in
`~/github/zclassic23-3` AFTER S-6 is merged into main.

<!-- Worker: append a Completion section below when done. -->
