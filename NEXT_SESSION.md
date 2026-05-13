# Next-session handoff — fast-sync continuation

**Last session ended 2026-05-13 ~18:55.**
Plan file: `~/.claude/plans/look-i-need-you-hazy-lemur.md`.

## State at handoff

### Running node
- **zclassic23 PID is the post-deploy binary** (`make deploy` ran at 18:47, `systemctl --user restart zclassic23` succeeded, `deploy_verify.sh` returned `Deployed + RPC live at block 3099373`).
- **Node tip is STUCK at h=3,099,501.** zclassicd is at h=3,111,011 → **lag = 11,510** and not closing.
- Watchdog reports `SLOW_PROGRESS: 0.26 blocks/sec`.

### Why it's stuck (from `~/.zclassic-c23/node.log`)
```
[gap-fill] queued 1 blocks (window [3099502..3099853] tip=3099501 best=3099853)
activate_best_chain: near-tip block h=3099603 was not a direct extension of tip=3099501; falling through to most-work reorg selection
activation: connecting->ready (behind_peers)
```
A block in the 3,099,502 prefix is missing; gap-fill queues but never delivers it. We have h=3,099,603..3,099,853 sitting as orphans, blocked by the missing prefix. Reorg-selection refuses to bridge.

### Loopback peer never connects
`127.0.0.1:8034` (zclassicd) shows `state=connecting` and never finishes handshake. Whole point of K1 (loopback fast lane) is lost while this peer isn't `active`. This is the most-likely root cause of the gap-fill stall: zclassicd is the only peer that has the missing block 3,099,502, but we can't ask it.

## Unpushed commits on `main` (4)
```
48e036501 fast-sync: LDB snapshot trick, embedded UTXO sidecar, loopback fast lane
4a7445851 bench: surface new subsystem state dumps in cold-start benchmark
c282b31d8 fast-sync: JSON-RPC batching + mmap phase-1 reads
d37a426e4 fast-sync: oracle policy + rolling anchor + LevelDB pre-pop scaffolding
```

## Immediate priorities (next session)

### P0 — diagnose loopback handshake failure
`127.0.0.1:8034` stays in `connecting` after the post-deploy restart. Possible causes:
1. **Version mismatch** — zclassicd's P2P protocol version may not match what `msg_version.c` accepts. Inspect `lib/net/src/msg_version.c handle_version()` flow vs zclassicd's getnetworkinfo `protocolversion`.
2. **Self-loop guard** — we set `-externalip=205.209.104.118` and `-port=8033`; zclassicd uses port 8034. If our addr_local equals the peer's claimed addr we may be rejecting as self.
3. **Inbound nonce collision** — both nodes generate `nonce` for ping-of-self detection.

Probe: `./tools/zcl-rpc node_log "127.0.0.1:8034" 300 200` (server-side regex tail). Look for VERACK ↔ VERSION exchanges and any disconnect reason.

### P0 — unstick the tip
If loopback fix doesn't free the gap on its own, fetch h=3,099,502..3,099,602 from zclassicd via RPC + insert directly:
```
./tools/zcl-rpc node_log "gap-fill" 60 50
./tools/zcl-rpc --repair 2000 8232 zclrhett:zclrhettpass2026
```
The `--repair` mode in `main.c` fetches missing UTXOs via zclassicd RPC. May need a similar `--repair-blocks` if the gap is in block-data not utxo state.

### P1 — pre-existing lint cleanup
`make deploy` fails on pre-existing fprintf observability-pairing violations in `lib/storage/src/coins_view_sqlite.c` (commit `df0f929fce`, lines 171, 179, 186, 267, 293, 320, 335, 344, 416, 706, 731). Add `// obs-ok:reason-without-spaces` (note: tag must have no space after colon — that's the lint quirk that bit us). Surgical, one-commit task.

### P1 — push branch
`git push origin main` — branch is 4 commits ahead, unpushed.

### P2 — K2: loopback fast lane for block-body `getdata`
K1 (shipped in `48e036501`) handles headers only. Once loopback peer connects, block bodies still queue against the same throttle. Mirror the `peer_is_loopback` short-circuit into:
- `app/services/src/block_sync_service.c` — bump `MAX_BLOCKS_IN_TRANSIT` for loopback peers
- `lib/net/src/msgprocessor.c` — `handle_getdata`/`handle_block` paths
Expected: closes 12K-block lag in **minutes** instead of the ~2 hours current rate.

### P2 — bench the Round-3 work
Run `tools/bench_cold_start_from_legacy.sh` with the deployed binary and clean datadir to measure phase-1 mmap + phase-2 sidecar (if present) wins. Compare to pre-Round-3 baseline.

To exercise J3 fast path: generate the sidecar once, place at `~/.zclassic-c23/utxo_snapshot.dat`, and rerun cold-start. SHA3 only matches the compile-time anchor when generated against a zclassicd at exactly h=3,056,758, so for a live diagnostic test the sidecar SHA3 won't match — code falls back to chainstate iter cleanly.

## Deferred (out of scope this session, plan still tracks)
- **A2** — 4-way SHA3-256 AVX-512 absorber (~6 hr crypto work; phase-1 CPU win)
- **D1/D2** — io_uring phase 1 (needs `sudo apt install liburing-dev`)
- **E2** — P2P quorum source (depends on K1 in prod; loopback peer must actually connect first)
- **T2.3** — tip-zone shadow validate (semantics unresolved)
- **N6** — per-block SHA3 tip-zone (finer granularity than 1000-block windows)
- **N7** — `-trust-mode=pure|hybrid|full` CLI flag

## What was shipped this session
| Commit | Stage | Files |
|---|---|---|
| `48e036501` | I (LDB snapshot), J (embedded UTXO + sidecar), B1 (bulk INSERT), K1 (loopback fast lane) | 14 files, +1,406 lines |

Tests added: `test_ldb_snapshot` (live snapshot of running zclassicd LDB), `test_utxo_snapshot_loader` (round-trip + corruption detection). All pass.

Subcommand added: `zclassic23 --gen-utxo-snapshot <legacy_datadir> <out_sidecar>` — validated against live zclassicd: 501,513 records / 1,344,705 vouts / 104 MB output.

## Cruft purged at handoff
- `/tmp/test_ldb_snapshot` (410 KB build-time tool binary — superseded by `lib/test/src/test_ldb_snapshot.c`)
- `/tmp/utxo_snapshot_test.dat` (104 MB sidecar test output)
- `./tools/gen_sha3_windows` (stale build artifact)

## Files of interest
- `lib/storage/src/ldb_snapshot.c` — the unblock that made Stage C viable
- `lib/chain/src/utxo_snapshot_loader.c` — runtime mmap+verify+iter
- `main.c` `gen_utxo_snapshot_mode()` — build-time sidecar emitter
- `app/services/src/local_chain_ingest.c` `phase2_chainstate_import` — fast path before chainstate iter
- `app/services/src/header_sync_service.c` `syncsvc_getheaders_interval` — K1 loopback fast lane
- `config/src/boot.c` near line 1488 — snapshot-based LDB read (replaces dangerous `unlink(LOCK)`)
- `lib/storage/src/coins_view_sqlite.c` `coins_view_sqlite_bulk_insert` — B1 helper
