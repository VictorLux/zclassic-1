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

## Current status — 2026-04-19 (afternoon, P7.1 + P2.2 shipped)

**Done and on main (32 rows + 1 infra):** P1.1, P1.2, P1.5, P6.1–P6.6,
P3.1–P3.7, P5.1, P5.2, P5.3, P5.4, P5.6, P5.7, P4.3, P4.4, P4.5,
P2.2 (`352a83167`), P2.3, P2.4, P2.5, P2.6, P2.7, P2.8, P7.1
(`a6bedccad`), P7.2, P7.3, P7.5, P7.6, P7.7, P7.8. Plus parallel
test runner infra (`df5de36c4`).

P7.1 was the live-outage CRIT — the `update_tip` silent-drop that
turned any csr commit rejection into an unrecoverable stuck-tip loop.
Fix shipped; Rhett still needs to `make deploy` for production to
pick it up.

**Open queue (4 logical tasks):**

| Order | Row | Size | Severity |
|---|---|---|---|
| NOW | **P2.1** wire tx validation into `msg_tx` handler | medium | CRIT |
| NEXT[1] | **P4.1 + P4.2** script interpreter stack refactor (paired) | large | HIGH |
| NEXT[2] | **P7.4** backpressure watchdog under tip-stuck | medium | HIGH |
| NEXT[3] | **P7.9 + P7.10** thread registry + shutdown audit (paired) | large | HIGH |

---

## NOW — P2.1: wire tx validation into `msg_tx` handler

File: `lib/net/src/msg_tx.c:34-69` (the handler that accepts incoming
`tx` P2P messages). Now the last open P-tier CRIT. Same file you've
touched for P2.2/P2.3/P2.4 — familiar territory.

### Bug

Incoming `tx` messages go straight into the mempool with no
signature / UTXO / fee check. An attacker can flood the node with
invalid, double-spending, or zero-fee transactions to exhaust memory
or poison the mempool for other peers.

### Fix

Wire `check_transaction` (from `lib/validation/`) + mempool
accept-policy (min-relay-fee, per-peer quota) INTO the handler before
the `tx_mempool_add` call. Rough shape:

```c
/* Inside handle_tx_message */
struct tx_validation_result r = check_transaction(&tx, &state);
if (!r.ok) {
    peer_score_decrement(peer, r.severity);   /* existing infra */
    event_emit(EV_TX_REJECTED, peer->id, hash.data, 32);
    return true;  /* consumed the message, just reject the tx */
}
if (tx_fee_below_min_relay(&tx, mempool)) {
    /* rate-limit, not malicious — drop silently, no ban-score */
    event_emit(EV_TX_BELOW_RELAY_FEE, peer->id, hash.data, 32);
    return true;
}
if (tx_mempool_has_conflict(mempool, &tx)) {
    peer_score_decrement(peer, SCORE_DOUBLE_SPEND);
    event_emit(EV_TX_DOUBLE_SPEND, peer->id, hash.data, 32);
    return true;
}
/* only now: insert into mempool */
tx_mempool_add(mempool, &tx);
```

The specific function names above are suggestive — use whatever
already exists in `lib/validation/` + `lib/policy/` + the mempool
helpers. Don't invent a new validation pipeline.

### Acceptance (3 regression tests in `lib/test/src/test_mempool.c`)

1. **Invalid signature.** Forge a tx with a wrong scriptSig sig →
   handler rejects, mempool size unchanged, peer ban-score
   incremented.
2. **Double-spend vs current mempool.** Two txs spending the same
   UTXO → first accepted, second rejected, second peer's ban-score
   incremented.
3. **Below-relay-fee.** Tx at fee rate 0 → silently rejected, mempool
   size unchanged, peer ban-score UNCHANGED (rate-limit, not
   malicious).

Also: the full `./test_zcl` must stay green. No deadlock under
concurrent-peer stress (if you've wired new locks, add a pthread
stress test).

### Commit template

```
net: reject invalid / double-spend / below-fee txs at msg_tx handler (P2.1)

Fixes P2.1 (AGENT.md). Before: any peer could flood the mempool
with invalid txs — no sig check, no UTXO check, no fee check.
After: check_transaction + mempool policy run before tx_mempool_add;
malicious peers get ban-score, rate-limit violators drop silently.
3 regression tests in test_mempool.c cover invalid-sig,
double-spend, and below-relay-fee paths.
```

---

## NEXT — queue (pre-authorized, in order)

### NEXT[1]: P4.1 + P4.2 — script interpreter stack refactor

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

### NEXT[2]: P7.4 — backpressure watchdog under tip-stuck

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

### NEXT[3]: P7.9 + P7.10 — thread registry + shutdown flag audit

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

### 2026-04-19 (afternoon) — P7.1 + P2.2 landed

**P7.1 (`a6bedccad`):** root cause was `update_tip` at
`process_block.c:525` silently discarding the bool return from
`process_block_commit_tip`. Any csr rejection (coins mismatch, tip
not in index, OOM, etc.) converted into unrecoverable stuck-tip loop
— `EV_TIP_UPDATED` fired with stale height, `active_chain_tip`
never advanced, every inbound block re-triggered the same validation
pass, download queue buffered to 6 GB OOM.

Fix: `update_tip` now returns bool, `EV_TIP_UPDATED` only fires
after commit succeeds, `connect_tip` records
`validation_state_error("csr-tip-commit-rejected")` on rejection.
Regression test in `test_chain_state_repo.c` fixtures a csr rejection
and asserts the error propagates + chain_tip unchanged.

**P2.2 (`352a83167`):** heap-allocated `process_mempool` scratch
(1.6 MB → `zcl_malloc`); 2 regression tests in `test_mempool.c`.

**Rhett action item:** the P7.1 fix is on main but production is
still stuck at h=3,081,411 because nothing redeployed. A `make
deploy` will unstick the live chain.
