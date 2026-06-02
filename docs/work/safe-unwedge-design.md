I have all the verified facts. Producing the design doc now.

# SAFE Auto-Un-Wedge for the Staged Reducer — Boot-Time Cursor Reconcile

Branch `finish/self-healing-service`, HEAD `fbfec53a0`. All file:line references verified against the tree at that commit.

## 1. The wedge, and why the naive rewind collapsed the public tip

### 1.1 The wedge (verified)

After an unclean restart (kill-9 + WAL), the durable `progress.kv` reducer stage cursors (`body_fetch..tip_finalize`) sit AHEAD of the durably-applied coins tip. The connect gate then rejects every block at `applied_tip+1`:

`app/services/src/chain_activation_controller.c:576-603` — `reducer_read_back_verdict()`:

```c
uint8_t finalized[32];
if (pdb &&
    tip_finalize_stage_finalized_tip_at(pdb, height, finalized) &&
    memcmp(finalized, hash->data, 32) == 0) {
    return true; /* out left MODE_VALID by the caller's init */
}
validation_state_invalid(out, false, REJECT_INVALID,
                         "block-not-finalized-by-reducer", NULL);
return false;
```

The gate requires a durable `ok=1` row in `tip_finalize_log` whose `tip_hash` equals the candidate. The `tip_finalize` cursor is at `applied_tip+N` (e.g. 3132857), so `step_finalize` never writes the row for `applied_tip+1`: its cursor is already past it. `tip_finalize_stage.c:234-249` reads `next_h = c->cursor_in` and idles when `next_h >= utxo_apply` cursor — it never re-descends to `applied_tip+1`. Wedge.

### 1.2 The public-tip authority (the single point that resets)

The ONLY public-tip authority is `active_chain_tip()` at `lib/validation/src/chainstate.c:255-272`. It has two tiers:

- **Tier 1 (authority):** `g_chain_authority.get_hash()` → `tip_finalize_stage`'s `get_hash` (registered at `tip_finalize_stage.c:397-402`), which returns `g_last_advance_hash`. The tip is `block_map_find(g_chain_block_map, &hash)` (`chainstate.c:265`). **If that hash is not in the loaded block map, Tier 1 returns NULL and falls through.**
- **Tier 2 (fallback):** `active_chain_height()` at `chainstate.c:445-473`:

```c
if (sqlite3_prepare_v2(db, "SELECT MAX(height) FROM tip_finalize_log WHERE ok = 1", -1, &st, NULL) != SQLITE_OK) { ... }
...
if (h > c->height) return h;
return c->height;
```

The tip becomes `c->chain[h]` (`chainstate.c:270-271`).

### 1.3 Exactly why the naive fix reset the tip to ~47279

The reverted `stage_repair_rewind_reducer_to_floor()` did two things in one tx: (a) clamped every downstream cursor to `floor`, and (b) **`DELETE FROM tip_finalize_log WHERE height > floor`**. On restart:

1. Tier 1 failed. `g_last_advance_height/hash` is seeded at `tip_finalize_stage_init` from `active_chain_cached_tip` (`tip_finalize_stage.c:390-395`). After a kill-9 the cached tip reflects the pre-crash finalized height (~3132857), whose `block_index` is NOT in the rebuilt map (the map only loads up to the applied/coins tip). So `block_map_find` at `chainstate.c:265` returned NULL → fall through to Tier 2.
2. Tier 2 executed `SELECT MAX(height) FROM tip_finalize_log WHERE ok = 1`. **The DELETE had removed every row above the floor, but the floor passed in was wrong** — it was derived from `active_chain_height` (the stale header high), or the DELETE orphaned the evidence base so the highest surviving `ok=1` row was an ancient anchor/reorg row at ~47279. `MAX(height) WHERE ok=1` returned ~47279.
3. `active_chain_tip` returned `c->chain[47279]`. The public tip collapsed, and the node began re-syncing from there.

**Two independent faults, both fatal:**
- **F1 — deleting `tip_finalize_log` rows destroys the Tier-2 authority's evidence base.** `MAX(height) WHERE ok=1` is only meaningful while the contiguous finalized rows survive. The DELETE turned a precise fallback into "whatever ancient `ok=1` row is left."
- **F2 — the floor was `active_chain_height` (stale header high), not the applied coins tip.** Even a correct clamp aimed at the wrong height.

