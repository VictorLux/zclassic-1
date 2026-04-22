# AGENT-3 — Cryptography / Sapling / Consensus-Crypto / Net-Parallel

**Working directory:** `~/zclassic23-3`.
**Coordinator:** Rhett (`~/zclassic23`), coordinator-only.
**Sibling:** Agent-2 (`~/zclassic23-2`).

See [`AGENT.md`](AGENT.md) for the cross-agent priority table and
every row's full description. This file is Agent-3's executable
checklist plus the lane rules.

---

## Lane — what you may edit

**Full edit access:**
- `lib/crypto/`, `lib/sapling/`, `lib/keys/`
- `lib/core/src/random.c`
- `lib/validation/src/sigops.c` (P1.6)
- `lib/validation/src/check_block.c` (P1.7)
- `vendor/tor` submodule (pin bumps only)
- `lib/net/src/msgprocessor.c`, `lib/net/src/download.c` — **ONLY**
  for the P7.4 backpressure watchdog scope. Agent-2 owns the rest
  of `lib/net/`.
- `lib/test/` — add/modify tests for your changes

**Read-only / off-limits:**
- `lib/wallet/`, `lib/storage/`, `lib/coins/`, `app/*`, `tools/mcp/` — Agent-2
- `lib/rpc/`, `lib/script/` — Agent-2
- Other `lib/validation/` files — Agent-2
- `lib/core/` beyond `random.c` — off-limits
- Other `vendor/` dirs — pinned infrastructure

**STOP + ping Rhett triggers:**
- Any change to serialized block/tx format
- Any change to consensus constants
- For P7.4: any change that affects the P2P protocol wire format
  (it's a watchdog — observes state, rejects after parse — not a
  protocol change)
- Any change to a Priority Group's scope or acceptance criteria

---

## Current status — NOW = P11.8

## 📡 MCP is live — use these tools instead of shell whenever possible (2026-04-22 04:30)

Coordinator registered `zcl23` as a Codex MCP (global, `~/.codex/config.toml`).
`.mcp.json` is also in the repo root so Claude Code auto-discovers it.

**To verify** in any Codex session: `codex mcp list` — you should see
```
zcl23  /home/rhett/zclassic23/zclassic23  -mcp  -  -  enabled  Unsupported
```

In Codex, the tools surface as `zcl23.<name>`. Useful for YOUR lane:

| Task | MCP tool | Shell fallback |
|---|---|---|
| Check node state | `zcl_status` | `./tools/zcl-rpc getblockcount` |
| List Sapling addresses | `zcl_z_listaddresses` | — |
| Benchmark | `zcl_benchmark` | `./test_zcl benchmark_*` |
| Self-test | `zcl_self_test` | `./test_zcl` |
| Tail events | `zcl_events` | — |
| Log tail | `zcl_logtail(lines=N)` | `tail -N ~/.zclassic-c23/node.log` |
| Arbitrary RPC | `zcl_rpc(method, params)` | `./tools/zcl-rpc ...` |

**⚠ UNSAFE against live node until P24.14 redeployed:** `zcl_syncdiag`,
`zcl_walletaudit`, `zcl_listunspent`, `zcl_z_listunspent`, `zcl_rescanblockchain`.
For shielded-tx inspection use `zcl_listtransactions` / `zcl_z_listaddresses` —
those don't touch coins_view_cache and are always safe.

**Coord tools (`zcl_coord_*`) are P25 scope — Agent-2 building them.** Once
P25.3 lands, use `zcl_coord_mail_inbox` on kickoff instead of re-reading AGENT-3.md.

---

## ⚡ ACTION LIST — Agent-3 — 2026-04-22 04:00

**You are on MVP #8: parity-diff CI gate. This is a test-writing row.
It pairs with Agent-2's P12.3 parity cluster but you own the test.**

### Goal

Build a deterministic CI gate that walks a fixed set of mainnet blocks
through zclassic23 AND zclassicd, captures their validation decisions +
mempool state + wallet events, and asserts parity (same accept/reject,
same UTXO deltas, same fee totals).

### Step 1 — pull + restore existing RED

