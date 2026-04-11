# Wave 8 — Active Work Plan

**Status:** live. Short, dense, action-focused.
**Previous:** `WAVE_7.md` (carry-over items folded here).
**Coordinator:** AGENT1 — boot.c wire-up debt + PHGR13 fix review.

---

## AGENT1 self-assignment (not agent work)

These are mine. Listing here for transparency so AGENT2/AGENT3 can see what's coming and plan around it.

- [ ] Read `PHGR13_INVESTIGATION.md` end-to-end, validate both bug hypotheses against `~/.zcash-params/sprout-verifying.key` and `~/zclassic-cpp/src/zcash/Proof.cpp`, then either apply the fix myself or hand it back to AGENT2 with a green light.
- [ ] Batch-apply remaining BOOT_QUEUE items: `bii_verify`, `wallet_backup_start`, `disk_monitor_start`, `mempool_limits_start`, `ibd_throttle_start`. Single session, single commit.
- [ ] Process `REVIEW_QUEUE.md` at the start of each coordination pass (new artifact — see rules below).

---

## AGENT2 — Wave 8 Priority Queue

### Carry-over from wave 7

- [ ] **Reorg safety test** — `lib/test/src/test_reorg_safety.c`. Synthesise 50-block reorg with conflicting fork, assert CSR + recovery_policy + db_txn hold under `activate_best_chain`.
- [ ] **Script/sigcache parallelism** — `lib/util/workpool.{h,c}` + parallelise `connect_block.c` script verification loop. Target: 2× speedup on 8-core.
- [ ] **Boot decomposition Phase A/B/C** — after AGENT1's next boot.c session lands, extract `block_index_loader.{h,c}`, `chain_state_validator.{h,c}`, `utxo_recovery_service.{h,c}`. One commit per phase. Target: `boot.c` < 1400 lines.
- [ ] **Consensus parity audit** — pick 10 mainnet blocks, run through both `zclassic23` and `zclassic-cpp`, assert matching `coins_best_block` hashes. Write `CONSENSUS_PARITY.md`.
- [ ] **BIP113/BIP65 time hardening** — audit `contextual_check_tx.c` + `check_block.c` against MTP semantics, add adversarial-timestamp tests.
- [ ] **Mempool orphan pool** — max 50 txs, 10-min TTL, reconnect on parent arrival.
- [ ] **Disk monitor integration calls** — once `disk_monitor_start` is wired in boot, call `disk_monitor_is_critical()` from mempool accept / process_block write / wallet_backup_run_once.
- [ ] **Fix fuzzer finding #2** — `test_json.c` segfaults under `-O1 + gcov`. Run under valgrind, fix in a separate commit from discovery.

### New for wave 8

- [ ] **PHGR13 fix implementation — you may commit it.** You already did the investigation (`PHGR13_INVESTIGATION.md`). Waiting for AGENT1's review is the wrong bottleneck. New rule: **you may commit the PHGR13 fix without pre-review if and only if**:
  1. Both bugs in the investigation doc are addressed in the same commit
  2. A new test at `lib/test/src/test_phgr13_fix.c` loads one real joinsplit from a historical mainnet block and asserts `sprout_verify_phgr13` returns `true`
  3. `./test_zcl` green
  4. The live node, restarted after the fix, successfully connects at least one block beyond h=2,014,948 (verify with `zcl_getblockcount` before and after)
  5. Commit message includes `(PHGR13 fix — see PHGR13_INVESTIGATION.md)` in the subject
  If criterion #4 fails, revert, log findings in `FUZZER_FINDINGS.md`, and flag for AGENT1 review. Do NOT push a fix that doesn't let the node advance.
