# Wave 6 — Active Work Plan

**Status:** live. Agents work items in order, reach for stretch, pull from `BACKLOG.md` when the wave is clear.

**Coordinator:** AGENT1. Push plan refreshes here instead of editing `AGENT2.md` / `AGENT3.md` Current Status (which are agent-owned logs).

---

## AGENT2 — Wave 6 Priority Queue

### Carry-over from wave 5

- [ ] **#2 PHGR13 sync stall investigation** — `lib/sapling/src/sprout.c`, produce `PHGR13_INVESTIGATION.md` with reproduction + diff vs `~/zclassic-cpp` + hypothesis + fix sketch. Don't commit the fix in the same session.
- [x] **#6 mempool_limits** — `app/services/mempool_limits.{h,c}` with env-tunable caps, eviction by fee-per-byte, expiry sweep, `EV_MEMPOOL_EVICT`/`EV_MEMPOOL_EXPIRE`. 12 tests. Post-add hook installed via `tx_mempool_set_post_add_hook`, background expiry pthread. Boot wiring queued.
- [ ] **#3/#4/#5 boot decomposition** — **UNBLOCKED** after AGENT1's upcoming boot.c session. Extract in order: `block_index_loader.{h,c}`, `chain_state_validator.{h,c}`, `utxo_recovery_service.{h,c}`. Each extraction is one commit; `./test_zcl` green; `boot.c` shrinks monotonically. Target: `boot.c` < 1400 lines.
- [ ] **#9/#10 stretch** — snapshot automation service; drive CSR migration site count toward zero.

### New for wave 6

- [ ] **Reorg safety test** — `lib/test/src/test_reorg_safety.c`. Synthesise a 50-block reorg with a conflicting fork, drive the node through `activate_best_chain`, assert no UTXO loss / no orphan rows / no `EV_CHAIN_TIP_REJECTED`. This is the test class that doesn't exist yet and that the CSR + recovery_policy were built for.
- [ ] **Script/sigcache parallelism** — audit `lib/validation/src/connect_block.c` script verification loop. Is it parallel? If not, introduce a worker pool (`lib/util/workpool.{h,c}`) and run signature checks across N threads. Measure connect-tip wall time before/after on a recent mainnet block. Target: 2× speedup on 8-core machine.
- [x] **Compact block (BIP152) — investigate only** — `BIP152_INVESTIGATION.md` landed. Finding: zero BIP152 support today (no wire messages, no short-ID index, no service flag). Recommended cost: L. No architectural blockers — Sapling tx serialization is already deterministic so SipHash short-IDs are stable. Not scheduled for implementation yet.
- [x] **Addrman persistence robustness** — `app/services/addrman_integrity.{h,c}` with 48-byte ADIX sidecar (SHA3-256 + magic + version + body_size). Wired into `connman_save_addrman` (write sidecar after atomic rename) and `connman_load_addrman` (verify before deserialize; quarantine + start fresh on mismatch). 11 tests + `EV_ADDRMAN_CORRUPT` event.
- [ ] **IBD throttle** — initial block download can saturate the DB write path. Add a simple token-bucket between `process_block` and `update_coins` so a firehose peer can't starve the rest of the node. Env: `ZCL_IBD_BLOCKS_PER_SEC` (default 500).
- [x] **Checkpoint enforcement audit** — finding: checkpoints ARE consulted in `contextual_check_block_header` (exact-height match) and enabled by default. Extracted `checkpoints_hash_at_height`, `checkpoints_last_height`, `checkpoints_validate_header` from inlined loop; refactored `check_block.c` call site; 8 unit tests in `test_chain.c` covering match/miss/reject/NULL-safety/chainparams-loaded.

### Stretch for wave 6

- [ ] **Block pruning** — `app/services/block_pruning.{h,c}` — keep last N blocks' raw data, discard older raw blocks, keep block_index entries forever. Saves disk for non-archival nodes. Env: `ZCL_PRUNE_KEEP_BLOCKS` (default 0 = archival).
- [ ] **Drive CSR migration to zero** — audit the ~56 remaining call sites and either migrate each or add a `/* CSR-internal */` comment with justification.

---

## AGENT3 — Wave 6 Priority Queue

### Carry-over from wave 5