You already wrote a 632-line RED test for P11.8 in a prior session.
Coordinator preserved it to side branch `wip/agent-3-p11.8-red`.
Restore it untracked so you can continue:

```
cd ~/zclassic23-3
git fetch origin && git checkout main && git reset --hard origin/main
git show origin/wip/agent-3-p11.8-red:lib/test/src/test_parity_diff_gate.c \
  > lib/test/src/test_parity_diff_gate.c
wc -l lib/test/src/test_parity_diff_gate.c   # expect 632
```

Your existing RED already defines the comparison contract:
`PARITY_GATE_OK`, `PARITY_GATE_FAIL_REMOTE_UNREACHABLE`,
`PARITY_GATE_FAIL_HEIGHT_MISMATCH`, `PARITY_GATE_FAIL_HASH_MISMATCH`,
`PARITY_GATE_FAIL_LOCAL_BEST_BLOCK_MISMATCH`. Don't redo this — pick
it up from where you left off.

### Step 2 — push RED (your existing 632 lines)

Wire into test runner + commit:
```
# lib/test/include/test/test_helpers.h — add:
#   int test_parity_diff_gate(void);
# lib/test/src/test.c — add call site in the test list
git add lib/test/src/test_parity_diff_gate.c \
        lib/test/include/test/test_helpers.h \
        lib/test/src/test.c
git commit -m "test/P11.8: RED for parity-diff CI gate"
git push origin main
```

### Step 3 — write GREEN

Read your existing RED to see what needs to exist for the test to
pass. Likely dependencies (grep your own 632-line file):

- An RPC client helper that calls `zclassicd-rhett` at 127.0.0.1:8232
  (use existing `lib/rpc/src/client.c` or write a minimal one in
  `lib/test/src/test_helpers.c`).
- A deterministic way to get local `chain_height` and local block-hash
  -by-height (use `zcl_rpc` or direct block_index lookup).
- Socket-timeout + error-path handling so the test fails loudly
  (not silently) when zclassicd isn't running.

Sketch of GREEN work:
1. Add any helper function the RED test references but that doesn't exist.
2. Run `make test_zcl && ./test_zcl` — expect your parity_diff test
   to transition from FAIL-compile → FAIL-runtime → PASS.
3. If legacy zclassicd isn't available in CI, gate the test behind
   `ZCL_STRESS_TESTS=1` (same pattern as P11.4 shielded-payment gate).

GREEN commit:
```
make -j$(nproc) test_zcl && ./test_zcl 2>&1 | grep parity_diff
git add <files you added>
git commit -m "test/P11.8: GREEN for parity-diff CI gate"
git push origin main
```

### Step 4 — rotate NOW

Update this file's `## Current status` header to `NOW = P15.4` (per
your post-MVP roadmap), commit, push.

### Parallel note — P24.27 also assigned to you

After P11.8 lands, your next row is **P24.27** (observability lint
gate — `fprintf(stderr)` must pair with `event_emit` or `// obs-ok:`
marker). See AGENT.md for the full description. This is a natural
pair with your existing P24.2/P24.4 lint cluster.

**P11.6 landed 39bb904f3 [test:1.0]** (RED: 4ae4b09db). Four pieces
now gate "someone we don't know can run zclassic23 for a week
without intervention":

- `lib/test/include/test/soak_harness.h` — analyzer interface with
  five ordered verdict rules: NO_SAMPLES > CRASH > TOO_SHORT >
  TIP_STALL > RSS_WALK. Priority-ordered so the soonest-actionable
  signal lands first and the output stays deterministic for CI.
- `lib/test/src/soak_harness.c` — high-water-mark tip tracking so
  reorgs don't reset the stall timer (only a new peak does);
  latched RSS baseline after a 30-min warmup (min-seen, not max
  or last — robust against both slow creep and transient spikes);
  one-strike crash_count because the MVP criterion is "no
  operator intervention", not "cleanly restarted by systemd".
- `lib/test/src/test_soak_harness.c` — six synthetic cases cover
  every verdict path. Pre-GREEN stub always returned SOAK_OK, so
  5/6 failed; post-GREEN all six pass.
