# Next-session handoff — Phase 1 done (modest gain), real bottleneck identified

**Last session ended 2026-05-13 ~23:10.** Plan file:
`~/.claude/plans/look-i-need-you-velvet-mist.md`.

## What shipped (3 new commits on origin/main)

```
3c84e4f0c fast-sync: wrap body-pull in node.db IBD turbo mode
?         fast-sync: SHA3 spotcheck + assume_valid trust-mode for body-pull  (021a41da4-chain)
60b317794 session-end: handoff doc post P0.7/P0.8
42c014806 fast-sync: standalone -bodypull-from-legacy + height-based traversal
021a41da4 fast-sync: P0 durable body-pull + P1 loopback connect-timeout
```

The Phase 1 plan (`look-i-need-you-velvet-mist.md`) targeted **100+
blocks/sec** body-pull. Live test result: **~4 blocks/sec sustained**,
vs 3 bps baseline. ~30% improvement, not 30x. Mechanism is correct
(SHA3 spotcheck passes, trust-mode armed, IBD turbo engaged), but
the bottleneck has shifted.

## Where the time actually goes (NEW finding)

`legacy_body_pull` calls `process_new_block` → `activation_request_connect` →
`activate_best_chain` → `connect_tip` (NOT `chain_advance`, which is
only used by `local_chain_ingest.c` phase 3). The per-block hot path
in `connect_tip` (`lib/validation/src/process_block.c:2166+`) does:

1. **`block_tree_db_write_block_index_sync`** — LevelDB sync write per block (~10-30ms on SSD).
2. **`block_tree_db_write_tx_index`** — per-tx LevelDB writes when
   `-txindex` is on (production unit has it).
3. **`wallet_sync_transaction`** — per-tx trial-decryption of Sapling
   outputs (BLS12-381 ops).
4. **Sapling commitment tree updates** — Pedersen hashes per output.
   NOT gated by `g_assume_valid_height`.

The trust-mode work (1a) skips ECDSA, Groth16, JoinSplit Ed25519,
BIP-30 — but those are tiny compared to the I/O + wallet sync above.

Also: `node_db_ibd_turbo_mode`'s DROP INDEX *failed* (table locked by
the coins_view_sqlite dedicated connection), so node.db indexes stayed
on. synchronous=OFF succeeded but the savings are smaller without the
index drops.

## Secondary finding: activate_best_chain stalls on stale failed flags

Live: `applied=856 ok=no final_tip=3,100,643` despite 856 blocks
written to disk. The active tip did NOT advance.

Hypothesis: stale `BLOCK_FAILED_VALID` flags upstream in the chain
prevent `find_most_work_chain` from selecting the path through the
body-pulled blocks. Body-pull writes them to disk, but
`activate_best_chain` refuses to extend through invalid markers.
This is exactly the P0.5 task from the original handoff doc, never
shipped.

## Next-session priorities

### P1-NEW: Clear stale BLOCK_FAILED_VALID on boot (was P0.5, now load-bearing)

**File:** `app/services/src/chain_restore_service.c` (around line 648,
the `invalidated_off_chain` counter pattern).

When the tip is detected as stuck (active_tip < pindex_best_header by
≥ N for ≥ M seconds), boot-time policy clears `BLOCK_FAILED_VALID`
from any block whose `nHeight > active_tip`. This unblocks
`find_most_work_chain` from selecting paths through those blocks; the
next `connect_tip` re-validates them (cheap under assume_valid trust
mode) — if they're spuriously-flagged they now connect; if they're
genuinely invalid they get re-flagged.

Without this, Phase 1's body-pull writes blocks to disk that never
connect. ETA: ~30-60 min.

### P2-NEW: Defer LevelDB block_index_sync during body-pull

The dominant per-block cost. `connect_tip` line ~2809 hard-codes
`block_tree_db_write_block_index_sync`. Replace with a body-pull-aware
buffered writer: accumulate `disk_block_index` entries in memory,
flush + fsync the whole batch at the end of body-pull (or every N
blocks for crash safety). One fsync amortized over thousands of
writes.

Combined with P1-NEW, this should plausibly hit the 100+ bps target.
ETA: ~2-3 hr.

### P3-NEW: Defer tx_index + wallet sync during body-pull

`-txindex` writes one LevelDB key per transaction. With ~250 txs/block
average this is the secondary bottleneck. Defer to end-of-body-pull
batch.

`wallet_sync_transaction` does Sapling trial-decryption. During
body-pull-trust-mode, skip wallet sync entirely; user runs
`rescanblockchain` afterward to populate the wallet at leisure.

ETA: ~1-2 hr each.

### Deferred from Phase 1

- **1b (batched chain_advance commits):** the existing chain_advance
  isn't on the body-pull hot path (see above). DELETE this idea;
  the equivalent for body-pull is P2/P3-NEW above.

## What's still broken / open issues

- **node.db DROP INDEX fails during turbo enter** — locked by
  coins_view_sqlite dedicated connection. Either drop the indexes
  before opening the dedicated coins connection, or accept reduced
  turbo benefit.
- **active_chain stuck at 3,100,643** despite body-pull writing 856
  blocks to disk. P1-NEW unblocks this.
- **Phase 1 SHA3 window 0 mismatch** still unresolved (`-importfromlegacy`
  can't reach phase 3). Documented in plan as Phase 2.
- **Rolling anchor regeneration** still not built. Plan Phase 3.

## Verification commands

```bash
# After P1-NEW lands:
systemctl --user stop zclassic23
./zclassic23 -datadir=$HOME/.zclassic-c23 \
  -bodypull-from-legacy -nobgvalidation \
  -port=8033 -rpcport=18232 \
  > /tmp/fast-sync.log 2>&1 &

# Should see:
#   [chain-restore] cleared N stale BLOCK_FAILED_VALID flags above tip ...
#   [legacy_body_pull] SHA3 spotcheck: K=3 ... 3/3 windows match
#   [legacy_body_pull] trust-mode armed: assume_valid X -> Y
#   Body pull: node.db turbo mode ON
#   [legacy_body_pull] applied=N h=H at increasing rate
#   Body pull: applied=10500 ok=yes final_tip=3,111,198  (← key: final_tip moves!)

# After P2/P3-NEW lands:
#   Rate should sustain ≥100 blocks/sec
#   Total catch-up of 10K-block gap should be < 2 min
```

## Files touched in Phase 1

- `app/services/src/legacy_body_pull.c` — SHA3 spotcheck + assume_valid bump (committed)
- `config/src/boot.c` — wrap body-pull in IBD turbo enter/exit (committed)

## Memory entries to consult next session

- `feedback_fast_sync_phase1_findings.md` — full bottleneck-shift analysis
- `feedback_at_tip_kill9_ordering_invariant.md` — preserve when batching commits
- `reference_zclassicd_local_fast_sync.md` — datadir layout
