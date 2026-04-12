# Wave 10 — Active Work Plan

**Status:** live. Replaces wave 9.
**Previous:** `WAVE_9.md`
**Coordinator:** AGENT1

---

## AGENT1 self-assignment

- [ ] Batch-apply BOOT_QUEUE: `bii_verify`, `wallet_backup_start`, `disk_monitor_start`, `mempool_limits_start`, `ibd_throttle_start`.
- [ ] Verify PHGR13 fix: confirm live node passed h=2,014,948.

---

## AGENT2 — Wave 10 Priority Queue

Work in order. `./test_zcl` green on every push.

### Critical (do first)

1. **Reorg safety test** — `lib/test/src/test_reorg_safety.c`. Synthesize 50-block reorg with conflicting fork through `activate_best_chain`. Assert no UTXO loss, no orphan rows, CSR + recovery_policy + db_txn hold. This is the test that proves all the safety infrastructure works together.

2. **Consensus parity audit** — 10 mainnet blocks through both `zclassic23` and `zclassic-cpp`, assert matching `coins_best_block` hashes. Write `CONSENSUS_PARITY.md`.

3. **PHGR13 live verification** — if the live node has NOT advanced past h=2,014,948, debug why, fix, and re-test. If it HAS advanced, document the final height reached in `AGENT2.md` Current Status and mark wave 9 #1 fully closed.

### Architecture

4. **Boot decomposition Phase A** — extract `block_index_loader.{h,c}` from boot.c. All `block_index.bin` read/write + `bii_verify()` into new service. 6+ tests.

5. **Boot decomposition Phase B** — extract `chain_state_validator.{h,c}`. Cross-check block_map vs SQLite vs coins_best_block vs active_chain. 5+ tests.

6. **Boot decomposition Phase C** — extract `utxo_recovery_service.{h,c}`. Wipe/recover gated through recovery_policy. 8+ tests. **Target: boot.c < 1400 lines.**

### Hardening

7. **Script/sigcache parallelism** — `lib/util/workpool.{h,c}` + parallelize `connect_block.c` script verification loop. Target: 2× on 8-core.

8. **BIP113/BIP65 time hardening** — audit `contextual_check_tx.c` + `check_block.c` against MTP semantics. Adversarial-timestamp tests.

9. **Mempool orphan pool** — max 50 txs, 10-min TTL, reconnect on parent arrival. `test_mempool_orphan.c`.

10. **Fix fuzzer finding #2** — `test_json.c` segfaults under `-O1 + gcov`. Valgrind, diagnose, fix in separate commit.

### New for wave 10

11. **SQLite WAL size cap** — prevent unbounded WAL on slow checkpoints. Cap at env `ZCL_WAL_MAX_BYTES` (default 100MB), force checkpoint when exceeded.

12. **Fee estimation robustness** — protect fee estimation against manipulation. Audit current fee calculation, add tests with adversarial fee distributions.

13. **Headers-first sync refinement** — tighter getheaders loop, exponential backoff on stale peers, better locator construction.

### Stretch

- [ ] Drive CSR migration to zero (~56 remaining sites)
- [ ] Snapshot automation (nightly if at-tip, rotate last 7)
- [ ] Block pruning service
- [ ] Compact blocks (BIP152) implementation

---

## AGENT3 — Wave 10 Priority Queue

Work in order. `./test_zcl` green on every push.

### Observability & networking

1. **WebSocket event stream** — `lib/net/ws_events.{h,c}` with `/events?domain=…` filter, per-client ring buffer, heartbeat, max 100 subscribers. Subscribe via `event_subscribe()`.

2. **OpenTelemetry-compat tracing** — `lib/util/trace.{h,c}` W3C Trace Context format. Migrate 5 hot paths: MCP dispatch, HTTP RPC dispatch, `connect_tip`, `csr_commit_tip`, `snapsync_begin_receive`.

3. **peer_bandwidth wire-in** — primitives in `lib/net/src/peer_bandwidth.c`. Wire token-bucket into `connman.c` send/recv. Pause starved peers, resume on refill. `EV_PEER_THROTTLED`.

### Security

4. **RPC cookie rotation** — timed rotation (default 24h, env `ZCL_RPC_COOKIE_ROTATE_SEC`). Existing connections valid until next window.

5. **Sapling key scrubbing** — audit every path touching spending keys (`sk`, `ask`, `nsk`, `ovk`). `explicit_bzero` on free/scope-exit. `test_key_scrub.c`.

6. **Dependency vulnerability scan** — CI job: audit OpenSSL, libevent, SQLite, leveldb versions against known CVEs. `tools/dep_audit.sh` script + `make audit`.

### Coverage & quality

7. **Coverage 26% → 35%** — audit highest-LOC 0% files, write targeted tests. Post before/after in Current Status.

8. **HTTP RPC error envelope audit** — consistent `{error: {code, message, method, request_id}}` shape everywhere. One commit per category (wallet, chain, net, ops).

9. **Property-based tests for tx validation** — quickcheck-style randomized inputs to `check_transaction`, assert no crashes and correct accept/reject.

### Docs & operator experience

10. **Grafana dashboard JSON** — `docs/grafana/zclassic23.json`. Panels: chain height, peer count, UTXO count, mempool size, RPC RPS, disk free, consensus rejects.

11. **Operator RUNBOOK.md** — `docs/RUNBOOK.md`. Symptom → diagnostic tool → fix. Scenarios: 99% disk, peer misbehaving, backup failed, tip regressed, node stuck, RPC 429.

12. **Architecture diagrams** — `docs/ARCHITECTURE_DIAGRAMS.md` with mermaid diagrams: boot sequence, P2P flow, block validation pipeline, wallet tx lifecycle, MCP routing.

### Features

13. **Watch-only address support** — `zcl_importaddress` RPC + MCP tool. Track balance/transactions without private keys. No spending.

14. **MCP replay recorder** — `tools/mcp/replay.{h,c}` — ring buffer of last 100 requests+responses. `zcl_replay_dump` + `zcl_replay_exec` tools.

15. **Transaction coin selection audit** — review BnB vs knapsack algorithm in wallet send path. Add tests with adversarial UTXO distributions (dust, many small, few large).

### Stretch

- [ ] MCP TLS transport (optional TLS listener, reuse `https_server.c`)
- [ ] Chaos fault injection (`tools/mcp/chaos.{h,c}` under `#ifdef ZCL_CHAOS`)
- [ ] Continue tool backfill to 85+ RPC parity
- [ ] Bloom filter (BIP37) audit — deprecate or gate behind config

---

## Rules

1. Plans in `WAVE_N.md`. Tick items in the commit that closes them.
2. Agent status in `AGENT*.md` — AGENT1 doesn't edit Current Status.
3. Pull from `BACKLOG.md` when wave clears.
4. `BOOT_QUEUE.md` for boot.c edits.
5. `REVIEW_QUEUE.md` for expert review.
6. Self-certify with clear acceptance criteria.
7. `make zclassic23 test_zcl` before every push.
8. **No Docker. Ever.**
9. Reach for stretch when priority list clears.

## Territory

**AGENT2:** consensus/storage/boot(via queue)/fuzz/recovery, `lib/sapling/src/sprout.c`, `lib/validation/*`, `lib/chain/*`.
**AGENT3:** MCP/RPC/wallet-crypto/observability/docs, `tools/mcp/**`, `lib/net/peer_*`, `lib/wallet/*`, `lib/rpc/*`, `lib/util/{log_json,trace,alerts}.*`.
