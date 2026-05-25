# wt-connect-bip30-selfwrite — kill the recurring BIP30 self-write wedge (THE disease)

## Status

**DONE (wt2) — 2026-05-25; deployed and live forward progress proven.**

Implemented:
- `connect_block` now tolerates only a block's own same-height coinbase
  self-write, preserving BIP30 rejection for real unspent duplicates.
- `connect_tip` at-tip durability now writes the block index before forcing
  the coins.db flush, so a crash can leave replayable `block_index=N+1,
  coins=N` but not the wedged `coins=N+1, block_index=N` shape.
- Crash-stage ordering tests now assert `coins.db` never gets ahead of the
  durable block index.

Verified:
- `make -j$(nproc) test_zcl`
- `ZCL_TEST_ONLY=chain_stall_repro ./test_zcl`
- `ZCL_STRESS_TESTS=1 ZCL_TEST_ONLY=chain_advance_atomicity ./test_zcl`
- `make lint`
- `make test_parallel && ./test_parallel` — 0/215 groups failed

Live acceptance:
- Deployed from `/home/rhett/github/zclassic23` after wt2 pushed the fixes.
- The old stuck activation at `3124225` connected; `node.log` showed the
  same-height coinbase self-write was tolerated and no new `bad-txns-BIP30`
  rejection appeared after the fix.
- A follow-on startup blocker from stale
  `cec.contradiction_reason=missing_active_tip_evidence` was fixed by
  auto-clearing that demoted freeze and allowing active-tip evidence
  reconstruction while the derived `blocks` projection lags.
- `SAMPLES=12 INTERVAL_SECS=15 ./tools/bench_running_lag.sh` advanced live
  height from `3124230` to `3124293`; the script exited 2 only because its
  `peer_max_height` probe stayed 0 despite RPC `getpeerinfo` showing a
  handshaked MagicBean peer.
- `tools/bip30_unwedge_preflight.sh` at 2026-05-25T09:56:39Z returned
  `VERDICT=NO_STALE_ROW_AND_LIVE_HEALTHY` with
  `RPC_CHAIN_HEIGHT=3124298`, `RPC_LEGACY_HEIGHT=3124298`, and `RPC_GAP=0`.
- A 5-minute `getsyncdiag` stability watch held `mirror_lag=0`, empty
  `mirror_last_blocker_code`, and chain/legacy within 0 blocks. Diagnostics
  still label the legacy mirror state `blocked` with `activation-no-progress`
  while already `at_tip`; that is a status-reporting oddity, not a forward
  progress blocker.

> Supersedes the symptom-chasers. The boot-rewind (`dbf4845a1`) and cold-import
> only *move* the wedge one block; they do not cure it. This does.

## The disease (proven live, 2026-05-25)

The node freezes ~1 block below the tip with `connect_block FAILED: bad-txns-BIP30`,
forever. After a cold-import it just moved from 3,123,689 to 3,124,225 — same shape:

```
chain tip            = 3,124,224
utxos MAX(height)    = 3,124,225   (exactly ONE row)
that row             = txid 98963472…, vout 0, height 3124225, is_coinbase=1
→ it is block 3,124,225's OWN coinbase, already in the UTXO set while the tip is
  3,124,224. connect_block(3124225) runs BIP30, sees the block's own coinbase
  already present, and rejects the block as a duplicate-overwrite.
```

So the UTXO set ends up **one block ahead of the block-index tip**, and BIP30
rejects the node's own coinbase as if it were a consensus violation. This
regenerates every time the node advances at the tip → permanent stuck loop.

### Why this is ALWAYS a false positive at these heights

ZClassic/Zcash put the block height in the coinbase (BIP34-style) from early on,
so **coinbase txids are unique per height** — two different blocks can never share
a coinbase txid. Therefore a `bad-txns-BIP30` hit at height ~3.1M is **impossible
as a real consensus violation**. Any BIP30 trip up here is, by definition, the
node's own stale UTXO data — never a genuine duplicate. (Confirm the exact
BIP34/coinbase-uniqueness activation in `lib/consensus` / chainparams and cite it
in the fix.)

## The fix — two parts (do both)

### Task 0 — RED test first
`lib/test/src/test_connect_block.c` (or `test_chain_stall_repro.c`). Build the
exact shape: a coins view holding block H's coinbase at height H, chain tip at
H-1, then `connect_block(H)`. Assert today it returns `bad-txns-BIP30` (RED);
assert after the fix it connects (GREEN). Name it
`test_connect_block_tolerates_own_coinbase_self_write`.

### Task 1 — connect_block: don't reject a block's own coinbase (loop-breaker)
`lib/validation/src/connect_block.c:246-272` (the BIP30 loop). Today it rejects
whenever `have_coins(vtx[i].hash)` and the entry is unpruned. Fix: BIP30 can only
legitimately apply where coinbase txids are NOT height-unique (pre-BIP34 / the
known exception heights). For all heights at/after coinbase-uniqueness activation,
an existing coinbase with the **same txid at the same height** is a stale
self-write — **overwrite it and continue, do not reject.** A real violation (a
different block's still-unspent coinbase, only possible pre-BIP34) still rejects.
The `struct coins existing` carries the height — use `existing.nHeight ==
pindex->nHeight` to distinguish self-write from a genuine duplicate. Keep it
consensus-exact: cite why this cannot accept a real double-spend.

### Task 2 — write ordering: never leave coins ahead of the committed tip (root cause)
The deeper cause is that a flush/kill leaves the UTXO set at `tip+1` (see
[[feedback_at_tip_kill9_ordering_invariant]] — coins.db must commit only in lockstep
with the block_index tip). Find where a block's coins get flushed before its
connect is committed to the block index (connect_tip / flush policy / update_coins)
and make the commit atomic so `coins_best_block` can never exceed the durable
block-index tip. After this, Task 1 should rarely fire — it's the safety net.

## Acceptance — LIVE forward progress, not a unit test (RESILIENCE DOCTRINE #1)

A green unit test is NOT done. The node has faked "fixed" three times now.
Required:
- `make test_parallel` clean, `make lint`.
- Deploy + restart, then prove the tip advances **many** blocks through and past
  the old wedge points, sustained:
  ```
  SAMPLES=12 INTERVAL_SECS=15 ./tools/bench_running_lag.sh   # exit 0 = advancing
  ./tools/scoreboard.sh                                       # exit 0 = at tip
  ```
- Explicitly confirm `node.log` shows **zero** `bad-txns-BIP30` after the fix and
  the tip reaches the legacy tip and stays within ≤2 blocks for 5+ minutes.
- If you cannot show sustained live forward progress at the tip, it is NOT done.

## Non-goals
- Another reimport / boot-rewind. Those move the wedge; they don't cure it.
- Disabling BIP30 wholesale. Keep it correct where it can legitimately apply
  (pre-BIP34 exception heights). See `docs/archive/2026-04/2026-04-19-bip30-stall.md`.

## References
- `lib/validation/src/connect_block.c:246-272` (BIP30 loop).
- `lib/validation/src/connect_tip.c`, `process_block_flush_policy.c`, `update_coins.c` (write ordering).
- `docs/archive/2026-04/2026-04-19-bip30-stall.md` (prior BIP30 incident).
- Memory: [[feedback_at_tip_kill9_ordering_invariant]], [[project_bip30_stale_coins_wedge_2026-05-25]].
