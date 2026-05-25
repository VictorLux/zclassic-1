# wt-bip30-stale-coins-unwedge — clear the stale-coinbase BIP30 wedge

## Status

**READY — HIGHEST PRIORITY (this is the live wedge).** Independent of the
Phase-2 cutover soak. Claim by marking `IN PROGRESS (wtN)`.

> Goal moved: **UNBREAKABLE — Tip advancing / Wedge recovery**. This is the
> bug freezing the live node at height 3,123,688 right now.

---

## TL;DR — what's actually broken (root-caused 2026-05-25, evidence below)

The live node is frozen at **3,123,688**, rejecting block **3,123,689** with
`bad-txns-BIP30` on every retry. This is **not** a cutover divergence (that was
the earlier wrong guess). It is the documented **2026-04-19 BIP30 stall shape**
(`docs/archive/2026-04/2026-04-19-bip30-stall.md`): a torn coins write left the
coinbase of block 3,123,689 as a single stale unspent UTXO row **one block above
the chain tip**, so BIP30 sees it as a duplicate-coinbase overwrite and refuses
the block forever.

The trigger was the C-3 cutover flap → `chain_tip_watchdog` kill-9'd the node 12×
→ one kill hit the at-tip ordering hazard (`coins.db` commits *before* the
block_index fsync — see `feedback_at_tip_kill9_ordering_invariant`). The three
already-merged "wedge" fixes (`6e0f6a82c`, `47bdbc211`, `82ec4e11f`) stop the
*trigger* but do **not** clear the stale row — a plain deploy leaves the node
wedged.

## Evidence (read-only `~/.zclassic-c23/node.db`, all reproducible)

```
chain tip (active)         3,123,688   (block 3,123,689 NOT in sqlite `blocks` table)
coins_best_block anchor →  3,123,688   (status 11, resolves cleanly)
utxos MAX(height)          3,123,689   ← 1 row, the stale coinbase
utxos WHERE height>3123688 → exactly 1 row at 3,123,689
transactions WHERE block_height>3123688 → 0 rows
node.log: connect_block FAILED h=3123689: bad-txns-BIP30   (repeating)
node.log: STALL h=3123688 entries_at_3123689=1 (data=1 fail=1)
```

This is **textbook case (e)** of the existing boot guard
(`lib/storage/src/coins_view_sqlite.c:309`):
`max_utxo_height (3689) == tip_height (3688) + 1` AND `count(height>tip)=1 ≤ 32`
→ should auto-rewind and delete the row.

**Why it never self-heals (leading hypothesis — your Task 1 confirms it):** that
guard lives inside `coins_view_sqlite_open` (`coins_view_sqlite.c:498`). Its
stderr mismatch/rewind strings (`auto-rewind`, `DB_ERR_TIP_MISMATCH`) have **0
occurrences in `node.log` across all 12 restarts**, and the stale row persists —
so the case-(e) rewind is not clearing it on the live path. Most likely the
production coins-view open (`node_state.leveldb_utxo_migrated=01`) bypasses this
SQLite guard. **Caveat — do not trust log absence alone:** the guard's *healthy*
paths print to stdout, which this node may not route to `node.log`; only the
stderr path is guaranteed captured. **Trace the actual coins-open path in
`config/src/boot.c` to confirm whether the guard is invoked at all** before
choosing fix (a) vs (b).

## The fix — two parts

### Task 1 — RED regression test (do this FIRST)

**Scope:** `lib/test/src/test_chain_stall_repro.c` (BIP30 domain already lives
here) or a new `test_stale_coinbase_above_tip_unwedge.c`.

Build a node.db fixture in the wedged shape: chain tip at H, `coins_best_block`
→ H, and one `utxos` row (a coinbase txid) at height H+1, with H+1 absent from
the `blocks` table. Then drive the **actual live boot/coins-open path** the
production node uses (the one that bypasses the SQLite guard — confirm which by
tracing how `g_coins_tip` is opened in `config/src/boot.c`). Assert the node
ends boot with `have_coins(coinbase_H+1)` **false** (row cleared) so a
subsequent `connect_block(H+1)` would not trip BIP30.

**Acceptance:** test is RED on current HEAD (reproduces the wedge), GREEN after
Task 2. Name it for the invariant:
`test_stale_coinbase_above_tip_is_rewound_on_boot`.

### Task 2 — make the rewind run on the live boot path

**Scope:** `config/src/boot.c` (coins-view open / the existing
`utxo_recovery_clean_above_tip` call at `:3146`) and/or
`lib/storage/src/coins_view_sqlite.c`.

Pick the smaller correct fix:
- **(a)** Ensure `coins_view_sqlite_check_tip_consistency` (case (e) rewind) runs
  on the path the production node actually takes to open its coins view — not
  only the SQLite-direct `coins_view_sqlite_open`. OR
- **(b)** Extend the existing boot reconciliation (`boot.c:3006` only fires when
  UTXOs are **>100** above the anchor; `utxo_recovery_clean_above_tip` at
  `:3146`) so the **single-block** overshoot (`max_utxo == chain_tip+1`, count ≤
  `COINS_AUTO_REWIND_MAX_ROWS`) is rewound against the **block_index chain tip**,
  not just the coins-internal anchor.

**Invariant to preserve:** never wipe UTXOs more than 1 block / >32 rows above
tip without halting (the memory rule — see the `COINS_AUTO_REWIND_MAX_ROWS`
guard and `feedback_block_failed_mask_wedges_tip`). This fix must be a strict
single-block heal, not a broad "delete above tip."

**Acceptance:** Task 1 test GREEN. `make test_parallel` 0 failures. `make lint`.

### Task 3 — live unwedge verification (GATED on deploy; operator step)

Do NOT touch the running node without Rhett's go-ahead. When cleared:
`make deploy`, then the live forward-progress gate (RESILIENCE DOCTRINE #1):
```
SAMPLES=6 INTERVAL_SECS=15 ./tools/bench_running_lag.sh   # exit 0 = tip advancing
./tools/scoreboard.sh                                      # exit 0 = HEALTHY
```
Confirm the tip advances past **3,123,689** and `node.log` no longer logs
`bad-txns-BIP30`. Boot should also log the case-(e) rewind firing once.

## Non-goals

- **BIP30 itself.** It is correct (`connect_block.c:266`). The node is lying to
  it about which coinbases exist — fix the data, not the rule.
- **Snapshot re-sync.** `wt-snapshot-wedge-recovery.md` (PR-0) is the heavyweight
  fallback for arbitrary wedges. THIS is the targeted single-block fix; they're
  complementary. Don't re-snapshot 1.3M UTXOs to drop 1 stale row.
- **The cutover.** Already reverted to shadow (`6e0f6a82c`); separate track.

## References
- `docs/archive/2026-04/2026-04-19-bip30-stall.md` — the prior incident + the
  disconnect_block coins-erase fix that DID land (this is a different *path* to
  the same symptom: torn kill-9 write, not a disconnect).
- `lib/storage/src/coins_view_sqlite.c:138` (`rewind_above_tip`), `:229`
  (`check_tip_consistency`, case (e) at `:309`).
- `lib/validation/src/connect_block.c:246-272` (the BIP30 check).
- `config/src/boot.c:3006` (coins-far-above-tip promotion, >100 only) + `:3146`
  (`utxo_recovery_clean_above_tip`).
- Memory: `feedback_at_tip_kill9_ordering_invariant`,
  `feedback_block_failed_mask_wedges_tip`.
