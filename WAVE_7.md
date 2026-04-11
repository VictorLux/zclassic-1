# Wave 7 — Active Work Plan

**Status:** live. Agents work items in order, reach for stretch, pull from `BACKLOG.md` when the wave is clear.

**Coordinator note:** wave 6 carry-over is listed first per agent. Each agent has ~12 items plus stretch — deeper than waves 4–6 because agents have been outpacing plan refresh. When these clear, pull from `BACKLOG.md`.

**Previous wave:** `WAVE_6.md` (checkoffs still there for historical reference).

---

## AGENT2 — Wave 7 Priority Queue

### Wave 6 carry-over (in priority order)

- [ ] **PHGR13 sync stall investigation** — `lib/sapling/src/sprout.c`. Produce `PHGR13_INVESTIGATION.md` with: one rejected block replay through `verify_joinsplit()`, field-by-field diff vs `~/zclassic-cpp`, hypothesis, 5-line fix sketch. **Don't commit the fix.** This is the consensus bug blocking the node from reaching network tip.
- [ ] **Reorg safety test** — `lib/test/src/test_reorg_safety.c`. Synthesise a 50-block reorg with a conflicting fork, drive through `activate_best_chain`, assert no UTXO loss / no orphan rows / no `EV_CHAIN_TIP_REJECTED`. The CSR + recovery_policy + db_txn were built for exactly this test class — it still doesn't exist.
- [ ] **Script/sigcache parallelism** — audit `lib/validation/src/connect_block.c` script verification loop. If sequential, introduce `lib/util/workpool.{h,c}` and run sig checks across N threads. Measure `connect_tip` wall time on a recent mainnet block. Target: 2× speedup on 8-core.
- [ ] **IBD throttle** — `lib/validation/src/process_block.c` token bucket between block accept and `update_coins`. Env: `ZCL_IBD_BLOCKS_PER_SEC` (default 500). Prevents a firehose peer from starving the rest of the node during initial download.
- [ ] **Boot decomposition Phase A** — `app/services/block_index_loader.{h,c}`. AGENT1 has committed the boot.c safety gates; next boot.c session will wire `bii_verify` / `disk_monitor_start` / `wallet_backup_start` / `mempool_limits_start` and you can start extracting services in parallel. Coordinate via BOOT_QUEUE.md.
- [ ] **Boot decomposition Phase B** — `app/services/chain_state_validator.{h,c}`. Cross-check between block_map, SQLite blocks, coins_best, active_chain. Refuse-to-boot on mismatch.
- [ ] **Boot decomposition Phase C** — `app/services/utxo_recovery_service.{h,c}`. Wraps the wipe/restore decision in `recovery_policy` + `db_txn`. Target after A/B/C: `boot.c` < 1400 lines.

### New for wave 7

