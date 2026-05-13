# Next-session handoff — body-pull operational, performance + auto-recovery left

**Last session ended 2026-05-13 ~22:00.**

## State at handoff

### Running node
- Latest binary deployed via systemd unit (auto-picks up the build at
  `/home/rhett/github/zclassic23/zclassic23` via the `~/zclassic23`
  symlink).
- Active tip: ~**3,102,778** (live e2e test advanced 2,199 blocks from
  3,100,515 before SIGTERM landed cleanly).
- zclassicd tip: **3,111,137+**. Remaining gap: ~8,400 blocks.
- The four production fixes from this and last session — K2 loopback
  fast lane, P1 connman loopback exemption, P0 body-pull, P0.7
  standalone entry point — are all live in the running binary.

### What works
- `./zclassic23 -datadir=$HOME/.zclassic-c23 -bodypull-from-legacy` is
  the unsticker. Skips local_chain_ingest phase 1 (SHA3 verify) and
  phase 2 (chainstate import), pulls headers via header_probe, then
  height-iterates over `[active_tip+1 .. remote_tip]` calling
  `process_new_block` per block. Each block is durable on disk.
- Live test result: **applied=2199 skipped_have=63 rpc_errors=0** —
  no orphan accumulation, no validation failures, no RPC errors.
  Active tip extended from 3,100,515 to ~3,102,778.

### Commits shipped this session (all on origin/main)
```
42c014806 fast-sync: standalone -bodypull-from-legacy + height-based traversal
021a41da4 fast-sync: P0 durable body-pull + P1 loopback connect-timeout
```

## Immediate priorities (next session)

### P0.9 — Per-block performance (~3-6 hr)

Per-block rate during the live test was **~3 blocks/sec**, far below
the handoff doc's "3-5 min for 10 K-block gap" target. At this rate
the full catch-up takes ~50 minutes per restart. Suspect path:

1. `process_new_block` → `connect_tip` runs full check_inputs +
   sapling verify on every block. With `-nobgvalidation` set, the
   live tip path shouldn't be re-verifying historical Groth16
   proofs, but check_inputs (ECDSA per input) is unavoidable on the
   first connect.
2. Sapling tree warm-up: boot logs say
   `Sapling tree root MISMATCH (size=714331) - deferring live
   rebuild until after boot (tip_h=3100430)` — the first
   connect_tip may rebuild the entire incremental Merkle tree from
   a 714k-commitment cold state. One-time cost but expensive.
3. `process_new_block` triggers
   `activation_request_connect(ACTIVATION_SRC_NEW_BLOCK)` — find
   out whether the activation controller is serial (one block per
   queued request) and whether each request does its own coins
   commit + node_db commit (the chain_advance per-block latency
   floor).

Where to look:
- `app/services/src/chain_advance.c` — the atomic per-block
  commit. Look at the I/O footprint per call.
- `lib/validation/src/process_block.c:4137-4182` `process_new_block`
  — order of check/accept/activate.
- `app/services/src/chain_activation_controller.c` — whether
  there's a batching path or only one-at-a-time.

Possible quick wins:
- Defer coins.db `BEGIN IMMEDIATE` / fsync to batches of N blocks
  during body-pull. The at-tip kill-9 ordering invariant
  ([[feedback_at_tip_kill9_ordering_invariant]]) still has to
  hold, so consider only batching up to the second-to-last block.
- Skip script signature verification during catch-up when caller
  is body-pull (consenting trust of the legacy node — same trust
  model that local_chain_ingest already declares via
  `cfg.skip_pow_verify = true`).

### P0.5 — clear spurious BLOCK_FAILED_VALID (~30 min, still pending)

The original handoff doc called for this. Still relevant: if some
blocks in the catch-up window carry stale `BLOCK_FAILED_VALID` from
prior runs, body-pull silently skips them
(`legacy_body_pull.c:235` `skipped_failed++`). Pattern to copy is
`chain_restore_service.c:648` `invalidated_off_chain` counter.

Trigger condition: when `active_tip < pindex_best_header` by ≥ N
(or `active_tip < known_remote_tip` from oracle), clear
`BLOCK_FAILED_VALID` from entries with `nHeight > active_tip` so
the next body-pull / activate_best_chain can re-validate them.

### P0.10 — Heartbeat-triggered body-pull (~2-3 hr)

So the operator never has to run `-bodypull-from-legacy` manually.
A heartbeat tick in the existing health subsystem checks:
- `active_tip < remote_tip - N` (configurable threshold)
- zclassicd reachable on loopback
- last automatic pull ≥ M seconds ago

…and if all true, calls `legacy_body_pull_range_blocking` for a
bounded window. Combine with P0.5 and the node self-heals from
any tip-lag condition.

### P0.6 — `tools/zcl-resync-from-legacy.sh` wrapper (still pending)

Operator one-liner: stop service → run
`./zclassic23 -bodypull-from-legacy=...` → wait for completion →
restart service. Defer until per-block performance is fixed; today
the foreground binary leaves a long-running process the operator
has to watch.

## Lessons learned this session

1. **pindex_best_header is not the header tip.** `accept_block_header`
   creates block_index entries but does **not** promote
   `pindex_best_header`. Only `csr_commit_tip` (called when a block
   is connected as the new active tip) updates it
   (`lib/validation/src/process_block.c:1457`). For any height-based
   traversal that "knows" the remote tip, prefer the value returned
   by `header_probe_pull_range_blocking(out_remote_tip)`.

2. **Phase 1 SHA3 anchors are out of sync with the legacy datadir.**
   `local_chain_ingest_run` aborts at "window 0 mismatch" before
   reaching phase 3 — that's why the original `-importfromlegacy`
   flow couldn't unstick. The standalone `-bodypull-from-legacy`
   sidesteps it entirely. Re-regenerating `g_sha3_windows[]` via
   `tools/gen_sha3_windows` may be worth doing for completeness,
   but is no longer load-bearing for the unstick path.

3. **Foreground binary teardown is slow.** SIGTERM → "shutdown
   watchdog: 25s timeout — forcing exit" via SIGALRM. While
   teardown is in flight, the systemd service can't claim the
   datadir lock and fast-fails. Always wait for the foreground PID
   to actually exit before issuing `systemctl restart`.

## Files of interest

- `app/services/src/legacy_body_pull.c` — height-based pull, the
  heart of this session.
- `lib/rpc/src/legacy_rpc_client.c` — shared JSON-RPC transport.
- `config/src/boot.c:646-728` — `boot_step_bodypull_from_legacy`
  hook that runs the standalone path.
- `app/services/src/local_chain_ingest.c:1473-1532` — phase3-pre
  body-pull wired through the import path (still useful once
  phase 1/2 anchors are refreshed).

## Diagnostic check sequence

```
# 1. Is body-pull working?
grep "legacy_body_pull\] applied" ~/.zclassic-c23/node.log | tail
# 2. What's the active tip vs zclassicd?
mcp__zcl23__zcl_status         # local
zclassic-cli getblockcount     # legacy (via ~/.zclassic conf)
# 3. Sapling tree status (perf diag)
grep -E "Sapling tree.*MISMATCH|rebuild" ~/.zclassic-c23/node.log | tail
```
