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

2. ~~**Consensus parity audit**~~ — **DONE.** `CONSENSUS_PARITY.md` + `tools/consensus_parity_audit.sh`. 10/10 block hashes match between C23 (h=2,014,988) and C++ (h=2,015,541). Full chain consensus parity confirmed from genesis to h=2,014,988.

3. ~~**PHGR13 live verification**~~ — **DONE.** Live node at h=2,014,988, past the h=2,014,948 barrier. Wave 9 #1 fully closed. Documented in AGENT2.md Current Status.

### Architecture

4. ~~**Boot decomposition Phase A**~~ — **DONE.** `app/services/{include/services/block_index_loader.h,src/block_index_loader.c}` extracted from `boot_index.c`. 5 functions moved (save/load_block_index_flat, save/load_block_index_recent, load_block_index). 577 lines extracted (boot_index.c 1709→1132). 8 tests in `test_block_index_loader.c`: flat round-trip, pprev linking, bad magic, truncated file, SQLite round-trip, small cache rejection, chain_work preservation, missing file.

5. ~~**Boot decomposition Phase B**~~ — **DONE.** `app/services/{include/services/chain_state_validator.h,src/chain_state_validator.c}` extracted from `boot_index.c`. `validate_coins_chain_agreement()` (159 lines) handles 4 recovery cases: BOOT_OK, REIMPORT, WIPE_WAIT, RESET_CHAIN. 6 tests in `test_chain_state_validator.c`. boot_index.c 1132→973 lines.

6. ~~**Boot decomposition Phase C**~~ — **DONE.** `app/services/{include/services/utxo_recovery_service.h,src/utxo_recovery_service.c}` extracted from `boot.c`. 7 public functions: `utxo_recovery_wipe`, `check_reimport_flag`, `prepare_reimport`, `import_ldb`, `restore_chain_tip`, `execute` (validation recovery), `clean_above_tip`, `backfill_shielded`. All wipe/recover paths gated through recovery_policy. 867 lines extracted + 126 lines dead code removed. boot.c 2745→1856 lines. 10 tests in `test_utxo_recovery_service.c`: policy-gated wipe allow/refuse, reimport flag detect/absent/zero-value, prepare reimport, clean above tip (stragglers/refuse>1000/no-op), LDB import skip.

### Hardening

7. ~~**Script/sigcache parallelism**~~ — **DONE.** `lib/util/{include/util/workpool.h,src/workpool.c}` — fixed-size thread pool (lazy-init, persistent workers, ring-buffer queue). `connect_block.c` refactored to two-phase script verification: Phase 1 collects all inputs' script checks and precomputed tx data on the main thread; Phase 2 dispatches them to the workpool for parallel `verify_script()`. Sequential fallback for <4 inputs or pool init failure. Early-out on first failure. 3.8× measured speedup at 4 threads. 15 tests in `test_workpool.c`: init/destroy, auto threads, bad args, single/multi item, failure propagation, mixed pass/fail, atomic counter, data modification, multiple batches, single-thread, parallel speedup, empty batch, reset after failure.

8. ~~**BIP113/BIP65 time hardening**~~ — **DONE.** Fixed BIP113 bug: `contextual_check_block` was using wall-clock block timestamp instead of MTP for time-based nLockTime checks. `check_block.c` now uses `block_index_get_median_time_past(pindex_prev)`. 24 tests in `test_bip113_bip65.c`: MTP calculation (ascending/out-of-order/short-chain/single-block), `is_final_tx` (height/time/boundary/final-sequence), adversarial timestamps (miner-advance/backdate), `contextual_check_block` MTP enforcement (reject/accept/height/boundary/final-sequence), block header MTP validation, BIP65 OP_CHECKLOCKTIMEVERIFY (negative locktime/stack-exceeds/pass/final-sequence/mixed-domain).

9. ~~**Mempool orphan pool**~~ — **DONE.** `lib/validation/{include/validation/orphan_pool.h,src/orphan_pool.c}`. Fixed-size pool (50 entries), 10-minute TTL. `orphan_pool_add/remove/exists/clear/expire/size` + `find_children/extract_children` for parent-arrival reconnection. Mutex-protected. 18 tests in `test_mempool_orphan.c`: empty/add/duplicate/full-reject/remove/clear/expire/non-expire/find-children/extract-children/multi-input/no-match/null/non-existent/zero-inputs/readd/expire-all/max-out.

