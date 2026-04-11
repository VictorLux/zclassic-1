# Wave 9 — Active Work Plan

**Status:** live. Replaces wave 8 as the active surface.
**Previous:** `WAVE_8.md` (uncompleted items carried forward here).
**Coordinator:** AGENT1 — boot.c wire-ups + PHGR13 review.

---

## AGENT1 self-assignment

- [ ] Batch-apply 5 BOOT_QUEUE items: `bii_verify`, `wallet_backup_start`, `disk_monitor_start`, `mempool_limits_start`, `ibd_throttle_start`. Single session, single commit.
- [ ] Process `REVIEW_QUEUE.md`.

---

## AGENT2 — Wave 9 Priority Queue

Work in order. When done, pull from `BACKLOG.md`.

### Critical path (do these first)

1. **PHGR13 fix** — self-certify rules from wave 8 still apply: both bugs fixed, `test_phgr13_fix.c` loads real joinsplit, `./test_zcl` green, live node advances past h=2,014,948, commit references `PHGR13_INVESTIGATION.md`. If criterion #4 fails, revert and flag for AGENT1.

2. **Reorg safety test** — `lib/test/src/test_reorg_safety.c`. Synthesize a 50-block reorg with a conflicting fork. Drive through `activate_best_chain`. Assert no UTXO loss, no orphan rows, CSR + recovery_policy + db_txn all hold. This is the test that proves the safety infrastructure actually works.

3. **Consensus parity audit** — pick 10 mainnet blocks, run through both `zclassic23` and `zclassic-cpp`, assert matching `coins_best_block` hashes. Write results to `CONSENSUS_PARITY.md`.

### Architecture

4. **Boot decomposition Phase A** — extract `block_index_loader.{h,c}` from boot.c. Move all `block_index.bin` read/write + `bii_verify()` into the new service. Replace boot.c callsite with single `block_index_loader_load()`. 6+ tests.

5. **Boot decomposition Phase B** — extract `chain_state_validator.{h,c}`. Cross-check block_map vs SQLite blocks vs coins_best_block vs active_chain. Refuse-to-boot on mismatch unless `ZCL_ALLOW_INCONSISTENT_CHAIN=1`. 5+ tests.

6. **Boot decomposition Phase C** — extract `utxo_recovery_service.{h,c}`. Wipe/recover decision gated through recovery_policy. 8+ tests. **Target: boot.c < 1400 lines after A/B/C.**

### Hardening

7. **Script/sigcache parallelism** — `lib/util/workpool.{h,c}` + parallelize `connect_block.c` script verification. Target: 2× speedup on 8-core.

8. **BIP113/BIP65 time hardening** — audit `contextual_check_tx.c` + `check_block.c` against MTP semantics. Add adversarial-timestamp tests.

9. **Mempool orphan pool** — max 50 txs, 10-min TTL, reconnect on parent arrival. Tests in `test_mempool_orphan.c`.

10. **Chain rollback stress test** — `lib/test/src/test_chain_rollback.c`. Disconnect 100 blocks, verify UTXO commitment matches at each height.

11. **Fix fuzzer finding #2** — `test_json.c` segfaults under `-O1 + gcov`. Run under valgrind, fix in separate commit from discovery.

### Stretch

- [ ] Drive CSR migration to zero (~56 remaining sites)
- [ ] Snapshot automation (nightly if at-tip, rotate last 7)
- [ ] Block pruning service

---

## AGENT3 — Wave 9 Priority Queue

Work in order. When done, pull from `BACKLOG.md`.

### Critical carry-over (do this FIRST — 4th wave asking)

1. ~~**Live wallet encryption integration**~~ — **DONE (535ef05ae).** Wired `wks_encrypt`/`wks_decrypt` through `wallet_sqlite.c` (write_key, read_keys, write_sapling_seed, read_sapling_seed, write_sapling_key, read_sapling_keys). Transparent WKS1 envelope detection on read — no schema migration needed. Mixed plaintext+encrypted DBs read cleanly. `ZCL_WALLET_PASSPHRASE` env controls. 7 integration tests in `test_wallet_sqlite_enc.c`: plaintext roundtrip, encrypted roundtrip, unreadable without pass, wrong pass fails, mixed DB, seed encrypt/decrypt, plaintext seed compat. **4-wave carry-over finally shipped.**

### Observability & networking

