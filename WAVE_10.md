# Wave 10 — Active Work Plan

**Status:** live. Replaces wave 9.
**Previous:** `WAVE_9.md`
**Coordinator:** AGENT1

---

## AGENT1 self-assignment

- [x] Batch-apply BOOT_QUEUE: `bii_verify`, `wallet_backup_start`, `disk_monitor_start`, `mempool_limits_start`, `ibd_throttle_start`.
- [x] Verify PHGR13 fix: confirmed — live node at h=2,014,988+ (2026-04-12). Required fixing 4 block-index height propagation bugs first.

---

## AGENT2 — Wave 10 Priority Queue

Work in order. `./test_zcl` green on every push.

### Critical (do first)

1. ~~**Reorg safety test**~~ — **DONE.** `lib/test/src/test_reorg_safety.c` — 23 tests: synthesizes 50-block reorg with conflicting fork. Exercises disconnect_block + update_coins for both chains. Asserts no UTXO loss, no orphan rows, CSR commit acceptance, recovery_policy allow/reject, db_txn scoped commit/rollback, non-coinbase spend undo, partial reorg (25/50), 5 rapid reorg cycles, commitment tracking. All safety infrastructure verified together.

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

1. ~~**WebSocket event stream**~~ — **DONE.** `lib/net/{include/net/ws_events.h,src/ws_events.c}` — RFC 6455 WebSocket endpoint at `GET /events?domain=chain,peer`. Internal 4096-slot lock-free event queue fed by per-type observers. Background pump thread writes JSON text frames at 100ms intervals. Per-client domain prefix filter, heartbeat ping every 30s, idle timeout 90s, max 100 subscribers. `ws_events_upgrade()` handles the full handshake (SHA-1 + base64 Sec-WebSocket-Accept). Hooked into `httpserver.c` — detects `Upgrade: websocket` header on `GET /events`. 5 tests in `test_ws_events.c`.

2. ~~**OpenTelemetry-compat tracing**~~ — **DONE.** `lib/util/{include/util/trace.h,src/trace.c}` — W3C Trace Context compatible spans with thread-local parent-child stack, /dev/urandom trace/span IDs, OTLP-compatible JSON output via log_jsonf. 5 hot paths instrumented: MCP dispatch (router.c), HTTP RPC dispatch (httpserver.c), connect_tip (process_block.c), csr_commit_tip via process_block_commit_tip (process_block.c), snapsync_begin_receive (snapshot_sync_service.c). 10 tests in `test_trace.c`.

3. ~~**peer_bandwidth wire-in**~~ — **DONE.** Existing token-bucket primitives wired into `connman.c` socket handler: download budget checked before recv (caps recv size), upload budget checked before socket_send_data, post-operation consume. Throttled peers skipped for that poll cycle (50ms), resume on refill. `EV_PEER_THROTTLED` emitted. Global instance init from env at `connman_start()`, `connman_peer_bandwidth()` getter exposed.

### Security

4. ~~**RPC cookie rotation**~~ — **DONE.** `lib/rpc/src/httpserver.c` — timed rotation (default 24h, env `ZCL_RPC_COOKIE_ROTATE_SEC`). Background pthread rotates cookie on interval, writes new `.cookie` to disk. Dual-password auth: current + previous cookie both valid during transition window; previous invalidated on next rotation. `memory_cleanse` scrubs old passwords. `rpc_http_cookie_rotate()` exposed for manual/test use. `ZCL_RPC_COOKIE_ROTATE_SEC=0` disables rotation. Explicit `rpcuser`/`rpcpassword` mode unaffected. 8 tests in `test_cookie_rotation.c`.

5. ~~**Sapling key scrubbing**~~ — **DONE.** Audited all paths touching spending keys (`sk`, `ask`, `nsk`, `ovk`). `memory_cleanse()` added to 16 uncleansed stack buffers in `lib/sapling/src/prf.c` (5 buffers: `sprout_prf` blob, `prf_expand` blob, `prf_ask`/`prf_nsk`/`prf_ovk` tmp) and `lib/sapling/src/zip32.c` (11 buffers: `expsk_from_spending_key` digest+scalars, `expsk_derive_child` digest+6 Fs scalars, `dk_master` digest, `dk_derive_child` digest, `zip32_xsk_master` i_master, `zip32_xsk_derive` tmp+expsk_bytes+fvk, `fvk_derive_child` digest+4 scalar buffers, `zip32_xfvk_derive` tmp, `zip32_xfvk_address` ivk). Wallet layer (`sapling_keys.c`, `wallet_sqlite.c`, `wallet.c`, `wallet_db.c`) already properly cleansed — no changes needed. 10 tests in `test_key_scrub.c`.

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
