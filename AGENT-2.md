# AGENT-2 — Wallet / Storage / App-Layer / Net / Validation

**Derived from:** the 2026-04-17 code review + the 2026-04-19 live-node
gap analysis. See `AGENT.md` for the cross-agent priority table.

**Working directory:** `~/zclassic23-2` (separate clone; pushes to `origin/main`).
**Coordinator:** Rhett (`~/zclassic23`), coordinator-only — does not code.
**Sibling:** Agent-3 (`~/zclassic23-3`), in the crypto / sapling / consensus-crypto lane.

---

## Lane — what you may edit

**Full edit access:**
- `lib/wallet/`, `lib/storage/`, `lib/coins/`
- `app/controllers/`, `app/services/`, `app/models/`, `app/views/`
- `tools/mcp/`, `tools/mcp/controllers/`
- `lib/test/` — add/modify tests for your changes
- Repo-root hygiene: `.gitignore`, tracked binaries

**Lane expansion (since Rhett became coordinator-only on 2026-04-19):**
- `lib/net/` — owned since P2.3/P2.4/P2.5/P2.6/P2.7/P2.8
- `lib/validation/` — owned since P4.4, P7.1, P7.2
- `lib/script/` — expansion for P4.1/P4.2 interpreter refactor
- `deploy/zclassic23.service` — owned since P5.2/P7.5–P7.7
- `lib/util/` — expansion for P7.9 thread registry
- `lib/event/` — owned since P7.3

**Read-only / off-limits:**
- `lib/crypto/`, `lib/sapling/`, `lib/keys/` — Agent-3
- `lib/validation/src/sigops.c`, `lib/validation/src/check_block.c` — Agent-3 (P1.6, P1.7)
- `vendor/` — Agent-3 owns `vendor/tor` (P5.5); all other vendor dirs are pinned

**STOP + ping Rhett triggers:**
- Any change to on-disk serialization (blocks, UTXOs, wallet keystore)
- Any change to consensus constants (MAX_BLOCK_SIZE, MAX_BLOCK_SIGOPS, MAX_P2SH_SIGOPS)
- Any change to the P2P protocol wire format

---

## Current status — 2026-04-19 (evening, P2.1 shipped)

**Done and on main (33 rows + 2 infra):** P1.1, P1.2, P1.5, P6.1–P6.6,
P3.1–P3.7, P5.1, P5.2, P5.3, P5.4, P5.6, P5.7, P4.3, P4.4, P4.5,
P2.1 (`da318931d`), P2.2 (`352a83167`), P2.3, P2.4, P2.5, P2.6, P2.7,
P2.8, P7.1 (`a6bedccad`), P7.2, P7.3, P7.5, P7.6, P7.7, P7.8. Plus
parallel test runner infra (`df5de36c4`) + block_pruning self-deadlock
fix uncovered while running P2.1 tests (`3979340c9`).

With P2.1 landed all P-tier CRIT rows are closed. Rhett still needs
to `make deploy` for production to pick up the P7.1 fix — chain is
still stuck at h=3,081,411.

**Open queue (3 logical tasks, all HIGH):**

| Order | Row | Size | Severity |
|---|---|---|---|
| NOW | **P4.1 + P4.2** script interpreter stack refactor (paired) | large | HIGH |
| NEXT[1] | **P7.4** backpressure watchdog under tip-stuck | medium | HIGH |
| NEXT[2] | **P7.9 + P7.10** thread registry + shutdown audit (paired) | large | HIGH |

---

## NOW — P4.1 + P4.2: script interpreter stack refactor

Files: `lib/script/include/script/interpreter.h:22-30`,
`lib/script/src/interpreter.c:619-652`.

**Bug (P4.1).** `struct script_stack` is 520 KB passed BY VALUE into
every recursive EVAL frame — on-stack.
**Bug (P4.2).** `stack_push` can fail silently under pressure; later
`OP_PICK` / `OP_ROLL` assume the stack shape is intact.

**Fix.** Pair these in one commit. Convert `script_stack` to a
pointer-owned heap buffer (the interpreter frame owns it, passes a
pointer into child frames). Make `stack_push` return `bool` and
propagate the failure up to `eval_script`. Every `stack_push` call
site now checks the return.

**STOP + ping Rhett:** any change to the serialized script format or
the set of accepted opcodes.

**Acceptance:** full `./test_zcl` + ASAN passes. New deep-recursion
test pushes 100 nested `OP_IF` frames and asserts graceful exit (no
SIGSEGV, no memory growth past 10 MB for the interpreter frame).

---

## NEXT — queue (pre-authorized, in order)

### NEXT[1]: P7.4 — backpressure watchdog under tip-stuck

