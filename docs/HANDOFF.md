# HANDOFF — read this first

**Restart command:** type **`continue zclassic23 development`**.

State at handoff: main worktree. Verify HEAD with `git status --short --branch`.

---

## ★ LIVE WEDGE — STILL OPEN; P2 (self-heal prerequisite) LANDED; next = L0/L1 H* self-heal (2026-06-07)

**The live wedge is NOT resolved.** (The 2026-06-06 `validate_headers` recheck
fix below was real and shipped, but it did **not** clear the live node — the
datadir is **multi-epoch torn**, a deeper class than recheck starvation.)

**Live truth (this handoff):** node serves at **3,134,951**, oracle (`zclassicd`,
RPC 8232) at **3,139,290** → **~4,339 behind and not climbing** (`tip_advance_age`
== uptime). The 8-stage reducer's upstream cursors have raced to **3,138,981**
while `tip_finalize` is stuck at **~3,134,954**. Per-height logs, the 8 stage
cursors, and the legacy `block_index` flags drifted to **different heights across
crash/rewind/replay epochs**, and **nothing reconciles them as a window**.
Crucially: **the coins are consistent** (inverse-delta-complete) — this is a
*flag/cursor view* tear, not a coin-money tear. Self-heal is `operator_needed`
(5 attempts exhausted). ⚠️ **Deploy drift:** the *running* binary predates repo
HEAD; a clean redeploy is owner-gated and won't by itself fix a torn datadir.

**The fix (active driver: `docs/work/self-healing-reducer-plan.md`, 2026-06-07):**
compute a provably-consistent frontier H* from durable state, then sweep-heal the
drifted flags/cursors forward over the (H*..served_floor] window — never rewinding
the consistent coins, never deleting a `tip_finalize_log` row.

- **P2 (DONE, on `main` `c81e69ae0..4b60c1149`, fully proven):** `coins_applied_height`
  is co-committed inside the `utxo_apply` txn on **all** write paths (forward,
  reorg-unwind, poison_rewind) → a contiguous applied-coins frontier that can't
  hide an interior hole. Proven by 2 independent workflow impls (agree) + 3
  adversarial verifiers + `test_parallel` **0/375** + `lint` 35-gate + **chaos 9/9**
  + a kill-9 copy-proof (`tools/copyproof_p2_frontier.sh`) holding the invariant on
  the raw crash image. Offline checker: `make p2_invariant_check` -> `build/bin/p2_invariant_check <datadir>`.
