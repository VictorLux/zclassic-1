# Next-session handoff — Phase 3 shipped, live test deferred

**Last session ended 2026-05-13 ~23:55.** Plan file:
`~/.claude/plans/look-i-need-you-velvet-mist.md`.

## What shipped

```
e639f1b4e fast-sync: direct LevelDB+mmap import (-fastimport)
```

Pushed to `origin/main`. Build clean, `make lint` clean, all 1500+ tests
pass.

## What the change does

A new CLI flag `-fastimport[=PATH]` (default `$HOME/.zclassic`) that
bypasses JSON-RPC entirely:

1. Opens zclassicd's `blocks/index/` LevelDB read-only and walks the
   `b`-prefixed keyspace, producing a height-ordered array of `(hash,
   nFile, nDataPos, nUndoPos, nStatus)`.
2. Maintains an 8-deep LRU of `mmap()`'d `blk*.dat` files; serves
   zero-copy payload pointers given `(nFile, nDataPos)`.
3. SHA3-spot-checks K=3 random anchor windows directly from mmap.
   Pass → arm `g_assume_valid_height = legacy_tip`. Fail → abort.
4. Sets `g_body_pull_active = 1`. In `connect_tip`, this gate:
   - swaps `block_tree_db_write_block_index_sync` for the async
     variant (no per-block fsync; LevelDB memtable batches the writes).
   - skips `wallet_sync_transaction` + Sapling trial-decrypt entirely.
5. Walks heights `[active_tip+1 .. legacy_tip]`, feeds payloads to
   `process_new_block`.
6. Runs `wallet_rescan` over the imported range when the loop
   completes — picks up any wallet hits that were skipped during the
   trust-mode walk.

Crash safety: coins.db still commits per block (at-tip kill-9
ordering preserved). On crash, block_index may be a handful of blocks
ahead in RAM but not durable; recovery rewinds to coins.db tip and
re-imports the gap.

## Constraint and why the live test is deferred

LevelDB acquires an exclusive LOCK file when opened. zclassicd holds
that LOCK while running, so a concurrent `-fastimport` open fails
with `LOCK held? stop zclassicd or snapshot the dir first`.

The plan's verification sequence stops both `zclassic23` and
`zclassicd-rhett` services, runs the import, then restarts. The user
chose `Commit + push, defer live test` rather than disturb the
running services. Phase 3 is ready to live-test whenever convenient.

## Live test sequence (copy-paste when ready)

```bash
# Current state snapshot (pre-test):
./tools/zcl-rpc getblockcount   # note zclassic23 tip
curl -s --data-binary '{"method":"getblockcount"}' \
     -u $(grep -E '^rpc(user|password)=' ~/.zclassic/zclassic.conf | \
          tr '\n' ':' | sed 's/rpcuser=//; s/rpcpassword=//; s/::*$//') \
     http://127.0.0.1:8232/                                # note zclassicd tip

# Stop both services
systemctl --user stop zclassic23
systemctl --user stop zclassicd-rhett

# Run fastimport (foreground, watch output)
./zclassic23 -datadir=$HOME/.zclassic-c23 \
  -fastimport=$HOME/.zclassic -nobgvalidation \
  -port=8033 -rpcport=18232 \
  > /tmp/fastimport.log 2>&1 &
PID=$!

# Look for in /tmp/fastimport.log:
#   [bilr] scanned=N usable=M max_height=...
#   [legacy_direct_import] legacy tip h=3,111,xxx
#   [legacy_direct_import] SHA3 spotcheck: K=3 ... 3/3 windows match
#   [legacy_direct_import] trust-mode armed
#   [legacy_direct_import] applied=N rate=>=100 bps
#   [legacy_direct_import] walk complete: ... final_tip=3,111,xxx
#   [legacy_direct_import] wallet rescan complete

# Wait for shutdown cleanup, then restart services
until ! kill -0 $PID 2>/dev/null; do sleep 1; done
systemctl --user start zclassicd-rhett
systemctl --user start zclassic23

# Verify
./tools/zcl-rpc getblockcount   # should match zclassicd tip
```

Expected outcome: sustained ≥100 bps; 10K-block catch-up in <90 s.

## What to look for in the log

- `[bilr] scanned=...` — confirms zclassicd's LevelDB opened.
- `[bilr] max_height=...` — should be the live zclassicd tip.
- `SHA3 spotcheck: K=3 ... 3/3 windows match` — anchor table agrees.
- `trust-mode armed: assume_valid X -> Y` — the bump.
- `rate=...` lines — must be > 30 bps to validate the unlock; > 100
  to validate the throughput target.
- Final `walk complete: applied=N` — N should equal `legacy_tip -
  active_tip`.

## Failure modes to expect

- `[bilr] open failed: ... LOCK held?` — zclassicd not fully stopped.
  Wait 5s and retry.
- `spotcheck FAILED at window K` — zclassicd's blocks don't match the
  compile-time anchor table. Likely zclassicd is on a different fork
  (testnet?) or the anchor table is stale. Sanity-check chain params.
- `process_new_block FAILED: ...` — a block failed validation. Check
  reject_reason. With trust-mode armed, only Merkle root / coinbase
  / ZIP-209 / UTXO-have-inputs checks remain — a failure here is
  meaningful.
- DROP INDEX failure log — known pre-existing issue;
  synchronous=OFF still engages, just without the index-drop benefit.

## If the rate is below 30 bps

The block_index sync-write deferral didn't engage. Check
`g_body_pull_active` is 1 by adding a debug log in `connect_tip`'s
gate, or run with `strace -c -e trace=fsync` to count fsyncs.

If wallet rescan dominates the runtime (large wallet), consider
adding `-norescan` flag or making it async.

## Open issues / next-pass priorities

- **Shallow-snapshot helper** for zero-downtime use: hardlink `.ldb`
  files + cp MANIFEST/CURRENT/LOG to a tempdir, open that. Unblocks
  fastimport while zclassicd keeps running. ETA ~1-2 hr.
- **Phase 4 rolling anchor**: `tools/gen_sha3_windows --max-height=tip`
  against zclassicd to refresh the compile-time anchor table; commit
  + ship per release. ETA ~1 hr operator step + ~5 min compile-time
  generation.
- **Phase 5 heartbeat auto-recovery**: when `active_tip < remote_tip
  - 100` for >60s and `~/.zclassic` exists, auto-trigger fastimport
  (or bodypull-from-legacy if LOCK held). Removes the manual ritual
  entirely. ETA ~2 hr.
- **node.db DROP INDEX during turbo enter** still fails (coins_view_sqlite
  table lock). Either reorder or accept reduced turbo benefit. Not
  blocking Phase 3.

## Memory entries to consult next session

- `project_fast_sync_phase3_2026-05-13.md` — what shipped this session
- `feedback_fast_sync_phase1_findings.md` — why Phase 1 didn't suffice
- `feedback_at_tip_kill9_ordering_invariant.md` — preserve when adding
  more I/O deferral
- `reference_zclassicd_local_fast_sync.md` — datadir layout
