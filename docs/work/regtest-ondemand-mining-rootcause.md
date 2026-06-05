# Regtest on-demand mining (`generate N`) — layered root cause

**Status:** root-caused 2026-06-05 on an isolated regtest node (empirical, not
theoretical). The fix touches the **live shared header-admission path** →
owner-gated, prove-on-copy. Do NOT land it as a drive-by.

**Why it matters:** MVP #6 (7-day soak) and #7 (kill-9 teeth) both need a regtest
node whose tip advances on demand. Today `generate N` returns `[]` and the tip
never leaves genesis, so the `make test-crash-bootstrap` overshoot teeth report
`over=-1` (not-applicable) and `make soak-ci` has no synthetic block load.

## Symptom (reproduced)

Isolated regtest node (`tools/scripts/isolated_node_env.sh`), `generate 3`
returns `{"result":[],...}`; all three attempts mine the **same** block hash;
`getblockcount` stays at 0. Each block is rejected
`block-not-finalized-by-reducer` (now logged at `mining_submit_mined_block`,
commit `b56e645ca`).

## The layers (each is upstream of the next)

### Layer 1 — the mined block never reaches the block_index (THE first blocker)

`reducer_ingest_block` (`app/services/src/reducer_ingest_service.c`) pushes the
carried header with `msg.height = -1` (line ~273; comment claims "admit verifies
against the active chain regardless of the hint"). But
`handle_header_admit_msg` (`app/jobs/src/header_admit_stage.c:178`) early-returns
on `m->height < 0` **before** it stages the carried raw header (line ~184
`if (m->has_header) pending_header_stage(&m->header)`). So for a self-mined
block the raw header is never staged, `step_admit`'s `pending_header_take` never
creates the block_index entry, and `block_map_find(...)` returns NULL
(`ingested == nil`, confirmed via diagnostic). Per the design comment at
`reducer_ingest_service.c:259-261`, the carried-header staging is *supposed* to
CREATE the block_index for the mining path "without legacy accept_block_header".

**Why network sync is NOT broken by the same `-1`:** the live network intake
path (`app/services/src/chain_activation_service.c:434`) ALSO pushes
`has_header=true, height=-1`, but network blocks already have a block_index
entry created by `accept_block_header` on the receive path, so the early-return
is harmless for them. Mining has no such pre-creation — it is uniquely broken.

**Candidate fix (consensus-sensitive — shared path):** stage the carried header
*before* the `m->height < 0` validity guard, so `has_header=true` pushers create
the block_index via `step_admit` regardless of the height hint. `step_admit`
still validates the parent and recomputes height from it, so nothing skips
validation. BUT this now also stages network-path carried headers → must prove
(a) no duplicate/racy block_index creation vs `accept_block_header`, and (b)
mainnet sync is byte-identical. Prove on a datadir COPY, never live.

### Layer 2 — even with a block_index, tip_finalize cannot finalize a successor-less tip

(This is what the `regtest-finalize-trace` workflow analysed; it only bites once
Layer 1 is fixed.)

- `step_finalize` (`app/jobs/src/tip_finalize_stage.c:298-303`) requires a
  one-block **lookahead**: `active_chain_at(next_h+1)` must be non-NULL. A
  self-mined tip has no successor header → JOB_IDLE → never finalizes.
- Convention split: `step_finalize` writes "row H stores block H+1's hash"
  (Convention A, line 374), while `seed_anchor`/`set_authoritative_tip` write
  "row H stores block H's OWN hash" (Convention B, lines 89/590).
  `reducer_read_back_verdict` (`reducer_ingest_service.c:135-137`) reads
  Convention B (queries row `ingested->nHeight`, compares the block's own hash).

**Vetted Layer-2 fix (from the workflow + adversarial critique, GO/LOW-risk):**
in `reducer_ingest_block`, after the second `reducer_drain_to_convergence()` and
under the held mutex, regtest-gate (`ctl->params->fMineBlocksOnDemand`, true ONLY
for regtest) + validation-gate (`BLOCK_HAVE_DATA && !BLOCK_FAILED_MASK &&
utxo_apply_stage_succeeded_at(h)`) a call to
`tip_finalize_stage_set_authoritative_tip(ingested->nHeight, block_hash.data)`.
Use `set_authoritative_tip` (not `seed_anchor`): it routes through
`anchor_cursor_to_authority` whose monotonic guard cannot lower the finalize
cursor. Inline the gate — do NOT call `reducer_pending_body_is_accepted`, which
calls `validation_state_init(out)` and would clobber the caller's verdict.
**This fix is necessary but NOT sufficient — it is a no-op until Layer 1 makes
`ingested` non-NULL and `utxo_apply` actually runs for the mined height.**

## Required proof (all three, per the critique)

1. `test_reducer_ingest_e2e.c`, `fMineBlocksOnDemand=true`: `reducer_ingest_block`
   returns true, `active_chain_tip()->nHeight` advances by 1,
   `tip_finalize_stage_finalized_tip_at(pdb, h, out) == mined hash`.
2. **Negative twin**, `fMineBlocksOnDemand=false`: the new path does NOT fire
   (hard assertion — locks the gate against regression / mainnet drift).
3. Multi-block integration: `generate 3` → three DISTINCT hashes,
   `getblockcount==3`, and **no** spurious `reorg_detected_total` increment
   between blocks (guards the dual-convention objection).

Plus `make lint` + `make test_parallel` green, and the hermetic
`generate 3 → getblockcount==3` proof on an isolated regtest node.

## Deeper findings (fix attempted + reverted 2026-06-05)

Implementing L1 (stage the carried header *before* the `height<0` guard in
`handle_header_admit_msg`) + L2 (the regtest-gated `set_authoritative_tip`) and
re-probing showed the block was **still not admitted** (`ingested==nil`, no
`produce_block_index`/admit log lines) — so there is a layer **below** L1:

- **L0 — the stage pipeline does not admit the pushed header at all on an
  at-tip regtest boot.** The inbox drain that stages carried headers lives in
  `header_admit_stage_step_once` (`app/jobs/src/header_admit_stage.c:500`,
  `mailbox_header_admit_drain(handle_header_admit_msg)`), gated by
  `if (!g_stage) return JOB_IDLE` (line 490). The stages are init'd by
  `staged_sync_supervisor_register` (`config/src/boot_services.c:1455`), but a
  fresh regtest node boots straight `connecting→at_tip` and the boot smoke
  showed **no** `[header_admit] stage initialised` / `[tip_finalize] stage
  initialised (authoritative)` line — so the reducer stage pipeline appears
  inactive/idle for an at-tip node, meaning `reducer_drain_to_convergence()`
  inside `reducer_ingest_block` does no admit/apply work for the mined block.
  Confirm whether `g_stage` is NULL here; if so, on-demand mining needs the
  stage pipeline live even with nothing to sync. **This is the true first
  blocker and must be fixed before L1/L2 do anything.**
- **Separate pre-existing symptom:** the node goes RPC-unresponsive immediately
  after a `generate` call (getblockcount returns nothing) on BOTH the committed
  binary and the L1/L2 build — i.e. NOT introduced by the fix. The `generate`
  RPC likely holds a lock / blocks the single RPC servicing path during the
  synchronous drain. Investigate independently.

So the lane is broken at **≥3 layers** (L0 stage-pipeline-inactive → L1
header-staging-height-guard → L2 finalize-lookahead/read-back), plus the
RPC-unresponsive symptom. The L1+L2 changes were reverted (unproven, and a no-op
without L0). Next focused session: start at L0 — prove whether the reducer
stages are live for an at-tip regtest node, on a COPY/isolated node, before
touching the shared header-admission handler.

## Method

`tools/scripts/isolated_node_env.sh` gives a safe isolated `/tmp` regtest node
(non-live 39xxx ports, dead `-connect` sink). Reproduce with `generate N` and
grep `node.log` for the `[mining] ... REJECTED by reducer` line. Layer 1 must be
proven first; Layer 2 is dead code until then.