- [ ] **#2 live wallet encryption integration** — wire `wks_encrypt`/`wks_decrypt` through `wallet_db.c` / `wallet.c` / `keystore.c` / `wallet_key.c` / `wallet_sqlite.c` (~3500 lines to audit). Each controller that touches a key gets per-file regression coverage. Migration path: plaintext → encrypted in one big `db_txn` at first boot after `ZCL_WALLET_PASSPHRASE` is set. Coordinate with AGENT2's `wallet_backup_service` — encrypted wallets back up ciphertext blobs, never plaintext.
- [ ] **#1 RPC timeout layer** — the `ZCL_RPC_TIMEOUT_MS` piece of wave 5 #1 hasn't landed yet per AGENT3's status note. Add a watchdog thread that kills connections past timeout, log `EV_RPC_TIMEOUT` with method + elapsed.
- [ ] **#3 WebSocket event stream** — `lib/net/ws_events.{h,c}` with `/events?domain=…` filter, per-client ring buffer, overflow frame, heartbeat, max 100 subscribers.
- [ ] **#4 OpenTelemetry-compat tracing** — `lib/util/trace.{h,c}` + 5 migrated hot paths (MCP dispatch, HTTP RPC dispatch, `connect_tip`, `csr_commit_tip`, `snapsync_begin_receive`).
- [ ] **#6 peer bandwidth quotas** — `lib/net/peer_bandwidth.{h,c}` with token buckets per direction, localhost exemption, `EV_PEER_THROTTLED`.
- [ ] **#7 onion service health probe** — `zcl_onion_health` tool + background pthread using direct in-process `onion_service_handle_request()`.
- [x] **#8 `make coverage`** — 54c434730. Per-source object tree under `build/cov/` to dodge `.gcda` basename collisions (lib/net vs lib/rpc `protocol.c`, etc.), `-O1`+`-DCOVERAGE_BUILD`, `cov_flush.c` SIGSEGV→`__gcov_dump` handler, lcov/gcovr/plain-gcov render fallback. Baseline: **26.0% line coverage** (298 TUs, 35,535/136,714 lines). test_json segfault under gcov logged as `FUZZER_FINDINGS.md` #2.

### New for wave 6

- [x] **Prometheus `/metrics` HTTP endpoint** — `lib/rpc/src/httpserver.c` now routes `GET /metrics` to `mcp_metrics_render_prometheus()` behind `ZCL_METRICS_HTTP_ENABLE=1`, returning `text/plain; version=0.0.4` so off-the-shelf Prometheus/Grafana scrapers work out of the box. Gated by the existing rate-limit + ban middleware (no auth — scrapers don't speak Basic). Disabled path returns 404 with a clear "set ZCL_METRICS_HTTP_ENABLE=1" hint.
- [ ] **MCP TLS transport** — today MCP is stdio-only, which means AI agents on the same machine only. Add an optional TLS listener on a configurable port that speaks the same JSON-RPC protocol. Env: `ZCL_MCP_TLS_PORT`, `ZCL_MCP_TLS_CERT`, `ZCL_MCP_TLS_KEY`. Reuse `lib/net/src/https_server.c` infra. The existing middleware gates still apply.
- [ ] **Chaos fault injection** — `tools/mcp/chaos.{h,c}` + `zcl_chaos_*` tools. Under `#ifdef ZCL_CHAOS`, expose tools to inject faults: drop-N-peers, fail-next-sqlite-write, delay-csr-commit-by-ms, etc. Only compiled into test builds. Used by AGENT2's reorg safety test and the crash harness.
- [ ] **Config hot-reload** — `app/services/config_reload.{h,c}` — SIGHUP handler that re-reads env vars and propagates updates to the live middleware tunables (rate limits, bucket sizes). Audit which env vars are safe to change at runtime. Emit `EV_CONFIG_RELOADED`.
- [ ] **Performance profiling MCP tool** — `zcl_profile` — starts a 5-second `CLOCK_THREAD_CPUTIME_ID` sample over the hottest N threads, returns {thread_name, user_ns, sys_ns} map. For diagnosing "why is this node slow" at 3am without an attached debugger.
- [ ] **Alert routing** — `lib/util/alerts.{h,c}` — threshold rules fire on `EV_*` events and dispatch to configured sinks (log, webhook, email via env `ZCL_ALERT_WEBHOOK_URL`). Seed with the four rules already baked into `zcl_admin.alerts`: disk_low, peer_bans_high, rpc_ratelimit_spike, chain_tip_rejected.

### Stretch for wave 6

- [ ] **gRPC alternative interface** — proto files derived from router metadata, minimal libgrpc server stub. Parallel to MCP/HTTP-RPC surfaces.
- [ ] **WebAuthn/passkey auth for admin** — replace cookie auth on the HTTP RPC with passkey challenge-response. Off by default. Experimental.

---

## New coordination rules for wave 6

1. **Plans live in `WAVE_N.md`, not `AGENT*.md`.** Agents never see a merge conflict on `AGENT*.md` Current Status again because I stop editing that section.
2. **Pull from `BACKLOG.md` when wave clears.** After items 1-10 and stretch are done (or blocked), open `BACKLOG.md`, grab the next item in your territory, check off as "in progress" (commit that state), work it, push. No waiting for me.
3. **Checklist format.** `- [ ]` → `- [x]` with commit hash and short result. One line. Same syntax as `BOOT_QUEUE.md`.
4. **Boot.c is about to become cheaper.** AGENT1's next session wires `bii_verify`, `wallet_backup_start`, the 7 remaining wipe gates, and the `db_monitor` / `db_maintenance` starts. After that, `config/src/boot.c` is down to pure orchestration and AGENT2 can start extracting services without me in the loop.
5. **When you finish a wave 6 item, tick it off here** — edit `WAVE_6.md` in the same commit that closes the item. The commit message should say `agent2: wave 6 #N done — <one line>`.