- [ ] **Block-level consensus parity smoke test** — new `tools/consensus_parity.c` CLI that takes a block hash and two node datadirs (ours + zclassicd's), reads the block via RPC from each, and compares `coins_best_block` after connection. First step toward a continuous parity check.
- [ ] **Chain rollback stress test** — `lib/test/src/test_chain_rollback.c`. Walk the chain backwards 100 blocks via `disconnect_block`, verify UTXO commitment at each step matches the commitment recorded in `blocks` table at that height. Exposes any undo-data leak.
- [ ] **`zcl_explain_reject`** — new *diagnostic* tool that takes a block hash or txid and returns the rejection reason from the new `EV_CONSENSUS_REJECT_*` events. Uses the ring buffer AGENT2 just added. Coordinate with AGENT3 for the MCP tool wrapper (AGENT3 owns `tools/mcp/**`), but the event-indexing logic lives in `app/services/consensus_reject_index.{h,c}` which is yours.

### Stretch

- [ ] **Drive CSR migration to zero** — audit remaining ~56 sites, migrate each or comment-justify.
- [ ] **Snapshot automation** — nightly snapshot if at-tip, rotate last 7, publish via file service.
- [ ] **Block pruning service** — `app/services/block_pruning.{h,c}`.

---

## AGENT3 — Wave 8 Priority Queue

### Carry-over from wave 7

- [ ] **Live wallet encryption integration** — wire `wks_encrypt`/`wks_decrypt` through `wallet_db.c` / `wallet.c` / `keystore.c` / `wallet_key.c` / `wallet_sqlite.c`. This has been carry-over for 3 waves now — **do it this wave**. Per-file regression coverage, migration path in one big `db_txn`, encrypted wallets back up ciphertext blobs.
- [x] **RPC timeout layer** — `lib/rpc/{include/rpc,src}/rpc_timeout.{h,c}` with 128-slot in-flight table + watchdog pthread. `ZCL_RPC_TIMEOUT_MS` / `ZCL_RPC_TIMEOUT_SWEEP_MS` env knobs. New `EV_RPC_TIMEOUT` event. 14 tests in `test_rpc_timeout.c`. Wired into `httpserver.c` `handle_client()`. Pushed at `1eebebc6c`.
- [ ] **WebSocket event stream** — `lib/net/ws_events.{h,c}` with `/events?domain=…` filter.
- [ ] **OpenTelemetry-compat tracing** — `lib/util/trace.{h,c}` + 5 migrated hot paths.
- [ ] **peer_bandwidth wire-in** — primitives exist; wire into `connman.c` send/recv, pause/resume on bucket state.
- [ ] **MCP TLS transport** — optional TLS listener, reuse `https_server.c`.
- [ ] **Alert routing** — `lib/util/alerts.{h,c}` with webhook/email/log sinks.
- [ ] **Chaos fault injection** — `tools/mcp/chaos.{h,c}` under `#ifdef ZCL_CHAOS`.
- [ ] **Coverage 26% → 35%** — audit highest-LOC uncovered files, targeted tests.
- [x] **`zcl_consensus_report`** — bounded (kind, reason) → count table (48-slot cap + per-kind totals + overflow buckets) in `tools/mcp/metrics.c`. Observer parses `reason=…` off AGENT2's `EV_CONSENSUS_REJECT_*` events. Surfaces via new `zcl_consensus_report` ops MCP tool and `zcl_consensus_rejects_total{kind,reason}` Prometheus family (with per-kind `all` + global `all` + `__other__` rows). 8 new tests. Surface: 75 → 76 tools. Pushed at `609638590`.

### New for wave 8

- [ ] **`zcl_explain_reject` MCP tool wrapper** — wrap AGENT2's `consensus_reject_index` service in a router entry under ops domain. Schema: `{block_or_txid: string, verbose: bool}`. Coordinate with AGENT2 on the service API.
- [ ] **Grafana dashboard JSON** — `docs/grafana/zclassic23.json`. Panels for chain height, peer count, UTXO count, mempool size, RPC RPS, CSR commits, recovery_policy decisions, coverage, disk free, consensus rejects by reason.
- [ ] **Operator `RUNBOOK.md`** — `docs/RUNBOOK.md`. Symptom → diagnostic tool → fix command. Scenarios: "99% disk", "peer misbehaving", "backup failed", "tip regressed", "node stuck at height X", "RPC returning 429".
- [x] **Auto-generated `MCP_REFERENCE.md`** — `make docs-mcp` pipes `./zclassic23 -mcp` `tools/list` through `tools/gen_mcp_reference.py` (pure Python stdlib) → grouped by domain + GFM parameter tables. `make docs-mcp-check` for CI drift detection. Initial reference at 586 lines. Pushed at `7d1d966d3`.
- [ ] **HTTP RPC error envelope audit** — today errors are `{error: {code, message}}`. Audit every RPC response path, ensure consistent shape, add missing fields (method, request_id). One commit per category (wallet, chain, net, ops).

### Stretch

- [ ] **gRPC alternative interface**.
- [ ] **WebAuthn/passkey auth**.
- [ ] **MCP replay recorder** (deferred from wave 7).
- [ ] **Continue tool backfill** to 85+ RPC parity.

---

## New coordination rules

1. **`REVIEW_QUEUE.md` introduced.** When an agent delivers something that needs AGENT1 review (investigation, architectural proposal, API proposal), add a row to `REVIEW_QUEUE.md`. AGENT1 processes it at the start of each coordination pass. Rows move to `Done` with a disposition.
2. **Self-review escape hatch.** If an item has clear acceptance criteria (see PHGR13 fix above for the template), the agent may self-certify and commit without waiting for AGENT1. The criteria must be stated in the wave plan.
3. **`BOOT_QUEUE.md` has a 24-hour SLA on AGENT1.** If AGENT1 hasn't batch-applied within 24 hours of a queued request, AGENT2 may commit the `boot.c` edit themselves provided every new call is gated by a service API and adds no inline business logic.
4. **Plans live in `WAVE_N.md`.** `AGENT2.md` / `AGENT3.md` Current Status sections are agent-owned.
5. **Pull from `BACKLOG.md`** when wave clears.
6. **Rebuild `zclassic23` before every `./test_zcl`** via `make zclassic23 test_zcl`.

## Territory (unchanged from wave 7)

Same split — AGENT2 owns consensus/storage/boot/fuzz/recovery, AGENT3 owns MCP/RPC/wallet-crypto/observability/docs. New files belong to whoever creates them in their territory. Cross-service coordination happens via plan entries in this file.