10. ~~**Fix fuzzer finding #2**~~ — **DONE.** Root cause: `parse_value()` in `lib/json/src/json.c` uses recursive descent — stack overflows under `-O1+gcov` instrumentation overhead. Fix: added `JSON_MAX_DEPTH` (256) limit to `parse_value_r()`, rejecting JSON nested beyond 256 levels. Removed AGENT3's `fork()` workaround from `test.c` — `test_json` now runs normally in all build modes. 2 new tests: depth-limit rejection (300 levels), at-limit acceptance (256 levels).

### New for wave 10

11. ~~**SQLite WAL size cap**~~ — **DONE.** `app/services/{include/services/db_maintenance.h,src/db_maintenance.c}`. New `wal_max_bytes` field in schedule config + `DB_MAINT_DEFAULT_WAL_MAX_BYTES` (100MB). Background thread checks WAL file size via `stat()` each tick; forces `PRAGMA wal_checkpoint(TRUNCATE)` when WAL exceeds the cap regardless of normal interval. Env override: `ZCL_WAL_MAX_BYTES` (0 disables cap). Integrates with existing `db_maintenance` service — no new threads or complexity.

12. ~~**Fee estimation robustness**~~ — **DONE.** `lib/test/src/test_fee_estimation.c` — 15 adversarial tests: all-zero fees (no estimate), MAX_MONEY fees (no overflow), single-satoshi fees, bimodal distribution (half low/half high), spam flood followed by normal blocks, sudden fee spike, empty blocks, duplicate block heights, decay convergence, varying tx sizes per-kB consistency, high conf_target with sparse data, monotonicity across targets, remove/re-add same hash, negative fees, rapid height jumps. Audited `fees.c`: no manipulation vectors found beyond expected decay-weighted behavior.

13. ~~**Headers-first sync refinement**~~ — **DONE.** `app/services/{include/services/header_sync_service.h,src/header_sync_service.c}`, `lib/net/include/net/net.h`. Three improvements: (1) **Tighter catching-up interval**: 60s → 30s for faster convergence when behind peer. (2) **Exponential backoff on stale peers**: new `getheaders_stale_count` field on `p2p_node`; each consecutive empty header response doubles the interval (cap 600s). `syncsvc_note_headers_received()` resets on good headers. (3) **Denser locator construction**: first 12 hashes kept at step=1 (was 10) for better fork detection near tip. 16 tests in `test_header_sync.c`.

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

6. ~~**Dependency vulnerability scan**~~ — **DONE.** `tools/dep_audit.sh` audits vendored OpenSSL (3.0.13→needs ≥3.0.16), SQLite (3.49.0 ✓), libevent (2.1.12 ✓), leveldb (1.18 ✓), zlib (1.3 ✓), libsecp256k1 (present ✓). Supports `--json` for CI. `make audit` target wired.

### Coverage & quality

7. ~~**Coverage 26% → 35%**~~ — **DONE (42.3%).** 4 new test files: `test_znam.c` (32 tests), `test_htlc.c` (30 tests), `test_file_market.c` (22 tests), `test_strong_params.c` (30 tests). Fixed coverage crash: `test_json` segfaults under `-O1+gcov`, isolated in `fork()` child under `COVERAGE_BUILD` — recovered ~17pp of previously-lost coverage. Before: 25.3%, after: 42.3%.

8. ~~**HTTP RPC error envelope audit**~~ — **DONE.** Consistent `{result:null, error:{code, message, method}, id:<id>}` shape everywhere. Core: added `json_rpc_error_full()` and `json_rpc_error_response()` to protocol.h/c. HTTP server: all 10 error paths (ban, rate-limit, WS full, method-not-allowed, unauthorized, payload-too-large, parse-error, invalid-request, OOM, server-busy) now produce proper JSON-RPC envelopes with numeric codes. RPC dispatch: warmup and method-not-found errors now include `method` field. MCP rpc_client: connection errors use structured `{code, message}`. Fixed critical bug: `rpc_table_execute()` errors were placed in `result` field instead of `error` field. 15 tests in `test_rpc_error_envelope.c`.

9. ~~**Property-based tests for tx validation**~~ — **DONE.** `lib/test/src/test_tx_property.c` — 18 QuickCheck-style property tests with deterministic PRNG (xoshiro128+). Properties: valid tx acceptance (sprout/overwinter/sapling), negative output rejection, MAX_MONEY overflow, total overflow, empty inputs/outputs, duplicate inputs, version 0 rejection, bad versionGroupId, expiry height threshold, coinbase+joinsplit rejection, non-coinbase null prevout, non-zero value_balance without shielded, value_balance out of range, coinbase script bounds, 500-round random mutation crash test, duplicate sapling nullifiers, MAX_MONEY boundary, overwinter version bounds.

