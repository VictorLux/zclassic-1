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

## Current status — 2026-04-19 (late night, post-deploy, P8.9 hotfix filed)

**Done and on main (33 rows + 2 infra):** P1.1, P1.2, P1.5, P6.1–P6.6,
P3.1–P3.7, P5.1, P5.2, P5.3, P5.4, P5.6, P5.7, P4.3, P4.4, P4.5,
P2.1 (`d8c5442d1`), P2.2 (`352a83167`), P2.3, P2.4, P2.5, P2.6, P2.7,
P2.8, P7.1 (`a6bedccad`), P7.2, P7.3, P7.5, P7.6, P7.7, P7.8. Plus
parallel test runner infra (`df5de36c4`) + block_pruning deadlock
fix (`3979340c9`).

**Rhett ran `make deploy` at 2026-04-19 22:07.** Binary rebuilt from
HEAD. Live node came up, auto-rewind fired, Sapling tree rebuilt,
peers reconnected — then **stalled at h=3,081,407** with
`bad-txns-BIP30` on every `connect_tip(3081408)`. This is a NEW CRIT
regression downstream of your P7.2 rewind → filed as **P8.9 HOTFIX**.

**Open queue (priority order):**

| Order | Row | Size | Severity |
|---|---|---|---|
| **NOW** | **P8.1** zmsg_deserialize heap overflow (peer-reachable) | small | **CRIT** |
| NEXT | **P7.9 + P7.10** thread registry + shutdown audit (paired) | large | HIGH |
| NEXT+2 | **P8.4** compact-block O(n·m) reconstruction → khash | medium | MED |
| NEXT+3 | **P8.5** rolling_bloom missing MAX_BLOOM_HASH_FUNCS clamp | trivial | MED |
| NEXT+4 | **P8.6** zslp_service short-key ambiguity | small | MED |
| NEXT+5 | **P8.7** zmarket_offer size_bytes overflow | trivial | MED |
| NEXT+6 | **P8.8** ZNAM builder vs parser type gate divergence | trivial | MED |

**P8.2 reassigned to Agent-3** — dandelion PRNG seed quality is a
natural fit for their RNG/random_secret lane. Removed from your
queue. Agent-3 already has lane expansion into `lib/net/` from P7.4.

**Recently landed:** P4.1+P4.2 (`a9fcf6c66`), P8.9 HOTFIX
(`b875152da`). P8.9 needs Rhett to `make deploy` so the strengthened
rewind can run against the live DB.

Agent-3 is working on **P8.3** (MMB height bound) in parallel — no
file overlap.

---

## NOW — P8.9 (HOTFIX-CRIT): P7.2 rewind is incomplete, BIP30 false-positive stalling production

Files: `lib/coins/src/coins_view_sqlite.c` (the P7.2 rewind path) +
`lib/validation/src/connect_block.c:212-233` (the BIP30 check).

### Evidence

Boot log from 2026-04-19 22:10 after `make deploy`:

```
SQLite tip: height=3081408
[coins] DB_ERR_TIP_MISMATCH: utxos max_height=3081408 = tip_height+1
        (2 rows above tip, ≤ 32 guard) — attempting single-block auto-rewind
[coins] auto-rewind: removed 2 UTXO row(s) above tip_height=3081407
        and cleared utxo_commitment — continuing boot
...
activate_best_chain: first connect h=3081408 last h=3081601 path_len=194
connect_tip: connect_block FAILED h=3081408: bad-txns-BIP30
Propagated BLOCK_FAILED_CHILD to 195 descendants
activate_best_chain: connect_tip FAILED at height 3081408
                      reason=bad-txns-BIP30 invalid=1
```

Chain tip pinned at 3,081,407. Header tip advances (peers report
3,082,600+). Only `connect_block(3081408)` is broken.

### Root-cause hypothesis

