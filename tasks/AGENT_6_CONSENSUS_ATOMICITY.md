# Agent 6 — Consensus / Validation / Sync Atomicity

**Read first:** [`HARDENING_CHECKLIST.md`](../HARDENING_CHECKLIST.md) §P2.4–P2.7, §R2.7.

**Worktree:** `~/zclassic23-6`
**Branch:** `a6/consensus-atomicity`
**Base:** `origin/master`
**Dependencies:** none (but please read Agent 5's task so you understand the overlapping "never silently wipe" philosophy).

---

## Mission, one sentence

Close every window where a crash between UTXO writes and tip update can leave the node with an inconsistent view of consensus; bound mempool growth on adversarial input; add CRC wrapping to block files.

---

## Scope

### Files you own

- `lib/validation/src/process_block.c` — ConnectBlock flush → tip ordering
- `app/services/src/snapshot_sync_service.c` — SHA3 verify ordering, CSR barrier
- `lib/validation/src/txmempool.c` — ancestor/descendant caps
- `lib/storage/src/disk_block_io.c` — block-file CRC wrapping
- `lib/test/src/test_connectblock_flush_tip.c` **(create)**
- `lib/test/src/test_snapshot_stream_verify.c` **(create)**
- `lib/test/src/test_mempool_ancestor_caps.c` **(create)**
- `lib/test/src/test_block_file_crc.c` **(create)**

### Files you MUST NOT touch

- `lib/wallet/`, `app/models/src/wallet*` (agents 2/3)
- `Makefile`, systemd units (agent 4)
- `app/models/src/database.c`, `lib/coins/src/utxo_commitment.c`, `lib/storage/src/coins_view_sqlite.c`, `lib/storage/src/dbwrapper.c` (agent 5)
- `lib/net/`, `tools/*` (agent 7)
- `lib/sapling/` (agent 8)

---

## Deliverables

### D1. ConnectBlock flush → tip atomicity (P2.4)

`lib/validation/src/process_block.c:1167-1233` — `coins_view_cache_flush` at line 1167, `update_tip` at line 1233. On flush failure flow currently still reaches `update_tip`. Fix:

```c
if (!coins_view_cache_flush(&view)) {
    LOG_FAIL("validation", "flush failed at height %d; halting", block_height);
    /* Do NOT advance tip. */
    node_state_halt(NODE_HALT_COINS_FLUSH_FAIL, block_hash);
    return false;
}
```

`node_state_halt` is the same STATE_D-style abort plumbing Agent 3 is adding for the wallet. If it is not on `origin/master` by the time you start, stub a minimal `node_state_halt(enum, const char*)` that `fprintf`s and `exit(2)`s; leave a TODO for the real halt-signal integration.

**Regression test** (`test_connectblock_flush_tip.c`): inject a flush failure via test-only hook, assert tip did not advance, assert node halted (caught via `setjmp`/`longjmp` test shim).

### D2. Snapshot SHA3 verify BEFORE chunk commit (P2.5)

`app/services/src/snapshot_sync_service.c:454-551` currently writes chunks, then verifies SHA3, then (on mismatch) invokes a wipe policy. Change to streaming:

1. Maintain a running SHA3-256 state while chunks arrive.
2. Write each chunk to `utxos_staging` table (not `utxos`). Staging is a sibling with identical schema; create it on first use of snapshot sync.
3. Only after final chunk, finalize SHA3. If it matches the committed root: `INSERT INTO utxos SELECT * FROM utxos_staging; DELETE FROM utxos_staging;` inside one transaction.
4. On mismatch: `DELETE FROM utxos_staging;`, log, return failure. Live `utxos` is untouched throughout.

No wipe policy required — `utxos` never held unverified data.

**Regression test** (`test_snapshot_stream_verify.c`): feed a snapshot with a bit-flipped final chunk; assert live `utxos` is unchanged and staging is emptied.

### D3. `snapsync_commit_tip` through CSR atomic barrier (P2.6)

`snapshot_sync_service.c:474-492` and line 1401 — snapshot commits `coins_best_block` + tip outside the CSR (coins-state-record) atomic barrier that `process_block_commit_tip()` uses. Route it through the same barrier. This is the exact bug class that caused the 2026-04-10 UTXO wipe.

If CSR API does not expose a snapshot path, add one: `csr_commit_snapshot_tip(chain_state*, block_hash, utxo_count)` that writes the new tip and the UTXO state hash atomically.

### D4. Mempool ancestor/descendant caps (P2.7)

`lib/validation/src/txmempool.c:228-232` — add Bitcoin-Core-style caps:

- `MEMPOOL_MAX_ANCESTORS = 25`
- `MEMPOOL_MAX_DESCENDANTS = 25`
- `MEMPOOL_MAX_ANCESTOR_SIZE_KB = 101`
- `MEMPOOL_MAX_DESCENDANT_SIZE_KB = 101`

Compute ancestor/descendant packages on `accept_to_mempool`; reject with reason `TOO_LONG_MEMPOOL_CHAIN` if any cap exceeded. Expose configurable via a `-limitancestorcount=` family of flags (default to the constants above).

**Regression test** (`test_mempool_ancestor_caps.c`): build a 30-deep chain of 250-byte zero-fee txs; assert the 26th is rejected.

### D5. Block-file CRC wrapping (R2.7)

`lib/storage/src/disk_block_io.c:181-213` currently writes `[magic(4)][size(4)][data]` then fdatasyncs. Change to `[magic(4)][size(4)][crc32(4)][data]`. Read path verifies CRC; mismatch returns `false` with a log, caller treats as corrupt block.

Backwards compatibility: existing blk*.dat files on disk lack the CRC. Strategy:

- On first read attempt, detect header shape: if the 4 bytes after `size` don't CRC-match the payload, fall back to the old "no CRC" layout and log a one-time `[storage] legacy block-file record at offset N; CRC not yet added`.
- New writes always include CRC.
- Expose a `tools/rewrite_block_files` migrator that rewrites legacy files with CRC in place (optional; P3 follow-up).

**Regression test** (`test_block_file_crc.c`): write a block, flip one data byte, read back; assert read returns false and logs. Also test legacy-format read path produces the expected block.

---

## Done when

- [ ] ConnectBlock flush failure halts the node; tip never advances.
- [ ] Snapshot sync stages into `utxos_staging` and only promotes on SHA3 match.
- [ ] `snapsync_commit_tip` goes through the CSR barrier.
- [ ] Mempool rejects 26-deep chains with `TOO_LONG_MEMPOOL_CHAIN`.
- [ ] Block writes include a CRC32; corrupted reads fail loud.
- [ ] 4 new tests pass under `./test_zcl`.
- [ ] `make lint` and `make ci` green.
- [ ] PR title: `a6: consensus atomicity — flush/tip, snapshot staging, mempool caps, block CRC`

---

## Gotchas

- `process_block.c:1167-1233` is in the hot path of every block. Your halt-on-flush-failure change MUST preserve the happy-path code exactly — add the check, don't restructure.
- Snapshot staging table (`utxos_staging`) adds a schema migration. Coordinate with Agent 5: register it as schema version 7 (or whatever the next number is after A5 lands). Don't use numbers below what A5 is consuming.
- The CSR barrier lives in `lib/storage/src/chain_state_record.c` (or similar — grep `csr_commit_tip`). Adding a snapshot variant must keep the same fsync discipline. Don't add a new in-memory-only tip path.
- Mempool ancestor math is O(k * depth) in the naive implementation. Bitcoin Core caches ancestor counts on insert. For the first implementation, O(k*depth) is acceptable because the cap is 25. Don't over-engineer; just don't enable unbounded scans.
- Block CRC must cover size + data, not just data, or a truncated file parses as a smaller block.

---

## Hand-off

```
cd ~/zclassic23-6
git push origin a6/consensus-atomicity
gh pr create --title "a6: consensus atomicity — flush/tip, snapshot staging, mempool caps, block CRC" \
             --body "$(cat <<'EOF'
## Summary
Implements HARDENING_CHECKLIST.md §P2.4-P2.7, §R2.7.

- ConnectBlock halts on coins flush failure before tip update
- Snapshot sync stages into utxos_staging; promotes only on SHA3 match
- snapsync_commit_tip routes through CSR atomic barrier
- Mempool 25-ancestor / 25-descendant caps
- Block files now record + verify CRC32

## Plan
See HARDENING_CHECKLIST.md §P2.4-P2.7, §R2.7.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```