- `tools/soak/main.c` — standalone runner. Polls via `pidof -s`,
  `/proc/<pid>/status` VmRSS, and `./zcl-rpc getblockcount`;
  emits a TSV log per sample; exits with the verdict ordinal at
  deadline. `make soak-7day` and `make soak-smoke` targets; neither
  wired into `make ci` (7 d is out of band; smoke needs a live
  node). Lint-clean; uses no `lib/` beyond `test/soak_harness.h`
  so the runner builds in ~1 s without dragging the full node
  compile graph in.

Smoke-verified with `--service=no-such-service --duration-sec=3
--interval-sec=1`: three crash samples, verdict FAIL_CRASH, exit
status 2, log in the advertised TSV shape.

**P11.4 landed <pending push> [test:1.0].**

Five pieces now gate the real transparent->shielded send path end-to-end:

- `lib/test/src/test_shielded_payment_gate.c` — deterministic stress gate that
  seeds a wallet-owned transparent UTXO, derives a Sapling address via
  `z_getnewaddress`, executes `z_sendmany`, then asserts mempool admission,
  one shielded output, correct negative `value_balance`, successful wallet
  trial decryption, and the expected Sapling note/balance.
- `lib/test/src/test.c` — focused subset hook
  `ZCL_TEST_ONLY=shielded_payment` plus full-suite registration so the gate is
  reachable both on demand and in the stress-enabled suite.
- `Makefile` — `test-shielded-payment` target that checks
  `~/.zcash-params` up front, runs the focused gate with
  `ZCL_STRESS_TESTS=1`, and strips `zclassic23` after link so the
  no-hardcoded-home suite stays green.
- `lib/test/src/test_make_lint_gates.c` — in-process raw-SQLite gate
  self-test so late-suite environment drift no longer makes the lint
  regression test flaky.
- Verification — `make test-shielded-payment` passes with real Sapling params,
  and `./test_zcl` is back to `ALL TESTS PASSED (0 failures)`.

**P11.5 landed <pending push> [test:1.0 c704fa0e2].**

Three pieces now gate the shipped store flow across the persistence boundary:

- `lib/test/src/test_store_e2e_gate.c` — deterministic stress gate that
  seeds a store order through the HTTP controller path, persists a confirmed
  Sapling note for the generated payment address, runs payment reconciliation,
  reopens the DB/model layer, and asserts the order advances to `STORE_ORDER_SENT`,
  credits `ZCL23ACCESS` exactly once, and unlocks token-gated access.
- `lib/test/src/test.c` + `Makefile` — focused subset hook
  `ZCL_TEST_ONLY=store_e2e` and `test-store-e2e` target so the gate is reachable
  both on demand and in the stress-enabled suite.
- `app/controllers/src/store_controller.c` — payment reconciliation now persists
  the post-mint order status through a fresh one-shot DB reopen instead of trying
  to reuse the pre-mint reader handle. The RED exposed a real split-handle
  persistence bug: token credit landed, but `orders.status` stayed `PENDING`.

Verification: `make test-store-e2e` passes, and `make test` is back to
`ALL TESTS PASSED (0 failures)`.

**P11.8 (HIGH): MVP #8 parity-diff CI gate. Agent-3 NOW.**