2. **WebSocket event stream** — `lib/net/ws_events.{h,c}` with `/events?domain=…` filter, per-client ring buffer, heartbeat, max 100 subscribers.

3. **OpenTelemetry-compat tracing** — `lib/util/trace.{h,c}` W3C Trace Context format. Migrate 5 hot paths: MCP dispatch, HTTP RPC dispatch, `connect_tip`, `csr_commit_tip`, `snapsync_begin_receive`.

4. **peer_bandwidth wire-in** — primitives exist in `lib/net/src/peer_bandwidth.c`. Wire token-bucket calls into `connman.c` send/recv. Pause starved peers, resume on refill. Emit `EV_PEER_THROTTLED`.

5. **Alert routing** — `lib/util/alerts.{h,c}` — threshold rules on `EV_*` events, dispatch to webhook (`ZCL_ALERT_WEBHOOK_URL`), email sink, log sink. Seed with 4 rules: disk_low, peer_bans_high, rpc_ratelimit_spike, chain_tip_rejected.

### Security

6. **RPC cookie rotation** — timed rotation (default 24h, env `ZCL_RPC_COOKIE_ROTATE_SEC`). Existing connections stay valid until next window.

7. **Sapling key scrubbing** — audit every path touching spending keys (`sk`, `ask`, `nsk`, `ovk`). `explicit_bzero` on free/scope-exit. Write `test_key_scrub.c` verifying freed memory is zeroed.

### Coverage & docs

8. **Coverage 26% → 35%** — audit highest-LOC uncovered files, write targeted tests.

9. **Grafana dashboard JSON** — `docs/grafana/zclassic23.json`. Panels: chain height, peer count, UTXO count, mempool size, RPC RPS, CSR commits, recovery_policy decisions, disk free, consensus rejects.

10. **Operator RUNBOOK.md** — `docs/RUNBOOK.md`. Symptom → diagnostic tool → fix. Scenarios: 99% disk, peer misbehaving, backup failed, tip regressed, node stuck, RPC 429.

11. **HTTP RPC error envelope audit** — ensure consistent `{error: {code, message, method, request_id}}` shape across all RPC response paths. One commit per category (wallet, chain, net, ops).

### Features

12. **Watch-only address support** — `zcl_importaddress` RPC + MCP tool. Track balance/transactions for addresses without private keys.

13. **`make ci` target** — single command: `make test` + `make fuzz-ci` + `make coverage`. Fail-fast.

14. **MCP replay recorder** — `tools/mcp/replay.{h,c}` — ring buffer of last 100 requests+responses. `zcl_replay_dump` + `zcl_replay_exec` tools.

15. **Architecture diagrams** — `docs/ARCHITECTURE_DIAGRAMS.md` with mermaid diagrams: boot sequence, P2P message flow, block validation pipeline, wallet tx lifecycle, MCP routing.

### Stretch

- [ ] MCP TLS transport (optional TLS listener, reuse `https_server.c`)
- [ ] Chaos fault injection (`tools/mcp/chaos.{h,c}` under `#ifdef ZCL_CHAOS`)
- [ ] Continue tool backfill to 85+ RPC parity
- [ ] gRPC alternative interface

---

## Coordination rules

1. **Plans live in `WAVE_N.md`.** Agents tick items in the same commit that closes each.
2. **Agent status in `AGENT*.md`.** AGENT1 doesn't edit Current Status — agents own it.
3. **Pull from `BACKLOG.md`** when wave clears.
4. **`BOOT_QUEUE.md`** for boot.c edits. AGENT1 batches.
5. **`REVIEW_QUEUE.md`** for expert review items.
6. **Self-certify escape hatch**: if clear acceptance criteria exist (like PHGR13), agent may commit without AGENT1 pre-review.
7. **Rebuild before test**: `make zclassic23 test_zcl` is canonical.
8. **No Docker. Ever.**
9. **Reach down for stretch** in the same session if priority list clears.

## Territory (unchanged)

**AGENT2:** consensus/storage/boot(via queue)/fuzz/recovery services, `lib/sapling/src/sprout.c`.
**AGENT3:** MCP/RPC/wallet-crypto/observability/docs, `tools/mcp/**`, `lib/net/peer_*`, `lib/wallet/*`, `lib/rpc/*`, `lib/util/{log_json,trace,alerts}.*`.