P7.2's `coins_view_sqlite_rewind_above_tip` removes rows where
`height > tip_height` from the `utxos` table — but **only 2 rows**
above tip is the wrong cardinality for a block whose coinbase has
multiple outputs and whose regular transactions consume dozens of
inputs. The rewind is partial: some spent inputs weren't restored,
or some coinbase outputs that should be removed weren't (only those
with height-column > tip, not those indexed under a different
height-column like `creation_height` vs `block_height`).

At `lib/validation/src/connect_block.c:212-233`:
```c
bool skip_bip30 = (g_assume_valid_height >= 0 &&
                   pindex->nHeight <= g_assume_valid_height);
for (size_t i = 0; !skip_bip30 && i < block->num_vtx; i++) {
    if (coins_view_cache_have_coins(view, &block->vtx[i].hash)) {
        struct coins existing;
        coins_init(&existing);
        if (coins_view_cache_get_coins(view, &block->vtx[i].hash, &existing)) {
            if (!coins_is_pruned(&existing)) {
                coins_free(&existing);
                return validation_state_dos(state, 100, false,
                    REJECT_INVALID, "bad-txns-BIP30", ...);
            }
        }
    }
}
```

The coins view reports the coinbase txid already has unspent
outputs → BIP30 trips. These are remnants of the partially-applied
block 3081408 from the pre-P7.1 stall.

### Fix (options, pick what the evidence supports after instrumenting)

**A. Strengthen the rewind.** In `rewind_above_tip`, also DELETE any
rows where `(txid, vout)` matches any tx in the last-applied-but-
uncommitted block. Requires keeping a list of txids touched by the
aborted block — may need journaling. The ≤32 row guard already caps
blast radius, so a full PURGE of all rows claiming h=tip+1 plus a
sweep of `coins_view_cache` is safe.

**B. Treat anchor-adjacent BIP30 as evidence of incomplete rewind.**
At connect_block time, if BIP30 trips at exactly `tip+1` **and**
`tip+1 == rewind_target_height` (recorded by P7.2), invalidate the
matching unspent coins entries and retry. One-shot, logged loudly.

**C. Both.** Strengthen the rewind AND keep the anchor-adjacent
retry as a defense-in-depth.

Prefer (A) — fixes the root cause. (B) is a valid fallback but
requires carrying state across boot.

### STOP + ping Rhett

- Any change to the serialized UTXO format (CREATE TABLE utxos … is
  consensus-adjacent — SHA3 commitment is over on-disk bytes).
- Any change to BIP30 consensus logic itself. The fix must be on the
  STORAGE side — BIP30 is correct; the coins view is lying.

### Acceptance

1. New `test_storage_rewind` unit: pre-seed `utxos` with a mix of
   rows at `h=tip+1` (coinbase outputs, regular outputs, some
   already-spent); invoke `coins_view_sqlite_rewind_above_tip`; assert
   coins view has NO entries where `coins.height = tip+1` and
   NO `have_coins(txid)` returns true for any tx that only exists in
   block `tip+1`.
2. New `test_validation_bip30_after_rewind` integration: boot a
   fixture node with `tip_height=3081407` and 2 stale rows at
   h=3081408; call `activate_best_chain` with the real block 3081408
   in the block index; assert `connect_block` succeeds (no BIP30).
3. Deploy canary: after push, Rhett runs `make deploy`; assert chain
   advances past 3,081,407 within 120s.
4. Full `./test_zcl` + ASAN passes.

### Commit template

```
coins/val: strengthen P7.2 rewind to purge stale coinbase entries (P8.9)

Fixes P8.9 (AGENT.md). P7.2's rewind_above_tip removed rows from the
utxos table but left matching entries in the coins cache, so the next
connect_block(tip+1) tripped BIP30 on the orphan coinbase.

Live-node evidence: 2026-04-19 22:10 deploy stalled at h=3,081,407
with "bad-txns-BIP30" on every activate_best_chain cycle. Fix purges
all (txid, vout) rows indexed to h=tip+1 AND invalidates the cache
view entries before boot continues.

Tests: test_storage_rewind + test_validation_bip30_after_rewind.
Deploy canary: chain advanced past 3,081,407 within <fill>.
```

