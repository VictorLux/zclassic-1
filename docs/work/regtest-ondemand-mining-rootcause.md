# Regtest on-demand mining (`generate N`) — layered root cause

> **STATUS 2026-06-05:** L0 shipped (`06ae18c26`). L1 + L2.5 + drain-ordering +
> L2 LANDED (`2cf7fa215`, union-gate + repro-on-copy green, network-safe). The
> pipeline now flows (validate_headers passes, body-absent cascade gone).
> **REMAINING red blocker:** `utxo_apply` still records ok=0 NONDETERMINISTICALLY
> — the failing upstream stage varies run-to-run (body_persist / proof_validate)
> — caused by active-chain WINDOW extend/collapse timing across the synchronous
> drain (`active_chain_at(next_h)` resolves or returns NULL depending on whether
> `reducer_extend_window_to_candidate` has run / been collapsed when a given
> stage executes). Next: make the window stable for the height under ingest
> across the whole synchronous drain (e.g. extend once before the full drain and
> keep it covering the candidate, or have the body/script/proof/utxo stages
> resolve via the same above-window path validate_headers uses). Deep
> reducer-internals; prove on the isolated node + union gate + repro-on-copy.


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
without L0).

### L0 root cause CONFIRMED (2026-06-05) — boot blocks before reducer-stage init

Instrumented `header_admit_stage_step_once` and `staged_sync_supervisor_register`
and probed the isolated node:

- `header_admit` `g_stage == (nil)` — the stage is **never initialized**.
- `staged_sync_supervisor_register`'s entry log **never fires** — the register
  (which calls every stage's `init(ms)` synchronously) is never reached.
- Boot-marker trace: `app_init_services` (`config/src/boot_services.c:565`,
  called from `config/src/boot.c:3582`) logs `svc.frontend_tor_start` (line
  1342) then prints `NAT: gateway 74.50.74.101 via enp1s0f0` — emitted *inside*
  `peer_strategy_discover_self` (line 1349) → `nat_add_port_mapping`
  (`lib/net/src/peer_strategy.c:39`) — but **never** prints `Reachability:`
  (line 1359, immediately after the call) and never reaches
  `staged_sync_supervisor_register` (line 1455) or `svc.peers_supervisors_runtime`
  (line 1522).

**Bottom line: boot HANGS in `nat_add_port_mapping` (a NAT-PMP/UPnP probe to an
unresponsive datacenter gateway), which is sequenced BEFORE the reducer-stage
init. The frontend/RPC thread already started (line 1342), so the node answers
RPC while `app_init_services` is wedged — masking the stall.** Because the
stages never init, every block (network or mined) that routes through
`reducer_drain_to_convergence` does nothing; the mined block is never admitted.

This is a **boot-robustness bug**: core consensus-engine initialization must not
be sequenced behind an optional, unbounded network reachability probe. The live
mainnet node boots past this (its gateway answers / times out fast); this
datacenter host's isolated harness hangs.

**Candidate fixes (live-boot — owner-gated, needs a mainnet boot proof on a
COPY):**
1. **Reorder:** run `staged_sync_supervisor_register` (+ the reducer-stage init)
   BEFORE `peer_strategy_discover_self`, so the consensus engine is always live
   regardless of NAT latency. Move the optional reachability/announce block
   (boot_services.c:1346-1403) to after the register, OR
2. **Bound/background** `nat_add_port_mapping` (a hard timeout, or run discovery
   on a detached thread) so it can never block boot.

Option 1 is the architecturally-correct fix (consensus init independent of
network reachability). Both touch the LIVE boot sequence → prove on a datadir
COPY that mainnet boot + sync are unchanged before deploy.

### L0 FIXED (2026-06-05, commit `06ae18c26`)

`peer_strategy_discover_self` now early-returns on `fMineBlocksOnDemand`
(regtest only; main/testnet byte-identical) so boot no longer wedges on the
UPnP probe. Placed inside `lib/net/src/peer_strategy.c` (NOT the frozen
`boot_services.c`, which is at its size ceiling). Proven: isolated regtest node
boots past discovery and initialises the reducer stages (`header_admit g_stage`
non-NULL, "stage initialised" logs appear — was NULL); lint 35/35;
test_parallel 0/371; repro-on-copy boots the real mainnet datadir to the
documented light-copy floor with no crash.

### L1 + L2 VERIFIED on a throwaway build, then reverted (dead until L2.5)

With L0 in place, applying L1 (stage carried header before the `height<0` guard)
+ L2 (regtest-gated `set_authoritative_tip`) and probing showed:
- **L1 works:** the carried header is now staged (`[L1-DIAG] inbox msg
  has_header=1 height=-1 staged`) and `produce_block_index` creates the
  block_index at height 1 (`staged_hdr` non-NULL) — the mined block is admitted.
- **L2 gate is reached** (`ingested` non-NULL, `have_data=1 failed=0`) but does
  NOT fire because of L2.5 below.

### L2.5 — NEW blocker: the downstream stage pipeline does not flow a successor-less synchronously-ingested block

After L0+L1, the mined block at height 1 is admitted, but EVERY downstream stage
cursor stays at 1 (`[L2-DIAG] ... utxo_ok=0 | cursors: vh=1 bf=1 bp=1 sv=1 pv=1
ua=1 tf=1`). So `utxo_apply` never applies block 1 → `utxo_apply_stage_succeeded_at(1)`
is false → the L2 finalize gate's validation witness fails → still
`block-not-finalized-by-reducer`. `validate_headers` floors at
`header_admit cursor - 1` (`app/jobs/src/validate_headers_stage.c:446-451`);
despite `header_admit` producing block 1 and advancing its cursor to 2, nothing
downstream advances within the synchronous `reducer_drain_to_convergence` pass.
This is the deepest layer: the staged reducer pipeline — designed for continuous
supervisor-ticked, network-fed operation — does not drive a single successor-less
mined block through validate→script→proof→utxo→finalize when driven synchronously
from `reducer_ingest_block`. Likely a cursor-visibility / persisted-cursor-read
timing issue across the per-stage `progress_store` transactions within one drain,
or a downstream active-chain-membership gate. **Investigate next.**

### L2.5 ROOT-CAUSED + the architectural wall (2026-06-05, all reverted)

**L2.5 = `pindex_best_header` not advanced by the producer path.** With L0+L1,
the mined block is admitted but every downstream cursor stayed at 1: nothing
advanced. Root cause: `vh_resolve_bi` (`validate_headers_stage.c:270-280`)
resolves a block above the finalized window ONLY via `ms->pindex_best_header`,
but the reducer PRODUCER path (`header_admit` → `add_to_block_index`) never
advances `pindex_best_header` — only the network path does
(`lib/net/src/msg_headers.c:485-517`, chainwork-ranked). **Verified fix:** after
`authoritative_admit` in `step_admit`, advance `ms->pindex_best_header` to the
admitted block when it is most-work (mirror msg_headers; needs
`core/arith_uint256.h`). With this, the pipeline FLOWS: all cursors advanced
(`vh=bf=bp=sv=pv=ua=2`) — a big step.

**CORRECTION (later same day): it is NOT a finalized-window wall — it is a
drain-ordering / stale-upstream-row problem.** A decisive probe added an
instrumented `active_chain_at(next_h)` inside `proof_validate` and found it
returns **non-NULL** with `have_data=1` once L2.5 is in place
(`[L2.6-DIAG] h=1 sv_upstream_ok=0 aca=0x… have_data=1`) — the active-chain
window IS extended to the candidate, so the earlier "finalized-window wall"
hypothesis was WRONG. The real signal: `proof_validate` sees its upstream
(`script_validate`) row `ok==0`, and `script_validate` in turn exits via its own
`upstream_failed` path BEFORE running `validate_block_scripts` (its
`[L2.6b-DIAG]` after the validate call never fires). So a **failure row is
recorded for height 1 by an early stage during drain #1** — which runs BEFORE
`reducer_persist_ingested_body_locked` makes the body available — and that stale
`ok=0` row then propagates downstream as `upstream_failed` and is never cleared
in drain #2. The fix is in the synchronous-ingest ordering
(`reducer_ingest_block`): persist the body and/or seed the stages so the body is
present BEFORE the first stage processes the height (or make the body-dependent
stages re-evaluate rather than persist a permanent failure when the body is not
yet available). This is a smaller, more localized change than the (incorrect)
"3-stage resolver" framing — but still consensus-path and must be proven on a
copy. **Next session: instrument `body_fetch`/`body_persist`/`validate_headers`
results for height 1 across drain #1 vs drain #2 to pin the exact stale row.**

### Next session order

L0 is shipped (`06ae18c26`). The full feature needs, in order: L1 (header
staging) + L2.5 (best_header in producer path) — both verified this session, make
the pipeline flow — then **L2.6/L2.7**: give `script_validate`/`proof_validate`/
`utxo_apply` an above-finalized-window block resolver (mirror `vh_resolve_bi`),
or extend the active-chain window for the duration of the synchronous ingest.
Then L2 (regtest-gated `set_authoritative_tip` finalize). Then the 3 proofs (e2e
+ negative-gate twin + `generate 3 → getblockcount==3`). All exploratory diffs
are in git history; only L0 is on disk.

## Method

`tools/scripts/isolated_node_env.sh` gives a safe isolated `/tmp` regtest node
(non-live 39xxx ports, dead `-connect` sink). Reproduce with `generate N` and
grep `node.log` for the `[mining] ... REJECTED by reducer` line. Layer 1 must be
proven first; Layer 2 is dead code until then.
