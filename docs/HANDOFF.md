# HANDOFF — read this first

**Restart command:** type **`continue the zclassic23-way convergence drive — use workflows, commit + push as you go`**.

State at handoff: main worktree, HEAD `030f16a0e` (round 4+5 below). Verify with
`git status --short --branch` before editing.

---

## ACTIVE AXIS (2026-06-05): convergence — "everything the zclassic23 way"

Owner directive this run: *"do EVERYTHING the zclassic23 way — dig into every
file, DRY, good API, document everything, use multiple workflows, commit + push
as you go, make the server amazing."* This is the **architecture/quality** axis
(distinct from the v1/live-wedge mission below — both are real; the owner chose
this one). Full state + gotchas: memory
`project_convergence_axis_2026-06-05.md`; ranked work: `docs/work/convergence-backlog.md`.

**Where it stands** (≈22 commits landed this run, all green):
- 8/8 framework shapes real; `framework_shape_allowlist`=0; 5 lint ratchets at 0.
- `boot_index.c` 1539→381; **`boot_services.c` 3517→1767** (decomposed into 7
  byte-identical units). File-size debt board: **2 files left** — `boot.c` (3618,
  FROZEN, never touch) + `boot_services.c` (1767).
- Real bugs killed: `coins_alloc` OOM NULL-deref; 2 MCP crashes; an `_Atomic` UB.
- 31 consensus/crypto public headers documented (accuracy-reviewed).
- **Rounds 4+5 (`cf7bbf05f..030f16a0e`)** swept the under-audited subsystems
  (tools/mcp, app/views, lib/wallet, lib/rpc, app/models, lib/util,
  app/controllers, service glue). Fixed a **build regression** (`blake2b.h` doc
  comment closed early, broke clean compile), **3 MCP + 2 explorer NULL-deref
  crashes**, a `contact.c` `before_save` veto-bypass, NULL-safe `sqlite3_errmsg`,
  2 local DRY folds, and ~16 documented public headers. Adversarial review
  declined 2 false findings (LOG_FAIL/LOG_ERR already return). **The
  non-consensus safe axis is now harvested** — audits over it return mostly
  self-rejected noise; see `docs/work/convergence-backlog.md` rounds 4/5 +
  deferred lists. Stop fanning safe audits over swept subsystems.

**The proven method (repeat it):** read-only audit Workflow → ranked backlog →
parallel **edit-only** adversarially-reviewed Workflows on **DISJOINT** file sets
→ ONE union gate I run myself (`make -j$(nproc) zclassic23` + `make lint` [35
gates] + `./test_parallel` [expect `0/371`] + boot-smoke if the boot path changed)
→ commit per logical group → push. Redirect `make` output to a file and grep
`error:|warning:` to save context. Mutating workflows on disjoint files may run
concurrently; verify the union together.

**Next targets, in priority order:**
1. **`boot_services.c` shutdown TU** (finishes the boot decomposition, →~1520).
   HIGH RISK: it carries the `coins.db` COMMIT-before-`block_index` fsync
   invariant. Boot-smoke CANNOT validate it — needs a real **SIGTERM stop +
   restart** cycle on a datadir COPY. Do it ALONE. See
   `[[feedback_at_tip_kill9_ordering_invariant]]`.
2. **flyclient/MMB block** out of `boot_services.c` — consensus-adjacent, shares
   the `g_mmb_leaf_store` extern + a lint-gate owner assertion; extract with the
   MMB-build block.
3. **peer-scoring enum extension** (OWNER-GATED — changes DoS-ban policy; design
   noted in `docs/work/convergence-backlog.md`). Do NOT do a naive
   `peer_misbehaving`→`peer_scoring_record` swap: mapping by meaning changes ban
   weights, mapping by weight mis-names. Extend the enum first.
4. More DRY/doc waves only if an audit returns real findings — the safe axis is
   nearly harvested; **STOP fanning out when sweeps return 0–1 findings.**

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

The node holds at tip but does **not finalize forward**: `tip_finalize` shows
`reorg_detected_total` climbing while `finalized_total=0`, and the boot
self-heal `tip_stall_oracle_rebuild` is exhausted. No v1 criterion that needs
live forward progress (C3 real cold-sync, C6 soak, C8 parity) can pass until
this clears.

- Diagnose on a datadir **COPY**, never live: `tools/diagnose_gap.sh`, then
  follow `docs/work/fast-path.md` (diagnose → design+critique → reset-safe
  test → repro-on-copy → commit).
- Leading autonomous fix: wire the safe have-data window extender
  (`app/jobs/include/jobs/stage_helpers.h` →
  `active_chain_extend_window_have_data` at `lib/validation/src/chainstate.c`).
  A prior naïve wiring was reverted (`481c520b9`) for churning `tip_finalize`
  — validate on a copy before any deploy.
- Owner-gated companion: the coins-commitment-persist keystone
  (`docs/work/coins-commitment-persist-plan.md`).
- Recovery FSM design: `docs/work/service-state-machine.md`.

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
touch lib/test/src/test_parallel.c && make test_parallel && ./test_parallel
./tools/zcl-rpc getblockcount        # live tip — is it advancing?
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