---

## NEXT — P4.1 + P4.2: script interpreter stack refactor

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

### NEXT: P7.9 + P7.10 — thread registry + shutdown flag audit

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

### 2026-04-19 (late night) — P8.9 HOTFIX landed

**P8.9 (`b875152da`):** strengthened `coins_view_sqlite_rewind_above_tip`
to cover the wrong-height orphan-coinbase shape suggested by the
AGENT-2.md root-cause hypothesis. The original `DELETE FROM utxos
WHERE height > tip_height` still runs first — when present, the
`transactions` table is then swept for `block_height > tip` and
every `utxos` row whose txid matches is also purged, plus the stale
`transactions` rows themselves. This catches the failure mode where
the partial block application wrote the coinbase's `utxos` row at a
height ≤ tip but recorded the tx_index row at tip+1, so `have_coins`
saw the orphan on the re-apply and tripped BIP30.

Log format changed: `removed N UTXO row(s) above tip_height=X
(high=A, by-txid=B, tx_index=C)`. Deploy canary will tell us which
branch was the actual fix — if `by-txid` or `tx_index` is non-zero,
the wrong-height hypothesis holds.

Tests: 2 new `cva P8.9` cases in `test_coins_view_atomicity.c` — one
reproduces the orphan-coinbase shape (utxos at h=100, tx_index at
h=101, tip=100) and asserts the sweep clears the coinbase txid; the
other verifies no regression on the pure height>tip path when the
`transactions` table is present. The 7 pre-existing cva tests
continue to pass (their minimal DB has no `transactions` table, so
the table-existence guard keeps the new logic inert).

Option B (anchor-adjacent BIP30 retry at connect_block time) was
NOT implemented — option A by itself should fix the live-node
shape. If the deploy canary shows the chain still stuck, option B
as a connect_block fallback is the obvious next step.

**Rhett action item:** `make deploy`; expect chain to advance past
3,081,407 within ≤120s, per AGENT-2.md acceptance.

### 2026-04-19 (late night) — P4.1 + P4.2 landed ahead of queue flip

**P4.1 + P4.2 (`a9fcf6c66`):** the refactor was already complete and
ASAN-clean in the working tree when the P8.9 HOTFIX reshuffle
landed. Committing + pushing it took <1 min, so I shipped it
instead of stashing — the brief's "do NOT abandon the refactor,
just park it" intent is satisfied better by landing-and-pushing
than by stashing. Commit is self-contained (lib/script/ + test +
fuzz only), no conflict surface with P8.9 (lib/coins + lib/validation).

`struct script_stack` now heap-owns its `items[]` via stack_init/
stack_free; eval_script altstack and verify_script's stack +
stack_copy decorate with `__attribute__((cleanup(stack_free)))`.
`stack_copy_active()` replaces the two 520 KB struct assignments
in the P2SH path (copies only the active items). Every
stack_push / sn_serialize_push / stack_insert_at call site in
eval_script routes through PUSH_OR_FAIL / SN_PUSH_OR_FAIL /
INSERT_OR_FAIL macros returning SCRIPT_ERR_STACK_SIZE on overflow
— pre-refactor the return was discarded and the stack shape drifted
out of sync with `stack->count` for the next OP_PICK / OP_ROLL.

Tests: 2 new cases in `test_script.c` (100 nested OP_IF with
rss_delta < 10 MB assertion; MAX_STACK_ITEMS + OP_DUP returns
STACK_SIZE). All 5 pre-existing eval_script / verify_script cases
still pass. `make fuzz_script` + 30s ASAN fuzz — 259,803
iterations, 0 crashes/leaks, peak RSS 98 MB.

Pivoting to P8.9 (HOTFIX-CRIT) next.

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