Continues the MVP drain. P11.6 is green as of this commit; P11.7
was already green (kill-9 chaos recovery). The remaining MVP row in
this lane is P11.8 (parity diff, coupled with Agent-2's P12.3).

**Discipline (every P11+ row is [test:1.0]):**
1. **RED FIRST** — failing test that demonstrates the gap.
2. **GREEN** — real implementation.
3. **Mark done** — update `AGENT.md` row + current NOW.

**PROGRESS HINT (added 2026-04-21 05:08 by coordinator):** if you have
a RED test file on disk but haven't pushed it yet, push the RED alone
first — even if it's rough, even if GREEN isn't ready. That commit is
the agreement with the coordinator that we understand the failure
shape. It's OK for a RED test to be 506 lines or 20 lines; what
matters is that it's the smallest thing that fails for the right reason.
If the test file compiles and fails (any kind of failure), stage it
and push as `test/P11.4: RED for shielded-payment CI gate`. Don't
wait to polish it together with the GREEN.

**⚠ URGENT (coordinator 2026-04-21 21:00):** your worktree has an
**untracked** `lib/test/src/test_shielded_payment_gate.c` at 360 lines
that hasn't been touched since 06:01 (~15h). This is the 4th attempt
at P11.4 — the previous three all died because `git reset --hard
origin/main` wipes untracked files too. **Before you do anything else:**
```
cd ~/zclassic23-3
git add lib/test/src/test_shielded_payment_gate.c
git commit -m 'test/P11.4: RED for shielded-payment CI gate (WIP)'
git push origin HEAD:refs/heads/wip/agent-3-p11.4
```
That pushes to a side branch so your work survives any kickoff reset.
Then continue working on main. When GREEN is ready, rebase main onto
wip/agent-3-p11.4 or cherry-pick, whichever is easier.

**⚠ Do not call `zcl_syncdiag` or `zcl_getrawtransaction`** (or the
raw RPCs `getsyncdiag` / `getrawtransaction`) against the live node.
Both crash the node right now — see AGENTS.md safe/unsafe list. Use
only `zcl_status`, `zcl_kpi`, `zcl_events`, `zcl_peers`, `zcl_logtail`,
`zcl_health`, `zcl_validationstatus`, `zcl_getblockcount`,
`zcl_peer_report`. For shielded-tx inspection you can call
`zcl_listtransactions` / `zcl_z_listaddresses` — those don't touch
`coins_view_cache` and are safe.

**Cross-cutting check:** P9.10 is about cache-side-channel on the
MSM witness — related but distinct. P9.1 closed the timing channel
on the blinding scalars; P9.2 closed the circuit-side nk-derivation
UB; P9.6 closed the prover-boundary input-validation gap. P9.7–P9.9
are parked per coordinator direction.

---

## POST-P9 WORK QUEUE — the shining-example roadmap (Agent-3 lane)

Filed 2026-04-20 after the coordinator's full review + Erigon / Caplin
architecture study. Work the list **in order** unless you hit a blocker.

Every row has a full description in [`AGENT.md`](AGENT.md).

### Phase 0 — Finish P9 sapling audit (CURRENT)

- [x] **P9.4** HIGH — `fr_fft` / `fr_fft_parallel` silent no-op. done f5a31b48d [test:1.0].
- [x] **P9.1** HIGH — `g1_scalar_mul` variable-time double-and-add leaks Groth16 blinding. done f10b39303 [test:1.0].
- [x] **P9.2** CRITICAL — `sapling_circuit.c:65 / 161-162` placeholder UB paths. done 94532c87e [test:1.0].
- [x] **P9.6** MED — `zclassic_sapling_spend_proof` witness length not bounded. done 2fe801a08 [test:1.0].
- [ ] **P9.7** MED — `sprout_verify_groth16` size_t underflow + race on `sprout_set_vk(NULL)`. **Parked** per coordinator direction.
- [ ] **P9.8** MED — `ensure_generators` 256-retry silent exhaustion. **Parked**.
- [ ] **P9.9** MED — prover printf leaks wallet-activity timing. **Parked**.
- [ ] **P9.10** LOW — `msm_parallel` cache-side-channel on witness (after threat-model decision).
- [ ] **P9.11** LOW — `zip32_diversifier` `for(;;)` cap at 256 iterations.

### Phase 1 — MVP CI gates (continue Agent-3 lane)

- [x] **P11.4** — shielded-payment CI gate (MVP #4). done <pending push> [test:1.0].
- [x] **P11.5** — store e2e CI gate (MVP #5). done <pending push> [test:1.0 c704fa0e2].
- [x] **P11.6** HIGH — 7-day soak harness (MVP #6). done 39bb904f3 [test:1.0 4ae4b09db].
- [ ] **P11.8** — parity-diff CI gate (MVP #8). **Agent-3 NOW.** Coupled with Agent-2's P12.3 + P12.3.1 and Agent-3's P17.5.

### Phase 2 — P15 Discipline (Agent-3 lanes)

*Attribution: Erigon per-subsystem `agents.md` (LGPL-3.0 © Erigon Authors).*

- [ ] **P15.4** HIGH — remove `-Wno-unused-result` for `lib/crypto/`, `lib/sapling/`, `lib/keys/`, `lib/core/src/random.c`. Annotate headers with `[[nodiscard]]`. One PR per subsystem.
- [ ] **P15.5 (A3 portion)** MED — author `lib/crypto/agents.md`, `lib/sapling/agents.md`, `lib/keys/agents.md`. Short files (~50 lines each).

### Phase 3 — P17 Testing regime (Agent-3 leads)

The backbone of the shining-example bar. Agent-3 owns it because
fuzz/property-based/spectest overlap the sapling + wire-parser lanes
natively.

- [ ] **P17.1** HIGH — wire `tools/fuzz/fuzz_script.c`, `fuzz_block.c`, `fuzz_p2p.c` into CI via libFuzzer (`-fsanitize=fuzzer,address`). `make fuzz` (60s per harness per PR); `make fuzz-long` (30 min nightly). Corpus in `tests/fuzz_corpus/` seeded from real mainnet traffic. Add 5 new harnesses: `fuzz_zmsg`, `fuzz_znam`, `fuzz_zslp`, `fuzz_mmb`, `fuzz_compact_block`.
- [ ] **P17.2** HIGH — sanitizer matrix: `make ci-asan` / `ci-tsan` / `ci-ubsan` / `ci-msan`. TSan will likely surface real races in `connman`, `dandelion`, `swarm_sync`.
- [ ] **P17.3** MED — hand-rolled property-based test harness in `lib/test/include/test/proptest.h`. 10k iterations per property. Apply to: tx serialize/deserialize roundtrip, wire parsers, Sapling proof verify with hand-fuzzed witnesses.
- [ ] **P17.5 (A3 lead)** HIGH — spectest harness. Reference block corpus at `tests/spectest/blocks/<height>.dat` (first 10k + last 10k). Replay through both zclassic23 and zclassicd via RPC, diff state. *Attribution: Erigon `cl/spectest/` (LGPL-3.0).* Pair with Agent-2.
- [ ] **P17.6** HIGH — per-stage Unwind-is-inverse-of-Forward contract test. For each P16 stage, `forward(N) + unwind(N)` must leave every storage-temporal domain byte-identical. Framework in `lib/test/src/test_stage_contract.c`.

### Phase 4 — P18 Perf (Agent-3 crypto lane)

- [ ] **P18.4** MED — validate AVX-512 IFMA isn't silently falling back. New `zcl_crypto_status` MCP tool reports active code path (`AVX-512 IFMA / AVX2 / scalar`) + cycles-per-op benchmark.

### Phase 5 — P20 Developer MCP (Agent-3 share — START IMMEDIATELY)

**Can run in parallel with P9 drain.** Lane match: test infrastructure
+ coverage tooling naturally overlaps with Agent-3's testing lead.

- [ ] **P20.4** HIGH — `zcl_coverage` MCP tool: per-file line coverage + test-file mapping. Built from `gcov`/`llvm-cov` artifacts.
- [ ] **P20.10** HIGH — `zcl_test_map` MCP tool: `test_file → [files_exercised]` + reverse. Answers "what test would catch this?"

### Phase 6 — P21 Test oversized-file deconstruction

**No dependency** — can start anytime. Big quality-of-life for every
sapling + net test touch.

- [ ] **P21.7** HIGH — `test_sapling.c` (4,677) → `test_sapling_{crypto,circuit,proof,note,tree,wallet}.c`. No file over 1,000 lines post-split.
- [ ] **P21.8** HIGH — `test_net.c` (4,123) → `test_net_{msgprocessor,download,connman,dandelion,swarm}.c`. Preserves all current cases; rename-only diff.

### Phase 7 — P22 AI-native scaffolding (Agent-3 share)

- [ ] **P22.4 (A3 share)** MED — `docs/spec/{crypto,sapling,keys}.md` cold-memory RAG corpus. Each: architecture, invariants, known gotchas, constant-time guarantees, side-channel notes.

### Phase 8 — P23 Generative MCP (Agent-3 share)

Agent-2 owns the rest of P23 (see AGENT-2.md Phase 13). Agent-3's
share is the test-scaffold generator — natural lane match because
Agent-3 leads P17 testing discipline and P22.2 `.ac.yaml` sidecars
encode the RED-first contract.

- [ ] **P23.7** HIGH — `zcl_scaffold_test_from_row(row_id)`. Reads `docs/rows/P<id>.ac.yaml` (P22.2), emits a RED test skeleton that matches the acceptance criteria: fixture hooks, setup/teardown, expected-failure assertions. One MCP call → ready-to-fail test file committed into `lib/test/src/`. Biggest productivity multiplier for the `[test:1.0]` discipline because it removes the "copy another test and edit" step from every new row.

**Blocks on:** P22.2 (sidecar format must exist first).

### Phase 9 — P24 Coordinator audit wave (2026-04-21, Agent-3 share)

Filed after the coordinator's binary-drift session + landmine scan.
Full descriptions in [`AGENT.md`](AGENT.md) Priority 24.

- [ ] **P24.2** HIGH — lint rule: ban `__attribute__((unused))` on function parameters named `*_len`, `*_size`, `*_count`, `*_sz`. This is the exact landmine that hid P9.6 for months. Wire into `make lint`. **Highest-leverage Agent-3 post-P11.6 row** — one lint gate retroactively protects every Sapling/crypto parameter.
- [ ] **P24.4** HIGH — `abort()` triage. Refactor `lib/keys/src/key.c:26,215`, `lib/sapling/src/sapling.c:107`, `lib/sapling/src/note_encryption.c:75` to return `zcl_result` instead of `abort()`. Void-return callers make propagation impossible → every site is an uptime landmine. **Depends on P15.2 landing first** (zcl_result type).
- [ ] **P24.27** HIGH (filed 2026-04-22 02:45) — **observability lint gate.** Every `fprintf(stderr, ...)` in lib/ + app/ must either (a) have an adjacent `event_emit(...)`, (b) be followed by `return false`/`exit`/`abort`, or (c) carry a trailing `// obs-ok:<reason>` marker. New lint script `tools/scripts/check_observability_pairing.sh` + self-test `lib/test/src/test_observability_gate.c` (matches the P24.14 fixture pattern Agent-2 landed). This is the defensive lint gate that catches P24.18-class silent failures (`flush_coins: sapling_tree persist failed` printed to stderr and nothing else). The bulk of work is annotating ~500 existing fprintf sites; coordinator estimates 60% are "debug log, harmless" (add `// obs-ok:debug`), 35% are "failure path with propagation" (add event emit), 5% are real regressions requiring refactor. **Natural pair with your P24.2/P24.4 lint cluster.**

---

## Cross-cutting notes

- **Agent-2 owns `lib/net/`** except the P7.4 backpressure watchdog scope.
- **P17.6 depends on P16 stages existing.** Until then, P17.1/P17.2/P17.3/P17.5 proceed in parallel.
- **P11.6 soak harness is independent.** Start design work now; runner can ship the moment P14.13 lands and the chain advances.

---

## Execution discipline (non-negotiable)

1. **RED test first.** Every P9.x row is [test:1.0]; maintain the discipline.
2. **One row per commit.** Row ID in commit message.
3. **Update AGENT.md.** Mark `done <SHA> [test:X.X]`.
4. **Respect lane boundaries.** `lib/wallet/`, `lib/storage/`, `lib/coins/`, `app/*`, `tools/mcp/`, `lib/rpc/`, `lib/script/`, and most of `lib/validation/` are Agent-2.
5. **STOP + ping Rhett** on any serialization / consensus constant / P2P wire format change.
6. **Keep `make test` green.** Push every row; never amend pushed commits.

Total rows in this queue: **~26** across 8 phases. P17.1 + P17.2 +
P11.6 are the three highest-leverage rows — each unblocks measurement
we don't have today. P23.7 ships late but compounds forever: every
future row gets its RED test generated.