- **L0 (task #11) `reducer_frontier_compute_hstar` — DONE** (on `main` `50dfd1753`):
  pure SELECT-only authority in `app/jobs/src/reducer_frontier.c` returning
  {hstar, served_floor}; anchored at the SHA3 checkpoint **3,056,758** (cold-import
  logs are sparse → contiguity-from-genesis is wrong); C1–C6 of the plan, NULL-hash
  = no-evidence, C4 coin-tear = WARN-only. PURE read-only (no mutation — heal is L1).
  Adversarially reviewed (PASS: read-only purity, checkpoint floor, tear-correctness,
  mutation-sensitivity via 2 revert-experiments); `test_reducer_frontier` 5 topologies;
  build green, `test_parallel` **0/376**, `lint` 35-gate. Follow-up: convert the C2/C3
  per-height query loop to set-based SQL before wiring into a hot/boot path.
- **NEXT — L1 (task #12) `reducer_frontier_reconcile_light`:** a Condition that
  sweep-heals `block_index` flags + clears HAVE_DATA holes (so `body_fetch`
  re-requests) + clamps ONLY the `tip_finalize` cursor — never touches coins.
- **L2/L3 (task #13):** forward-immunity for a *future genuine coin tear* +
  subtraction of the now-subsumed legacy repair code. NOT needed for today's wedge.

**Method discipline (mandatory):** diagnose on a datadir **COPY** only (isolated
ports 18299/18933, `setsid`); never touch the live datadir/oracle/ports; H* ≥
checkpoint 3,056,758; never delete a `tip_finalize_log` row; never lower the
public tip below `coins_best`. See `docs/work/fast-path.md` and memory
`project_tipfinalize_precondition_desync_fix_2026-06-07`.

---

## ACTIVE AXIS (2026-06-05): convergence — "everything the zclassic23 way"

Owner directive this run: *"do EVERYTHING the zclassic23 way — dig into every
file, DRY, good API, document everything, use multiple workflows, commit + push
as you go, make the server amazing."* This is the **architecture/quality** axis
(distinct from the v1/live-wedge mission below — both are real; the owner chose
this one). Full state + gotchas: memory
`project_convergence_axis_2026-06-05.md`; ranked work: `docs/work/convergence-backlog.md`.

**Where it stands — the autonomous-safe convergence drive is COMPLETE** (rounds
2–15, all green build0/lint35/test_parallel 0/371, all pushed; latest run
`88349b39d`→`ea7542cda` fixed ~52 real bugs + ~40 docs):
- 8/8 framework shapes real; `framework_shape_allowlist`=0; lint ratchets at 0.
  File-size debt board: `boot.c` (3618, FROZEN — **never touch**) + `boot_services.c`.
- **The whole non-consensus surface is harvested** (lib/net feature transports,
  every app/ controller/view/model, lib/wallet, lib/rpc, lib/storage projections,
  supervisors, metrics). Real bugs killed this session: a wallet keypool concurrent
  OOB + data-race snapshots, 2 key-material `memory_cleanse` leaks, a 1-byte stack
  overflow, a 4-projection event-skip class bug, a post-broadcast send-failure bug,
  a proven send-path tx/entry leak, 11 P2P-transport NULL-derefs/unchecked writes,
  3 shielded signing-path `signature_hash` checks, 5 UTXO script leaks, 2 realloc-OOM
  NULL-derefs, a double-free, a snapshot-anchor UAF race, a malformed-txid uninit read.
- **Consensus-adjacent layer handled the careful way** (rounds 12–15): strict-bar
  audit → read-only prove-or-refute (skeptical analysts + adjudicator) BEFORE any
  edit. Fixed: bg-validation honesty (no false "verified" when script checks skipped
  for missing undo; surfaces `verification_incomplete`), quorum symmetric tally.
  Refuted with evidence: `header_sync:575` (sanity gate; connect_block is the arbiter),
  coins-commitment no-rollback (derivative), `block_index_db` nFile cast (fails closed).
- **CORRECTION recorded**: `zcl_mutex` is RECURSIVE — two earlier "deadlock" calls
  were overclaims (code stands; framing corrected in git + backlog).

**The proven method (repeat it):** read-only audit Workflow → ranked backlog →
parallel **edit-only** adversarially-reviewed Workflows on **DISJOINT** file sets
→ ONE union gate I run myself (`make -j$(nproc) zclassic23` + `make lint` [35
gates] + `build/bin/test_parallel` [expect `0/371`] + boot-smoke if the boot path changed)
→ commit per logical group → push. Redirect `make` output to a file and grep
`error:|warning:` to save context. Mutating workflows on disjoint files may run
concurrently; verify the union together.

**What remains is owner-gated / repro-on-copy ONLY** (the autonomous-safe drive is
done — do NOT re-audit the swept surface, it's harvested; see
`docs/work/convergence-backlog.md` §12, §12-EXT, Round 8–15):
1. **`block_index_loader.c:376` nChainTx DRY** — real but latent (safe error
   direction; `connect_block` re-validates). The clean fix (call
   `block_index_forward_pass` instead of 3 hand-rolled recomputes) touches the
   **never-touch** `config/src/boot.c:2138` — needs an owner call on how to fix
   without editing boot.c. Boot-smoke on a COPY before deploy.
2. **`boot_services.c` shutdown TU** + **flyclient/MMB block** — boot decomposition
   tail; HIGH RISK (coins.db COMMIT-before-`block_index` fsync), needs a real
   SIGTERM stop+restart on a datadir COPY. Do ALONE.
3. **peer-scoring enum extension** (OWNER-GATED — changes DoS-ban policy). Extend
   the enum first; do NOT do a naive `peer_misbehaving`→`peer_scoring_record` swap.
4. **§12 consensus-DECISION items** (coins_view rollback nit, nFile>INT_MAX reject,
   plus the originals) — each repro-on-copy; most already triaged/refuted.
5. More audits ONLY if a NEW subsystem appears — **STOP fanning over swept code.**

**Gotchas that cost time this run (don't relearn):**
- **Boot-smoke light-copy floor is NOT a regression.** `repro_on_copy.sh`
  (default `--light`, no `blocks/`) rewinds tip 3134303→**3132299** given ≥~90s
  (DEGRADED_SERVING, "Not fatal"). PROVE innocence by running HEAD's binary on the
  same window — it floors identically. A crash or a LOWER floor = real regression.
- `test_make_lint_gates.c` BANS ~90 refactor-scaffold substrings in production
  comments ("byte-for-byte", "verbatim", "extracted from", "code motion", …).
  Name the PURPOSE, not the refactor. Renaming a symbol's home also needs the
  gauges/owner-file asserts in that test repointed.
- `check-observability-pairing` is a compiled tool: a raw `fprintf(stderr,…)`
  must be paired (−3/+6 lines) by `event_emit`/terminal `return`/`// obs-ok:`,
  ELSE route it through `LogPrintf` (not flagged). Editing a file can line-shift a
  PRE-EXISTING fprintf out of its window → newly fails.
- Gate #15/#21 (supervisor) now scope `config/src/`; a moved boot worker that
  spawns a thread must keep its `supervisor_register_in_domain`/contract or carry
  `// supervisor-ok:<tag>`.
- `config/src/*.c` is globbed (`CONFIG_SRCS = $(wildcard …)`) — new boot units
  build with NO Makefile edit.
- Adversarial review EARNS its keep (caught the `coins_alloc` 4th caller, a silent
  peer-ban weight change, an over-broad blake2b doc). Always run it; respect FAILs.

---

## The mission is v1 (not the refactor)

The v1 bar is **[`docs/MVP.md`](./MVP.md)** — 8 operator acceptance criteria;
v1 = MRS 8/8. **THE plan is [`docs/work/FORWARD_PLAN.md`](./work/FORWARD_PLAN.md)**
(MVP-anchored, with the live wedge as priority #1 and the autonomous /
owner-gated / operational critical path).

Honest status: **~2/8 met by hand, 0/8 CI-enforced** (every criterion test
gates on `ZCL_STRESS_TESTS=1`, which `make ci` never sets). Do not trust
`make test_parallel` green as a v1 proof — it runs zero MVP criteria.

The framework/architecture refactor is **~90% done and OFF the v1 path.**
`docs/FRAMEWORK.md` (architecture) and `docs/REFACTOR_STATUS.md` (debt board)
are reference. **Do not jump the queue into refactor work** while v1 buckets
are open.

---

## ⛔ #1 priority — the live wedge

**ROOT CAUSE (verified live 2026-06-05, multi-agent dx + direct probing) — it is
NOT a consensus code bug; the code is behaving CORRECTLY.** The node holds at
tip 3,134,303 because block body **3,134,304 is missing** (`legacy_mirror`
`stuck_reason:"missing-have-data"`) and there is **no eligible body source**:
- **Native P2P is correctly gated out by the anti-eclipse floor.** The node's
  ONLY reachable peer is one local MagicBean on `127.0.0.1:8033` that **flaps**
  (`disconnected=748`, `blocks_received=0`, stuck `syncing_headers`); external
  mainnet peers are TCP-unreachable from this box (`addnode_tcp_failures=1765`).
  A single localhost peer can never satisfy `p2p_minimum_viable`
  (`block_source_policy_runtime.c:161` — needs ≥2 healthy on ≥2 distinct IPv4
  groups, or ≥3). This gate is a **safety invariant — do NOT lower it.** A
  diagnosis workflow proposed lowering the floor to 2; that is **WRONG** (the
  effective floor is already 2 + eclipse checks, and the node has only 1 peer)
  and would weaken eclipse defense. Rejected.
- **The co-located zclassicd is healthy and readable but advisory-only.**
  zclassicd (PID 2273227, up 2 days) serves RPC on `127.0.0.1:8232`
  (`getblockcount`→3,136,562 verified) and `legacy_mirror` reads it fine
  (`reachable:true`). But the mirror is `bounded_advisory_fallback` trust and is
  in **"observing" mode** (`last_error:"local sync primary; mirror observing"`)
  — by design it will NOT authoritatively supply consensus blocks. The
  `rpc-unreachable` blocker label is **stale** (mirror actually reads zclassicd).

So `reorg_detected_total` climbing / `finalized_total=0` / the
"no-header-solution-backfill-required" rejections are all **downstream
symptoms** of the body-source starvation, not independent bugs.

**The two real resolutions are OPERATIONAL / OWNER-GATED (no consensus edit):**
1. **Native peer diversity** — get ≥2 honest, reachable, distinct-IP-group
   native peers so P2P clears the eclipse floor and bodies flow. Blocked here by
   the environment (only one flapping localhost peer is reachable). This is the
   trust-preserving fix.
2. **(Owner-gated policy)** Decide whether a fully-validated, co-located
   zclassicd may *authoritatively* supply bodies when native P2P is
   eclipse-starved — the long-standing advisory→authoritative mirror trust
   decision (see the cutover/single-engine history). Do NOT flip unilaterally.

Housekeeping: the `zclassicd-rhett` **systemd unit is crash-looping**
(`NRestarts=3990`, "Cannot obtain a lock on data directory" — it is a duplicate
fighting PID 2273227 for the datadir lock). Harmless to the oracle but pure
wasted CPU; mask/stop it. It is NOT the running oracle — do not confuse them.

Older notes (still valid as background): diagnose on a datadir **COPY**, never
live (`tools/diagnose_gap.sh`, `docs/work/fast-path.md`); the prior have-data
window-extender wiring was reverted (`481c520b9`) for churning `tip_finalize`;
recovery FSM design in `docs/work/service-state-machine.md`.

---

## Do Not

1. Do not weaken a lint gate or grow a baseline.
2. Do not delete `tip_finalize_log` rows or hand-edit stage cursors.
3. Do not ship a consensus-adjacent fix without a datadir-copy proof
   (`tools/repro_on_copy.sh`). The boot self-heal heals only on a `utxo_sha3`
   commitment match; otherwise it preserves FATAL — never weaken that.
4. Do not stop `zclassicd-rhett`; manage long-running services through
   `systemctl --user`.
5. Do not restore deleted cutover/projection-diff/public shadow tooling.
6. Do not move the local `zclassic23` P2P listener back to `8033`; the active
   dev node is on `8023` to avoid a `zcashd` port conflict.

---

## First 5 Minutes

```bash
git status --short --branch
make lint
touch lib/test/src/test_parallel.c && make test_parallel && build/bin/test_parallel
build/bin/zcl-rpc getblockcount        # live tip — is it advancing?
```

If the node is not running, or the tip is not advancing, record that
explicitly before claiming any live proof. Forward progress on the running
node is the real bar.

---

## Where the detail lives

| Need | Doc |
|------|-----|
| The v1 contract (8 criteria) | [`docs/MVP.md`](./MVP.md) |
| **THE plan** (critical path) | [`docs/work/FORWARD_PLAN.md`](./work/FORWARD_PLAN.md) |
| How to execute consensus-critical work safely | [`docs/work/fast-path.md`](./work/fast-path.md) |
| Engineering quality board (41 items) | [`docs/work/FINISH_CHECKLIST.md`](./work/FINISH_CHECKLIST.md) |
| Architecture (canonical) | [`docs/FRAMEWORK.md`](./FRAMEWORK.md) |
| Architecture debt board (off v1 path) | [`docs/REFACTOR_STATUS.md`](./REFACTOR_STATUS.md) |
| Directory / file-purpose map | [`docs/PROJECT_OVERVIEW.md`](./PROJECT_OVERVIEW.md) |

Default to subtraction. Prove on a copy before touching the live chain.
