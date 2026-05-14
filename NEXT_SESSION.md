# Next-session handoff — Phases 3 + 7 shipped, live tests deferred

**Last session ended 2026-05-14 ~00:15.** Plan file:
`~/.claude/plans/look-i-need-you-velvet-mist.md`.

## What shipped this session (3 commits)

```
e639f1b4e fast-sync: direct LevelDB+mmap import (-fastimport)
c63837ad1 session-end: Phase 3 handoff doc
db8b880cd fast-sync: -cold-import for state-only sync in ~60s
```

All on `origin/main`. Build clean, `make lint` clean, all tests pass.

## Two new CLI flags

### `-fastimport[=PATH]` — warm catch-up

Direct LevelDB+mmap path that replaces JSON-RPC for the body-pull
loop. Walks heights ascending feeding mmap'd payloads to
`process_new_block`. Per-block I/O deferral via `g_body_pull_active`
(block_index sync write → async; wallet trial-decrypt skipped). Auto
`wallet_rescan` over imported range at end. Target: ≥100 bps; 10K
blocks in <90 s.

### `-cold-import[=PATH]` — state-only sync (NEW, Phase 7)

The breakthrough. Refuses to run if `active_tip > 1000`. Bypasses
`process_new_block` entirely:

1. SHA3 spot-check K=5 anchor windows.
2. Hardlink legacy `blk*.dat` into our `blocks/`.
3. Bulk-copy legacy `blocks/index/` LevelDB → our LevelDB (5000-record
   batches, transparent obfuscation).
4. Bulk-import legacy `chainstate/` UTXOs → our `coins.db` (5000-record
   batches).
5. Set `coins_tip` best_block to legacy chainstate's 'B' key.

After it runs, normal boot continues:
- `block_index_loader` populates the in-memory block_map.
- `chain_restore_service` walks pprev from best_block, rebuilds active_chain.
- `bg_validation_service` re-verifies every block bit-exact over hours
  ("process every bit" guarantee).

Target: empty datadir → tip in ~30-60s.

## Constraint shared by both new paths

Opening zclassicd's `blocks/index/` LevelDB acquires an exclusive
LOCK. zclassicd must be stopped briefly. The existing
`-bodypull-from-legacy` (JSON-RPC) tolerates a running zclassicd and
is still available as the live-zclassicd-tolerant fallback.

## Live test sequence (copy-paste)

### Test 1: -cold-import (empty datadir)

```bash
# Save current state, prep a fresh empty datadir
mkdir -p $HOME/.zclassic-c23-cold-test

# Stop services
systemctl --user stop zclassic23
systemctl --user stop zclassicd-rhett

# Run cold-import (foreground, no_services for clean shutdown)
./zclassic23 -datadir=$HOME/.zclassic-c23-cold-test \
  -cold-import=$HOME/.zclassic -nobgvalidation \
  -port=18033 -rpcport=18233 \
  > /tmp/cold-import.log 2>&1 &
PID=$!

# Watch /tmp/cold-import.log for:
#   [bilr] scanned=N usable=M max_height=3,111,xxx
#   [cold_import] SHA3 spotcheck: K=5 ... 5 OKs
#   [cold_import] blk files: linked=101 copied=0 skipped=0 errors=0
#   [cold_import] block_index copy: ~3,111,000 entries in ~15000 ms
#   [cold_import] chainstate: ~1,300,000 UTXOs ... in ~15000 ms
#   [cold_import] coins best_block set to <hash>
#   [cold_import] DONE in ~30-60s

# Cleanup
until ! kill -0 $PID 2>/dev/null; do sleep 1; done
systemctl --user start zclassicd-rhett

# Verify: re-run zclassic23 on the test datadir as a foreground
# binary, query the tip
./zclassic23 -datadir=$HOME/.zclassic-c23-cold-test \
  -nobgvalidation -port=18033 -rpcport=18233 \
  > /tmp/cold-boot.log 2>&1 &
sleep 10
curl -s --data-binary '{"method":"getblockcount"}' \
  -u <rpcuser>:<rpcpass> http://127.0.0.1:18233/

# Expected: getblockcount returns the legacy tip
```

### Test 2: -fastimport (warm catch-up against production datadir)

