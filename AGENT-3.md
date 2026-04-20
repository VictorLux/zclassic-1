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

## Current status — NOW = P9.1

**P9.1 (HIGH): `g1_scalar_mul` variable-time double-and-add leaks
Groth16 blinding (side-channel).**

`lib/sapling/src/bls12_381.c:g1_scalar_mul` walks the scalar bit by
bit and only runs `g1_add` on `(scalar[i] >> bit) & 1 == 1`. The
measurable time difference between iterations leaks the Hamming
weight of the scalar — and inside `groth16_prove` that scalar is
`r_blind` (line 857 → `r * delta_g1`) / `s_blind` (line 872 →
`s * delta_g2`, indirectly via `g2_msm(&pk->delta_g2, &s_blind, 1)`).
An attacker measuring wall-time of the prover recovers partial
information about the blinding factors, which degrades the
zero-knowledge property.

**Files + lines:**
- `lib/sapling/src/bls12_381.c:1708-1722` — `g1_scalar_mul` double-
  and-add with variable branch.
- Callers in `groth16_prover.c`: lines 857 (r*delta_g1), 885 (r*B_g1),
  911 (s*A), 920 (r*B_g1 again), 931 (rs*delta_g1).

**Fix shape:**
1. Replace the variable branch with a constant-time conditional add:
   always compute `tmp = g1_add(result, base)` and then a constant-
   time select between `result` and `tmp` based on the bit.
2. Mirror the change in `g2_scalar_mul` if the same pattern exists.
3. RED test: statistically measure cycle counts for two scalars with
   very different Hamming weights (e.g. all-zero vs all-one lower 64
   bits). Pre-fix the means differ by >> one standard deviation;
   post-fix they're indistinguishable.

**Discipline (every P9.x row is [test:1.0]):**
1. **RED FIRST** — timing-variance test in `test_sapling_crypto.c`
   that fails today because the means diverge.
2. **GREEN** — constant-time select. Test passes.
3. **Mark done** — update `AGENT.md` row + current NOW.

**Cross-cutting check:** P9.10 is about cache-side-channel on the
MSM witness — related but distinct. P9.1 is the timing channel on
the blinding scalars. Don't conflate.

---

## POST-P9 WORK QUEUE — the shining-example roadmap (Agent-3 lane)

Filed 2026-04-20 after the coordinator's full review + Erigon / Caplin
architecture study. Work the list **in order** unless you hit a blocker.

Every row has a full description in [`AGENT.md`](AGENT.md).

### Phase 0 — Finish P9 sapling audit (CURRENT)

- [x] **P9.4** HIGH — `fr_fft` / `fr_fft_parallel` silent no-op. done f5a31b48d [test:1.0].
- [ ] **P9.1** HIGH — `g1_scalar_mul` variable-time double-and-add leaks Groth16 blinding (side-channel). **Agent-3 NOW.**
- [ ] **P9.2** CRITICAL — `sapling_circuit.c:65 / 161-162` placeholder UB paths (if still live).
- [ ] **P9.6** MED — `zclassic_sapling_spend_proof` witness length not bounded.
- [ ] **P9.7** MED — `sprout_verify_groth16` size_t underflow + race on `sprout_set_vk(NULL)`.
- [ ] **P9.8** MED — `ensure_generators` 256-retry silent exhaustion.
- [ ] **P9.9** MED — prover printf leaks wallet-activity timing.
- [ ] **P9.10** LOW — `msm_parallel` cache-side-channel on witness (after threat-model decision).
- [ ] **P9.11** LOW — `zip32_diversifier` `for(;;)` cap at 256 iterations.

### Phase 1 — MVP CI gates (continue Agent-3 lane)

- [ ] **P11.4** — shielded-payment CI gate (MVP #4).
- [ ] **P11.5** — store e2e CI gate (MVP #5).
- [ ] **P11.6** HIGH — **7-day soak harness (MVP #6).** Biggest-leverage unfiled row in the tree. Design as a separate process that polls `zcl_status` every 60s, records RSS / tip-advance / crash events, asserts 7-day continuous run with no operator intervention.
- [ ] **P11.8** — parity-diff CI gate (MVP #8). Coupled with Agent-2's P12.3 + P12.3.1 and Agent-3's P17.5.

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