- [ ] **Fix fuzzer finding #2** — `test_json.c` segfaults under `-O1 + gcov` in the write+read roundtrip. Production builds unaffected. Run under valgrind, identify the root cause, file the fix in a separate commit from the discovery. This unblocks AGENT3's coverage builds from one warning.
- [ ] **Consensus parity audit** — pick 10 real mainnet blocks near the current tip, run them through `connect_block` in both `zclassic23` and `~/zclassic-cpp`, assert matching `coins_best_block` hashes after each. First cross-impl regression test. Write results to `CONSENSUS_PARITY.md`.
- [ ] **Block-time sanity hardening** — audit `lib/validation/src/contextual_check_tx.c` + `check_block.c` against BIP113 (median-time-past) and BIP65 (CLTV). Are the time comparisons wall-clock or MTP? Add tests for both paths with adversarial timestamps.
- [ ] **Mempool orphan handling** — `lib/validation/src/txmempool.c` currently drops any tx with a missing input. Add an orphan pool (max 50 txs, 10-min TTL) and attempt to reconnect orphans when their parent arrives. Tests in `test_mempool_orphan.c`.
- [ ] **Disk monitor integration** — now that `disk_monitor_start` is wired in boot (after AGENT1's next session), call `disk_monitor_is_critical()` in the mempool accept path + process_block write path + `wallet_backup_run_once`. Each integration point is one commit.
- [ ] **Consensus metrics** — new events `EV_CONSENSUS_REJECT_{BLOCK,TX}` with reason + height, wire into `check_block.c` / `check_transaction.c`. Let AGENT3 surface them in Prometheus in parallel.

### Stretch for wave 7

- [ ] **Block pruning service** — `app/services/block_pruning.{h,c}`. Keep last N blocks' raw data, discard older, keep block_index entries. Env: `ZCL_PRUNE_KEEP_BLOCKS` (default 0 = archival).
- [ ] **Drive CSR migration to zero** — audit remaining ~56 unmigrated call sites, each gets migrated or `/* CSR-internal: justified */` with reason.
- [ ] **Snapshot exporter automation** — auto-generate snapshots nightly when at-tip, rotate to keep last 7, publish via file service.

---

## AGENT3 — Wave 7 Priority Queue

### Wave 6 carry-over (in priority order)

- [ ] **Live wallet encryption integration** — wire `wks_encrypt`/`wks_decrypt` through `wallet_db.c` / `wallet.c` / `keystore.c` / `wallet_key.c` / `wallet_sqlite.c`. Primitives are in `lib/wallet/wallet_keystore.{h,c}` (wave 4 session 5). Per-file regression coverage on every controller that touches a key. Migration path: plaintext → encrypted in one big `db_txn` at first boot when `ZCL_WALLET_PASSPHRASE` is set. Coordinate with `wallet_backup_service` — encrypted wallets back up ciphertext blobs, never plaintext.
- [ ] **RPC timeout layer** — the `ZCL_RPC_TIMEOUT_MS` part of the wave 4 RPC middleware still isn't wired. Add a watchdog thread that kills HTTP connections past timeout, emit `EV_RPC_TIMEOUT` with method + elapsed_ms. 5+ tests.
- [ ] **WebSocket event stream** — `lib/net/ws_events.{h,c}` with `/events?domain=…` filter, per-client ring buffer, overflow frame, heartbeat, max 100 subscribers. Subscribe to the event bus via `event_subscribe()`.
- [ ] **OpenTelemetry-compat tracing** — `lib/util/trace.{h,c}` W3C Trace Context format, output via `log_json`. Migrate 5 hot paths: MCP dispatch, HTTP RPC dispatch, `connect_tip`, `csr_commit_tip`, `snapsync_begin_receive`.
- [ ] **Peer bandwidth quotas — wire into connman** — primitives landed in `8a5deed6f`. Now wire the token-bucket calls into `lib/net/src/connman.c` send/recv paths. Pause a starved peer, resume on refill. Emit `EV_PEER_THROTTLED` on starvation.
- [ ] **MCP TLS transport** — add optional TLS listener on a configurable port speaking the same JSON-RPC protocol. Env: `ZCL_MCP_TLS_PORT`, `ZCL_MCP_TLS_CERT`, `ZCL_MCP_TLS_KEY`. Reuse `lib/net/src/https_server.c`. Existing middleware still applies.
- [ ] **Alert routing** — `lib/util/alerts.{h,c}` — threshold rules fire on `EV_*` events, dispatch to webhook (env `ZCL_ALERT_WEBHOOK_URL`), email sink, log sink. Seed with the four rules from `zcl_admin.alerts`: disk_low, peer_bans_high, rpc_ratelimit_spike, chain_tip_rejected.
- [ ] **Chaos fault injection** — `tools/mcp/chaos.{h,c}` + `zcl_chaos_*` tools under `#ifdef ZCL_CHAOS`. Drop-N-peers, fail-next-sqlite-write, delay-csr-commit-by-ms. Used by AGENT2's reorg safety test.

### New for wave 7

- [ ] **Coverage trajectory: 26% → 35%** — the baseline is in place (`make coverage`). Audit which files are 0% and pick the highest-LOC uncovered files for targeted test writing. Target: +9% line coverage in one session. Post before/after in Current Status.
- [ ] **`zcl_consensus_report`** — new MCP tool that surfaces AGENT2's new `EV_CONSENSUS_REJECT_*` events as Prometheus counters + a per-reason histogram. Enables "why is this node rejecting blocks" debugging without grepping logs.
- [ ] **Grafana dashboard JSON** — ship `docs/grafana/zclassic23.json` — a canonical Grafana dashboard that pulls from the existing Prometheus `/metrics` endpoint. Panels for: chain height, peer count, UTXO count, mempool size, RPC RPS, CSR commits, recovery_policy decisions, fuzzer LOC coverage, disk free.
- [ ] **MCP request/response replay** — `tools/mcp/replay.{h,c}` — record the last N (default 100) MCP requests+responses in a ring buffer. New tool `zcl_replay_dump` returns the ring. New tool `zcl_replay_exec` replays a single recorded request against the live router. For regression and "what happened at 3am" forensics.
- [ ] **Operator runbook** — `docs/RUNBOOK.md` — cookbook for common operator scenarios: "node is at 99% disk", "peer is misbehaving", "backup failed", "tip regressed". Each entry: symptom → `zcl_*` tool to diagnose → command to fix. Read from agent memory files + `CLAUDE.md` for context.
- [ ] **Auto-generated MCP reference** — `docs/MCP_REFERENCE.md` generated from `zcl_tools_list` JSON at build time. Makefile target `docs-mcp` runs `./zclassic23 -mcp` with a synthetic tools/list request and formats the output as markdown. Replaces hand-maintained docs.

### Stretch for wave 7

- [ ] **gRPC alternative interface** — proto files derived from router metadata, minimal libgrpc server stub. Parallel to MCP/HTTP-RPC surfaces.
- [ ] **WebAuthn/passkey auth** — replace HTTP RPC cookie auth with passkey challenge-response. Off by default, experimental.
- [ ] **Continue tool backfill to 85+ RPC parity** — audit which RPC methods don't have MCP wrappers and wrap them.

---

## Coordination rules (unchanged from wave 6, reiterated)

1. **Plans live in `WAVE_N.md`.** AGENT1 writes wave plans here. Agents tick items off in the same commit that closes each item.
2. **Agent status logs in `AGENT*.md`.** AGENT1 doesn't edit those sections — agents own them.
3. **Pull from `BACKLOG.md`** when wave clears. 30+ unscheduled items across every area.
4. **BOOT_QUEUE.md** for boot.c edits. AGENT1 batches queued items per wave.
5. **FUZZER_FINDINGS.md** for bug discoveries. Separate discovery commit from repair commit.
6. **Rebuild `zclassic23` before every `./test_zcl`.** `make zclassic23 test_zcl` is canonical.
7. **Smoke-test `zcl_self_test` after every push**, paste result in Current Status.
8. **Reach down for stretch** in the same session if the priority list clears.
9. **Trade work** — if one agent finishes while the other is grinding, take test-file or docs work from the peer's list (still respecting territory).

---

## Territory (unchanged)

**AGENT2 owns:** `config/src/boot.c` (edits via BOOT_QUEUE.md only — AGENT1 batches), `app/services/chain_state_repository.*`, `recovery_policy.*`, `db_txn.*`, `block_index_integrity.*`, `wallet_backup_service.*`, `disk_monitor.*`, `db_maintenance.*`, `mempool_limits.*`, `addrman_integrity.*`, `block_index_loader.*` (new), `chain_state_validator.*` (new), `utxo_recovery_service.*` (new), `block_pruning.*` (stretch), `tools/fuzz/**`, `tools/crash_recovery_test.c`, `lib/sapling/src/sprout.c` (PHGR13).

**AGENT3 owns:** `tools/mcp/**`, `lib/net/peer_scoring.*`, `lib/net/peer_bandwidth.*`, `lib/net/ws_events.*` (new), `lib/wallet/wallet_encrypted.*`, `lib/wallet/wallet_keystore.*` + live integration, `lib/rpc/http_middleware.*` + timeout layer, `lib/util/log_json.*`, `lib/util/trace.*` (new), `lib/util/alerts.*` (new), `app/models/database_validators.*`, all `lib/test/src/test_mcp_*.c`, all `lib/test/src/test_peer_*.c`, all `lib/test/src/test_rpc_*.c`, all `lib/test/src/test_wallet_*.c` tests for encryption, `docs/RUNBOOK.md`, `docs/MCP_REFERENCE.md`, `docs/grafana/*`.

**AGENT1 owns:** `config/src/boot.c` direct edits, PR review + merge conflict resolution, PHGR13 if AGENT2 hands it back after investigation, wave plan refresh.