### Docs & operator experience

10. ~~**Grafana dashboard JSON**~~ — **DONE.** `docs/grafana/zclassic23.json`. 4 row sections (Node Overview, Chain & Validation, Network, RPC & MCP, Disk & System), 19 panels: chain height, peer count, UTXO count, mempool size/bytes, disk free, chain height over time, blocks connected/min, consensus rejects (stacked bars), bg validation gauge, coins flush lag, peer count over time, peer offences by kind, bans issued/active, RPC rps (allowed/rate-limited/banned), MCP tool latency p50/p99, MCP tool error rate, auth failures, top tools by request count, MCP timeouts, tracked IPs, disk free over time, WAL size. Prometheus datasource variable, 10s auto-refresh, threshold coloring throughout.

11. ~~**Operator RUNBOOK.md**~~ — **DONE.** `docs/RUNBOOK.md`. 9 symptom-driven scenarios: disk >99% full, peer misbehaving/banned, wallet backup failed, tip regressed/wrong fork, node stuck (not syncing), RPC 429 rate limited, RPC auth failures/unexpected bans, high memory usage, boot failure (won't start). Each section: symptoms, diagnose commands (CLI + MCP), step-by-step fix, prevention. Quick reference tables for all environment variables and key events with severity.

12. ~~**Architecture diagrams**~~ — **DONE.** `docs/ARCHITECTURE_DIAGRAMS.md`. 5 Mermaid diagrams: boot sequence (full flow from CLI parse through DB open, UTXO import, chain validation, recovery policy, service startup to NODE_READY), P2P network flow (discovery, handshake, sync with FlyClient/snapshot paths, steady-state relay, bandwidth control, peer scoring), block validation pipeline (structure checks, contextual checks, connect_block with script/Groth16/turnstile verification, UTXO checkpoint, flush, reorg path), wallet transaction lifecycle (coin selection, transparent vs shielded build, sign, mempool, block inclusion, wallet scan with trial decryption), MCP request routing (stdio parse, middleware auth/rate-limit chain, router dispatch with validation, handler domains, RPC layer, observability hooks), onion service architecture (embedded Tor, dynhost, direct C function calls).

### Features

13. ~~**Watch-only address support**~~ — **DONE.** `importaddress` RPC + `zcl_importaddress` MCP tool. Full stack: `keystore_add_watch_only_id()` for address-only import (no pubkey needed), `wallet_is_mine()` now checks watch-only keys, `wallet_is_watch_only()` distinguishes watch-only from spendable. New `wallet_watch_only` SQLite table with persistence via `wallet_sqlite_write_watch_only()`/`wallet_sqlite_read_watch_only()`. Loaded at boot and rescan. Instant UTXO indexing (same as `importprivkey`). Returns `{address, watch_only, utxos, balance}`. Rejects if private key already in wallet. 9 tests in `test_watch_only.c`: add_by_id, dedup, add_by_pubkey, remove, wallet_is_mine_watch_only, wallet_is_watch_only_false_for_full_key, unknown_address, sqlite_round_trip, sqlite_overwrite.

14. ~~**MCP replay recorder**~~ — **DONE.** `tools/mcp/replay.{h,c}` — 100-slot ring buffer recording every MCP request/response pair with timestamp, duration, error status. Hooked into `mcp_router_dispatch()` — zero-config, automatic recording. Self-referential calls (`zcl_replay_*`) excluded to prevent recursion. Args serialized via `json_write()`, responses truncated at 4KB. `mcp_replay_init()` called at MCP server startup. `zcl_replay_dump(count)` returns oldest-to-newest JSON array. `zcl_replay_exec(index)` re-dispatches a recorded tool by name. Both registered as ops-domain MCP tools.

15. ~~**Transaction coin selection audit**~~ — **DONE.** Reviewed `wallet_select_coins()` (wallet.c:622-643): uses naive first-fit greedy, no BnB or knapsack. Documented findings: order-dependent selection, no dust avoidance, no change minimization, target=0 suboptimality. 15 tests in `test_coin_selection.c` with adversarial UTXO distributions: exact single coin, 100 dust UTXOs (insufficient), dust+large (all-dust-then-large pathology), large-before-dust (single select), few large coins, empty set, all unspendable, target=0, exact sum of two, 200 small coins (0.01 ZCL), max_selected limit, mixed spendable/unspendable, single satoshi, change overshoot (8.5 ZCL on 1.5 target), order-dependence proof ([1,2,3]→2 inputs vs [3,2,1]→1 input).

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