Files: `lib/net/src/msgprocessor.c`, `lib/net/src/download.c`.

**Bug.** When chain_tip doesn't advance (P7.1-class bug or long fork
reorg), download buffers accumulate unbounded (observed 6 GB RSS peak
that triggered the OOM).

**Fix.** Watchdog: if `chain_tip` unchanged for N=60s AND download
queue >M=256 MB, drain the queue, refuse new block-inv for K=120s,
emit `EV_BACKPRESSURE_ACTIVE`.

**Acceptance:** test fixtures a stuck-tip scenario, asserts watchdog
fires + RSS stays bounded. Watchdog clears cleanly when the tip
resumes advancing.

### NEXT[2]: P7.9 + P7.10 — thread registry + shutdown flag audit

Files: new `lib/util/src/thread_registry.c` + every `pthread_create`
site (12+ known — grep for them).

**Bug (P7.9).** No central registry of spawned threads; SIGTERM
shutdown hits the 5-min timeout because 12 independent flags aren't
all checked.
**Bug (P7.10).** `g_shutdown_requested` is read in only 6 files;
`bg_validation`, `header_sync`, `peer_strategy`, `scheduler`,
`workpool` either ignore it or check a different flag.

**Fix.** New `thread_registry_spawn(name, fn, arg)` wraps
pthread_create + records tid with a shutdown callback.
`thread_registry_shutdown()` iterates, signals, joins with 10s
per-thread timeout. Every long-running loop checks the registry's
shutdown flag (single source of truth).

**Acceptance:** `systemctl --user restart zclassic23` completes in
<30s (not 5min). Stress test spawns 50 registered threads and asserts
all join on shutdown signal.

---

## Preflight — run verbatim before starting

```bash
cd ~/zclassic23-2
git fetch origin
git checkout main
git reset --hard origin/main
cat CLAUDE.md DEFENSIVE_CODING.md AGENT.md AGENT-2.md
make -j"$(nproc)" && ./test_zcl
```

If build or tests fail — STOP and report.

---

## Commit protocol

- One logical fix per commit.
- Every commit: `make test` passes. Every push: `make ci` passes.
- Commit body cites file:line from AGENT.md + ends with `Fixes P<N> (AGENT.md).`
- After each push, update the AGENT.md row to `done <SHA>`.
- Push frequently. Never `--amend` pushed commits. Never `--force-push`.
- Never log secret material.

---

## Coordination rules

- Agent-3 owns crypto/sapling/keys + P1.6/P1.7 consensus + P5.5 vendor/tor. Don't diff their files.
- Rhett is coordinator-only. When NOW + NEXT are empty, ping Rhett.
- Out-of-scope discoveries → append to "Notes from Agent-2" below.

---

## Notes from Agent-2

_(Keep short — 1-3 recent entries.)_

### 2026-04-19 (evening) — P2.1 landed + out-of-scope deadlock fix

**P2.1 (`da318931d`):** refactored `accept_to_mempool` into
`msg_tx_classify` (pure) + `msg_tx_accept` (classify + peer scoring)
returning `enum tx_accept_result` with 7 outcomes. Only INVALID and
CONFLICT trigger `peer_scoring_record(PEER_OFFENCE_INVALID_MESSAGE)`;
BELOW_FEE / MISSING_INPUTS / DUPLICATE / INTERNAL_ERROR drop
silently because they're rate-limit / our-problem, not misbehaviour.

Fee is computed from the coins tip (`coins_view_cache_get_value_in -
transaction_get_value_out`), with a missing-inputs branch that
returns MISSING_INPUTS for orphan txs. Added
`tx_mempool_has_conflict` read-only probe to `lib/validation/` so
double-spends can be attributed to the sending peer before
`tx_mempool_add_unchecked` folds them into its generic failure bool.

3 regression tests in `test_mempool.c` — invalid vout (→ INVALID +
ban), double-spend between two peers (first OK / second CONFLICT +
ban), below-relay-fee (→ BELOW_FEE + no ban).

**Bonus (`3979340c9`):** uncovered a self-deadlock while running the
full test suite end-to-end — `block_pruning_service:161-165`
acquired `disk_block_io_lock()` then called
`disk_block_io_close_cache()` which re-locks the same NORMAL mutex.
The bug dates back to wave 22 `e7d96bf01` ("fix lock race — hold
lock across unlink"). In production it would freeze the node on the
first pruned file. Fixed by adding `_while_locked` variant; pruning
service switched to it. Out-of-scope for P2.1 but in-lane (storage
+ app/services) and blocking the test-zcl run needed to validate
P2.1.

**Rhett action item (unchanged):** the P7.1 fix is still on main
only — production at h=3,081,411. `make deploy` needed.