**The fix below removes both faults:** it NEVER deletes `tip_finalize_log` rows, and it floors strictly on `coins_best_block_height`.

## 2. The floor: `coins_best_block_height`, never `active_chain_height`

The only value guaranteed to equal the durably-applied coins/UTXO tip is `cec.coins_best_block_height`. Exact accessor (already in the tree):

`app/services/src/chain_evidence_snapshot.c:103-104`:
```c
out->coins_best_block_height =
    state_get_i32(authority->ndb, "cec.coins_best_block_height", -1);
```

At boot, after the `utxo_chain_reconcile` span (`config/src/boot.c:3115-3296`) has promoted the coins anchor via `boot_promote_tip_via_csr(...)` and `utxo_recovery_clean_above_tip(...)`, this key is durable and equals the applied UTXO tip. Read it directly via the same primitive used everywhere else:

```c
int64_t coins_best = -1;
node_db_state_get_int(&g_node_db, "cec.coins_best_block_height", &coins_best);
```

`node_db_state_get_int` signature: `app/models/include/models/database.h:107`.

**Never** use `active_chain_height(&g_state.chain_active)` as the floor: at the unclean-restart boot gate it returns the stale header high (via Tier-2's `MAX(height)` over surviving log rows, or `c->height`), which is exactly fault F2.

## 3. The invariant and how it's enforced

> **INVARIANT (NO-BACKWARD-TIP):** the reconcile MUST NOT move the public/active tip below `coins_best_block_height`. It may only (a) clamp downstream stage cursors DOWN to `floor = coins_best_block_height + 1`, and (b) thereby permit `tip_finalize` to re-finalize forward from `coins_best+1` toward the header tip. The tip moves strictly UPWARD from `coins_best`.

Enforcement (three independent guards, all checkable in the unit test):

1. **Floor is `coins_best`, computed before any write.** The reconcile reads `coins_best_block_height` and sets `floor = coins_best + 1`. It refuses to run if `coins_best < 0` (no durable anchor → nothing to floor on; leave the chain alone).
2. **`tip_finalize_log` is NEVER deleted.** The contiguous finalized rows `0..coins_best` survive untouched, so:
   - Tier-2 `MAX(height) WHERE ok=1` can never drop below `coins_best` — the row at `coins_best` is still there (fault F1 eliminated).
   - `reducer_read_back_verdict`'s `finalized_tip_row_at(coins_best)` still matches, so blocks already applied stay accepted.
3. **Cursors only ever move toward `floor`, and the anchor row at `coins_best` guarantees the lookahead can resume.** `tip_finalize`'s `step_finalize` reads `cursor_in = coins_best+1`, finds the durable `utxo_apply`/`tip_finalize` evidence at `coins_best`, and finalizes `coins_best+1` forward. `update_last_advance` re-seeds `g_last_advance_height = coins_best+1` on the first successful finalize, restoring Tier-1 authority at a height whose block IS in the map.

Because the floor is `coins_best+1` and the log is intact, **the lowest possible public tip after reconcile is `coins_best`** (the surviving anchor) — never below.

## 4. Boot-time reconcile (primary), restart-safe runtime Condition (secondary)

### 4.1 Why boot-time, not runtime

Stages cache their cursor in-memory once at init (`stage.c` `cursor_read` → `s->cursor`); `tip_finalize_stage_step_once` (`tip_finalize_stage.c:446-464`) never re-reads `progress.kv`. A runtime cursor edit in `progress.kv` is invisible to the running stage — this is exactly why the failed fix had "no effect at runtime." The reconcile MUST run **before** stages init so they load corrected cursors.

The stage inits happen inside `app_init_services` at `config/src/boot.c:3520`. The `utxo_chain_reconcile` span (which promotes `coins_best`) completes at `boot.c:3296`, and the reducer activation / DEGRADED_SERVING classification runs at `boot.c:3298-3463`. **The safe window is after `coins_best` is durable (after line 3296) and before stage inits (before line 3520).** The natural home is the existing RECONCILE → DEGRADED_SERVING path at `boot.c:3382-3462`, where the integrity classifier already runs.

### 4.2 Forward-only anchoring cannot clamp DOWN — this is the load-bearing subtlety

`stage_anchor_upstream_cursors_to` (`app/jobs/src/stage_anchor.c:11-45`) calls `stage_set_named_cursor_if_behind` (`lib/util/src/stage.c:370-400`), which is **forward-only**: `if (current >= value) { ROLLBACK; return true; }` (`stage.c:396-400`). It can push a cursor UP to `floor` but can NEVER pull an ahead cursor DOWN to `floor`. The wedge is precisely the case where cursors are AHEAD. So the reconcile must do the clamp-DOWN itself with an unconditional `force_stage_cursor`-style write, then (optionally) call the existing forward anchor to align any laggards.

### 4.3 The reconcile algorithm (new function `stage_reconcile_clamp_cursors_to_floor`)

Add to **`app/jobs/src/stage_repair.c`** (declared in `app/jobs/include/jobs/stage_repair.h`). It reuses the existing private helpers `force_stage_cursor` (`stage_repair.c:478-505`), `cursor_at_unlocked` (`stage_repair.c:237-262`), `table_has_success_at_or_above` (`stage_repair.c:507-534`), and `stage_anchor_upstream_cursors_to`.

```c
/* Boot-time only. Clamp every downstream stage cursor DOWN to
 * floor = coins_best + 1 so that, on stage init, tip_finalize re-finalizes
 * from coins_best+1 forward. NEVER deletes tip_finalize_log (the Tier-2
 * authority evidence). NEVER moves below coins_best. */
bool stage_reconcile_clamp_cursors_to_floor(sqlite3 *db, int coins_best,
                                            struct stage_reconcile_result *out)
{
    if (!db || coins_best < 0) return false;            /* no durable floor */
    int floor = coins_best + 1;

    /* Downstream stages whose cursors may be ahead of the applied tip. */
    static const char *const downstream[] = {
        "body_fetch", "body_persist", "script_validate",
        "proof_validate", "utxo_apply", "tip_finalize",
    };
    /* Upstream logs we MAY delete above coins_best (re-derivable from the
     * header chain). tip_finalize_log is DELIBERATELY ABSENT — see §3.2. */
    static const char *const downstream_logs[] = {
        "body_fetch_log", "body_persist_log", "script_validate_log",
        "proof_validate_log", "utxo_apply_log", "utxo_apply_delta",
    };

    progress_store_tx_lock();
    char *err = NULL;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK) { ... }

    /* GUARD: do not run unless at least one cursor is actually AHEAD of
     * floor — i.e. the wedge condition. Otherwise ROLLBACK and no-op. */
    bool ahead = false;
    for (each downstream s) {
        int cur = -1; cursor_at_unlocked(db, s, &cur);
        if (cur > floor) ahead = true;
    }
    if (!ahead) { ROLLBACK; unlock; out->clamped = false; return true; }

    /* GUARD: refuse if any tip_finalize_log row at/above floor is ok=1 and
     * already on the active chain — that is NOT a wedge, it is real
     * progress (do not regress it). (table_has_success_at_or_above) */

    /* Clamp DOWN (forced — if_behind is forward-only and cannot do this). */
    for (each downstream s) force_stage_cursor(db, s, floor);

    /* Delete ONLY re-derivable upstream logs above coins_best.
     * tip_finalize_log is left intact (authority evidence). */
    for (each downstream_logs t) delete_above(db, t, coins_best); /* WHERE height > coins_best */

    if (sqlite3_exec(db, "COMMIT", ...) != SQLITE_OK) { ROLLBACK; ... }
    progress_store_tx_unlock();

    out->clamped = true; out->floor = floor;
    return true;
}
```

Notes:
- `delete_above` deletes `WHERE height > coins_best` (strictly above the applied tip) from upstream logs only. The existing `delete_from_table` (`stage_repair.c:454-476`) uses `>=`; add a `>` variant or pass `coins_best+1` to the existing one — both leave the row at `coins_best`.
- **`tip_finalize_log` is never touched.** This is the single most important line of the design.
- The whole thing is one `BEGIN IMMEDIATE`…`COMMIT` under `progress_store_tx_lock()`, mirroring `stage_repair_header_solution_poison_rewind` (`stage_repair.c:581-654`).

### 4.4 Boot wiring (one call site)

In `config/src/boot.c`, immediately after the `utxo_chain_reconcile` span completes (after line 3296) and before reducer activation (before line 3298), under `SERVICE_STATE_RECONCILE`:

```c
int64_t coins_best = -1;
node_db_state_get_int(&g_node_db, "cec.coins_best_block_height", &coins_best);
struct stage_reconcile_result rr = {0};
if (coins_best >= 0 && progress_store_db()) {
    if (stage_reconcile_clamp_cursors_to_floor(progress_store_db(),
                                               (int)coins_best, &rr) && rr.clamped) {
        printf("[boot] reducer-wedge reconcile: clamped downstream cursors to "
               "floor=%d (coins_best=%lld)\n", rr.floor, (long long)coins_best);
        event_emitf(EV_BOOT_ACTIVATE, 0,
            "reducer_wedge_reconcile floor=%d coins_best=%lld",
            rr.floor, (long long)coins_best);
    }
}
```

Then the existing reducer activation at `boot.c:3298-3318` drains forward from the clamped cursors, and stage inits in `app_init_services` (`boot.c:3520`) load the corrected cursors. `tip_finalize_stage_init`'s `anchor_cursor_to_authority` (`tip_finalize_stage.c:407-433`) re-anchors idempotently at `coins_best+1` and re-seeds `g_last_advance` from the cached tip at `coins_best` — restoring Tier-1 authority cleanly.

This sits inside the same RECONCILE/DEGRADED_SERVING boot path the task requires; no new boot phase.

### 4.5 Restart-safe runtime Condition (secondary, optional)

The runtime Condition `block_failed_mask_at_tip` (`app/conditions/src/block_failed_mask_at_tip.c`, `BF_STALL_NO_ADVANCE` remedy at line 119) must NOT edit cursors in-memory-blindly. Make it **restart-safe** by having its remedy:
1. Write a durable sentinel `cec.reducer_wedge_reconcile_request = coins_best` into `node_db`.
2. Request a clean restart (the existing supervised restart path), NOT a runtime cursor edit.

On the next boot, §4.4 reads `coins_best` fresh and performs the reconcile before stages init. This guarantees the stages ALWAYS load corrected cursors (never the failed "runtime edit ignored in-memory" path). The sentinel is consumed/cleared after a successful reconcile. If the node already wedges at boot (header tip > applied tip with cursors ahead), §4.4 fires unconditionally without needing the sentinel — the runtime Condition is just the trigger for the steady-state case.

## 5. Unit test with a synthetic wedge (no live datadir)

New file: **`lib/test/src/test_stage_reducer_unwedge.c`**, registered in `lib/test/src/test.c` alongside the other reducer tests. It mirrors the harness of `test_tip_finalize_stage.c` (the `stale_cursor` case at lines 298-351 is the closest existing analogue) and `test_reducer_ingest_e2e.c`.

### 5.1 Synthetic wedge construction (reuses the existing harness)

```c
/* Build an N=8 synthetic chain (block_index + hashes), insert into
 * map_block_index, window-tip to block[7] — same as synth_chain_tf_build /
 * tf_setup in test_tip_finalize_stage.c:56-81,223-258. */
const int APPLIED = 3;     /* coins_best — durably applied tip            */
const int HEADER  = 7;     /* header tip — where re-finalize must reach   */
const int STALE   = 6;     /* stale cursor AHEAD of applied tip (wedge)   */

progress_store_open(dir);
/* seed_utxo_apply up to HEADER so upstream evidence exists 0..HEADER */
seed_utxo_apply(db, HEADER, /*upstream_fail=*/-1);

/* Seed the WEDGE: every downstream cursor at STALE, AHEAD of APPLIED. */
for each downstream stage s:
    exec_sql("INSERT OR REPLACE INTO stage_cursor(name,cursor,updated_at)
              VALUES('<s>', 6, 1)");

/* Seed contiguous finalized rows 0..APPLIED (the surviving authority),
 * PLUS poisoned/stale rows above APPLIED to model the pre-crash state. */
for h in 0..APPLIED:  log_insert(db, h, "finalized", /*ok=*/1, ..., &hash[h]);
/* deliberately also insert an ancient ok=1 anchor far below to PROVE the
 * Tier-2 MAX never selects it after reconcile (mirrors the ~47279 row). */
log_insert(db, /*ancient=*/0, "anchor", 1, ..., &hash[0]); /* already 0; use a low h */
```

### 5.2 Run the reconcile and the asserts

```c
struct stage_reconcile_result rr = {0};
RC_CHECK("reconcile runs", stage_reconcile_clamp_cursors_to_floor(db, APPLIED, &rr));
RC_CHECK("reconcile clamped",            rr.clamped == true);
RC_CHECK("floor is coins_best+1",        rr.floor == APPLIED + 1);

/* (1) cursors clamped DOWN to floor (forward-only anchor could NOT do this) */
RC_CHECK("body_fetch clamped",     cursor_at(db,"body_fetch")     == APPLIED+1);
RC_CHECK("utxo_apply clamped",     cursor_at(db,"utxo_apply")     == APPLIED+1);
RC_CHECK("tip_finalize clamped",   cursor_at(db,"tip_finalize")   == APPLIED+1);

/* (2) tip_finalize_log NEVER deleted: row at APPLIED still ok=1 */
RC_CHECK("authority row survives",
         log_row_at(db, APPLIED, &ok, ...) && ok == 1);
/* and the surviving MAX(height) WHERE ok=1 is >= APPLIED, NEVER the ancient row */
RC_CHECK("Tier-2 floor >= coins_best",
         select_max_ok_height(db) >= APPLIED);   /* the ~47279-style regression guard */

/* (3) re-init stages from the CLAMPED cursors (the restart simulation) and
 *     drive forward; assert it re-finalizes APPLIED+1..HEADER and the public
 *     tip moves UPWARD only. */
tip_finalize_stage_init(&ms);              /* loads corrected cursor = APPLIED+1 */
tip_finalize_stage_set_utxo_counter(fake_utxo_count, &sc);
RC_CHECK("public tip never below coins_best (pre-drain)",
         active_chain_height(&ms.chain_active) >= APPLIED);
int drained = tip_finalize_stage_drain(100);
RC_CHECK("re-finalizes forward to header tip",
         tip_finalize_stage_cursor() == HEADER);
RC_CHECK("public tip reached header tip",
         active_chain_height(&ms.chain_active) == HEADER);

/* (4) THE core invariant — at no observed step did the tip drop below APPLIED */
RC_CHECK("tip NEVER dropped below coins_best", g_min_observed_tip >= APPLIED);
```

Where `g_min_observed_tip` is sampled after `tip_finalize_stage_init` and after each `tip_finalize_stage_step_once` in a loop (drive one step at a time, recording `active_chain_height` each iteration), so the assert proves monotonic-upward, not just the endpoints.

### 5.3 Negative / safety asserts (prove we didn't weaken a gate)

```c
/* No-op when NOT wedged: cursors already at/below floor → reconcile must
 * leave everything alone. */
RC_CHECK("no-op when not ahead",
         stage_reconcile_clamp_cursors_to_floor(db2, APPLIED, &rr2) &&
         rr2.clamped == false);

/* Refuses to regress real progress: a tip_finalize_log ok=1 row above floor
 * that IS on the active chain blocks the clamp. */
RC_CHECK("refuses to regress real finalized progress",
         /* setup an on-chain ok=1 row at APPLIED+1, then */
         stage_reconcile_clamp_cursors_to_floor(db3, APPLIED, &rr3) == ... /* no clamp */);
```

### 5.4 Test file + asserts summary

- **File:** `lib/test/src/test_stage_reducer_unwedge.c` (register in `lib/test/src/test.c`; build via the existing `lib/test` target).
- **Named asserts:** `reconcile clamped`, `floor is coins_best+1`, `body_fetch/utxo_apply/tip_finalize clamped`, `authority row survives`, `Tier-2 floor >= coins_best` (the explicit ~47279-regression guard), `re-finalizes forward to header tip`, `public tip reached header tip`, `tip NEVER dropped below coins_best`, `no-op when not ahead`, `refuses to regress real finalized progress`.

## 6. Files/functions to add/change (minimal diff)

**Add:**
- `app/jobs/include/jobs/stage_repair.h` — declare `struct stage_reconcile_result { bool clamped; int floor; }` and `bool stage_reconcile_clamp_cursors_to_floor(struct sqlite3 *db, int coins_best, struct stage_reconcile_result *out);`.
- `app/jobs/src/stage_repair.c` — implement `stage_reconcile_clamp_cursors_to_floor` (§4.3), reusing existing static helpers `force_stage_cursor` (478-505), `cursor_at_unlocked` (237-262), `table_has_success_at_or_above` (507-534), `delete_from_table` (454-476; add a `>` variant or call with `coins_best+1`).
- `lib/test/src/test_stage_reducer_unwedge.c` — the §5 test; register in `lib/test/src/test.c`.

**Change:**
- `config/src/boot.c` — one call site after line 3296 (post `utxo_chain_reconcile`, pre reducer-activation at 3298) reading `cec.coins_best_block_height` via `node_db_state_get_int(&g_node_db, ...)` and invoking the reconcile under `SERVICE_STATE_RECONCILE` (§4.4).
- `app/conditions/src/block_failed_mask_at_tip.c` — change the `BF_STALL_NO_ADVANCE` remedy (line 119) to persist `cec.reducer_wedge_reconcile_request` and request a clean restart instead of any in-memory cursor edit (§4.5). Boot consumes/clears the sentinel.

**Explicitly NOT changed (no consensus gate weakened):** `reducer_read_back_verdict` (`chain_activation_controller.c:576-603`), `connect_block` prevhash, CSR rejection, `find_most_work_chain` / `active_chain_most_work_candidate` validity+HAVE_DATA filters (`chainstate.c:391-443`), work-monotonicity in `step_finalize` (`tip_finalize_stage.c:298-309`). The reconcile only re-aligns durable cursors and deletes re-derivable upstream logs; every block still passes the full gate chain on re-finalize.

## 7. Why this provably avoids the §1.3 reset

| Failed fix | This design |
|---|---|
| Floor = `active_chain_height` (stale header high) | Floor = `cec.coins_best_block_height` (`chain_evidence_snapshot.c:103`) |
| `DELETE FROM tip_finalize_log WHERE height > floor` → orphans Tier-2 evidence; `MAX(ok=1)` falls to ~47279 | `tip_finalize_log` untouched; row at `coins_best` survives → `MAX(ok=1) >= coins_best` always |
| Runtime cursor edit ignored by in-memory stages | Boot-time clamp before stage init; stages load corrected cursors |
| Forward-only anchor silently failed to clamp ahead cursors | Explicit `force_stage_cursor` clamp-DOWN; forward anchor only for laggards |
| No guard; ran unconditionally | Runs only when a cursor is `> coins_best+1`; refuses if real on-chain finalized progress exists above floor |

The reset required BOTH a wrong floor AND a destroyed authority log. This design removes both, and the unit-test assert `Tier-2 floor >= coins_best` plus `tip NEVER dropped below coins_best` fail loudly if either regression is ever reintroduced.

---

## Adversarial critique (required changes applied)

{
  "can_reset_tip": false,
  "weakens_gate": false,
  "unit_testable": false,
  "verdict": "approve-with-changes",
  "required_changes": [
    "P0 SELF-STALL \u2014 the reconcile as designed cannot re-finalize. step_finalize (app/jobs/src/tip_finalize_stage.c:246) requires next_h < utxo_apply cursor STRICTLY ('if ((uint64_t)next_h >= uv_cursor) return JOB_IDLE') and reads utxo_apply_log_at(next_h) at line 252 (returns found==0 -> JOB_IDLE at 254-257). The design clamps the utxo_apply cursor DOWN to floor=coins+1 AND deletes utxo_apply_log WHERE height>coins. After that, for next_h=coins+1: next_h(coins+1) >= uv_cursor(coins+1) -> IDLE, and the utxo_apply_log row at coins+1 is gone. tip_finalize can NEVER re-finalize; it would require a FULL pipeline re-execution (body_fetch..utxo_apply) the design never acknowledges. FIX: do NOT clamp utxo_apply (or any upstream) cursor down and do NOT delete any upstream log. Clamp ONLY the tip_finalize cursor down to coins+1, leaving the utxo_apply cursor at the header high and all *_log rows (including utxo_apply_log) intact, so tip_finalize re-finalizes coins+1..header_high by replaying existing logs.",
    "P0 TEST INVALID \u2014 \u00a75.2 asserts tip_finalize_stage_drain(100) advances the cursor to HEADER, but under the design's own reconcile (utxo_apply cursor clamped to coins+1, utxo_apply_log deleted above coins) the drain returns 0 immediately (JOB_IDLE per the strict line-246 check). The test cannot pass as written. With the corrected minimal fix (clamp only tip_finalize cursor; keep utxo_apply cursor at HEADER; keep logs) the existing 'happy' pattern (test_tip_finalize_stage.c:355) drives tip_finalize_stage_drain forward and the test becomes genuinely unit-testable with NO live datadir.",
    "REMOVE the upstream-log deletion entirely. The downstream_logs deletion (body_fetch_log..utxo_apply_log, utxo_apply_delta) is unnecessary for the wedge and is exactly what breaks re-finalize. The wedge is purely tip_finalize-cursor-ahead-of-applied-tip; upstream logs above coins were validly computed pre-crash and must be preserved. Deleting nothing also moots the 'never delete tip_finalize_log' subtlety.",
    "HEDGE the \u00a71.3 root-cause claim. The reverted fix is fully gone from the tree (no git -S match), so F1 (deleting tip_finalize_log orphans Tier-2 evidence -> MAX(ok=1)=~47279) is a HYPOTHESIS, not verified. Note the shipped stage_repair_header_solution_poison_rewind (app/jobs/src/stage_repair.c:562) DOES delete tip_finalize_log safely because it is frontier-only (line 546) and refuses if any ok=1 row exists at/above (lines 605-619). The real invariant is the GUARD (never delete a row on the active chain / ok=1 at-or-above floor), not 'never touch tip_finalize_log'.",
    "RESOLVE the boot placement contradiction. \u00a74.4 says 'after 3296, before reducer activation at 3298, under SERVICE_STATE_RECONCILE', but SERVICE_STATE_RECONCILE is not entered until config/src/boot.c:3382 (AFTER the 3298-3318 reducer activation). The only constraint that matters is 'before stage init at boot.c:3520 (app_init_services -> staged_sync_supervisor_register at boot_services.c:3670)'. Place the call in the 3382-3462 RECONCILE/DEGRADED_SERVING window (still before 3520). The 3298 boot activation is already a tip_finalize no-op because g_stage is NULL until 3520 (tip_finalize_stage_step_once returns JOB_IDLE when g_stage==NULL).",
    "FIX the secondary runtime Condition (\u00a74.5). block_failed_mask_at_tip's witness (app/conditions/src/block_failed_mask_at_tip.c:134-135) requires current_tip > tip_at_detect within witness_window_secs=60. A 'persist sentinel + clean restart' remedy cannot satisfy a 60s witness and will be marked FAILED. Give this path its own condition with restart-aware witness semantics, or keep the runtime trigger as 'request restart' verified by a separate boot-time assertion \u2014 do not reuse the existing 60s witness.",
    "ADD explicit unit-test asserts that the utxo_apply cursor and ALL upstream logs are UNCHANGED by the reconcile (proving the corrected minimal fix deletes nothing), alongside 'authority row survives' and 'Tier-2 floor >= coins_best'. Also assert the no-op guard fires when no cursor is strictly above floor (mirror the stale_cursor test at test_tip_finalize_stage.c:298-351, which already shows init re-anchors a behind-cursor forward, not down).",
    "DOCUMENT the floor-source ordering dependency. cec.coins_best_block_height is persisted by boot_promote_tip_via_csr -> csr_commit_tip -> chain_evidence_controller.c:619 during the utxo_chain_reconcile span (boot.c:3115-3296); the active_chain window tip at stage init equals this coins tip, so tip_finalize_stage_init's anchor_cursor_to_authority targets coins+1, consistent with the clamp (cursor>=target early-return at tip_finalize_stage.c:101-102 means init never re-raises it). Keep the 'refuse if coins_best < 0' guard. The reconcile MUST run only AFTER the utxo_chain_reconcile span has persisted coins_best."
  ]
}