```bash
systemctl --user stop zclassic23
systemctl --user stop zclassicd-rhett

./zclassic23 -datadir=$HOME/.zclassic-c23 \
  -fastimport=$HOME/.zclassic -nobgvalidation \
  -port=8033 -rpcport=18232 \
  > /tmp/fastimport.log 2>&1 &
PID=$!

# Watch for:
#   [legacy_direct_import] SHA3 spotcheck: K=3 ... 3 OKs
#   [legacy_direct_import] trust-mode armed (assume_valid=...)
#   [legacy_direct_import] applied=... rate=>=100 bps
#   [legacy_direct_import] wallet rescan complete: ... in ...

until ! kill -0 $PID 2>/dev/null; do sleep 1; done
systemctl --user start zclassicd-rhett
systemctl --user start zclassic23

# Verify
./tools/zcl-rpc getblockcount   # should match zclassicd tip
```

## What to look for / failure modes

- `[bilr] cannot open ...` — zclassicd LOCK held; stop it and wait 5s.
- `[cold_import] REFUSING: our active_tip=X > 1000` — not for warm
  catchup; use `-fastimport` instead.
- `spotcheck FAILED at window K` — anchor table doesn't match
  zclassicd's blocks. Either wrong chain (testnet?) or compile-time
  anchor is stale (Phase 4 rolling anchor refresh).
- `link() failed: Invalid cross-device link` — legacy and our datadir
  on different filesystems. Falls through to copy; expect +5-10s.
- DROP INDEX failure log — pre-existing issue, unrelated.

## What to do if the rate / throughput target isn't hit

- **fastimport <30 bps**: `g_body_pull_active` may not be engaging.
  Add a debug log in `connect_tip` at `process_block.c:2820` showing
  the flag value. Run `strace -c -e trace=fsync` to count fsyncs.
- **cold-import takes >2 min**: chainstate import is dominated by
  per-batch SQLite COMMIT fsyncs. Currently batches are 5000 records;
  could try 10K or 20K. Or wrap the whole import in
  `node.db_ibd_turbo_mode()` (synchronous=OFF).
- **block_index copy slow**: increase BATCH_LIMIT from 5000.

## Open issues / next-pass priorities

- **Phase 8: shallow-snapshot helper** — hardlink `.ldb` files
  (immutable in LevelDB) + cp MANIFEST + CURRENT + `.log` to a temp
  dir. Open the snapshot dir. Unblocks both fastimport and
  cold-import while zclassicd keeps running. ETA ~1-2 hr.
- **Phase 9: heartbeat auto-recovery** — when watchdog observes lag,
  escalate through P2P → loopback → fastimport → cold-import. ETA
  ~3-4 hr.
- **Phase 10: operator tooling** — `tools/zcl-resync-from-legacy.sh`
  one-shot wrapper that stops services, runs `-fastimport` (or
  `-cold-import` on empty datadir), restarts.
- **Phase 4: rolling anchor refresh** — run
  `gen_sha3_windows --max-height=$(getblockcount)` against zclassicd
  per release; commit the refreshed compile-time anchor.
- **Sapling tree warmup**: after cold-import the tree is empty; users
  who want immediate shielded ops must wait for bg_validation to
  reach Sapling-active heights (~10-30 min). Phase 11 could ship a
  compile-time Sapling tree blob (~30MB at anchor height) for an
  instant working shielded wallet.
- **bg_validation default-on**: in the production unit, `-nobgvalidation`
  is currently set. After cold-import we depend on bg_validation
  re-verifying every block; ensure it's ON in production.
- **node.db DROP INDEX during turbo enter** still fails
  (coins_view_sqlite holds the table lock); synchronous=OFF still
  engages, indexes stay on.

## Files touched

### Phase 3 (already merged)
- `app/services/{include/services,src}/legacy_direct_import.{h,c}` (NEW)
- `lib/storage/{include/storage,src}/blocks_index_legacy_reader.{h,c}` (NEW)
- `lib/storage/{include/storage,src}/blocks_mmap_reader.{h,c}` (NEW)
- `lib/validation/{include/validation,src}/process_block.{h,c}` — `g_body_pull_active`
- `app/services/{include/services,src}/chain_restore_service.{h,c}` —
  `chain_restore_clear_failed_above_tip`
- `config/include/config/boot.h`, `config/src/boot.c`, `main.c` — wiring

### Phase 7 (this session)
- `app/services/{include/services,src}/legacy_cold_import.{h,c}` (NEW)
- `config/include/config/boot.h`, `config/src/boot.c`, `main.c` —
  `-cold-import` flag + boot step

## Memory entries to consult

- `project_fast_sync_phase7_2026-05-14.md` — Phase 7 (this session)
- `project_fast_sync_phase3_2026-05-13.md` — Phase 3
- `feedback_fast_sync_phase1_findings.md` — bottleneck-shift lesson
- `feedback_at_tip_kill9_ordering_invariant.md` — preserve when
  batching commits
- `reference_zclassicd_local_fast_sync.md` — datadir layout
