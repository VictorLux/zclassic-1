# Wave 12 — Active Work Plan

**Status:** live. Replaces wave 11.
**Previous:** `WAVE_11.md`
**Coordinator:** AGENT1

---

## Theme: Battle-Tested Under Adversarial Conditions

Waves 10-11 built features and migrated defenses. Wave 12 proves everything works under stress. By end of wave 12, the node is production-grade: chaos-tested, load-tested, and hardened against every known attack vector.

---

## AGENT2 — Wave 12 Priority Queue

Work in order. `./test_zcl` green on every push.

### Finish wave 11 carry-over

1. **Coverage push to 55%** — carry from wave 11. Target: `process_block.c`, `transaction.c`, `addrman.c`, `msgprocessor.c`. Post before/after.

2. **1000-iteration fuzzer CI** — carry from wave 11. `tools/fuzz_ci.sh` + add to `make ci`.

3. **Snapshot automation** — carry from wave 11. Nightly snapshot if at-tip, rotate last 7. `tools/snapshot_cron.sh` + systemd timer.

### Adversarial testing

4. **Chaos fault injection** — `lib/test/src/test_chaos.c`. Inject: random SQLite failures, malloc failures, peer disconnects mid-block, corrupt headers, truncated messages. Assert graceful recovery from each.

5. **Load test harness** — `tools/loadtest.sh`. Hammer RPC with 100 concurrent connections, 10k requests. Measure: latency p50/p99, error rate, memory growth. Assert: zero crashes, <1% errors, no memory leak.

6. **Eclipse attack test** — simulate 8 attacker peers trying to isolate the node. Assert addrman diversity, honest peer retention, no chain isolation.

7. **Time-warp attack test** — adversarial timestamps: future drift, past manipulation, MTP edge cases. Assert BIP113/BIP65 enforcement holds.

### Consensus depth

8. **Deep reorg stress test** — 200-block reorg with double-spend attempt. Assert: UTXO consistency, no balance corruption, CSR holds, wallet correctly updates.

9. **Checkpoint enforcement** — hardcoded checkpoints at key heights. Reject forks below last checkpoint. `lib/chain/src/checkpoints.{h,c}`.

10. **Selfish mining simulation** — withhold blocks, release chain at strategic moments. Assert: honest chain wins, no permanent fork.

### Performance

11. **UTXO cache optimization** — benchmark `coins_view_cache` hit rate. Add LRU eviction policy with configurable size. Target: 95%+ cache hit during IBD.

12. **Parallel block download** — request blocks from multiple peers simultaneously during IBD. Target: 2x sync speed on good connections.

13. **Memory profiling** — `tools/memprofile.sh`. Track RSS during full sync, at-tip steady state, heavy RPC load. Document memory budget per component.

---

## AGENT3 — Wave 12 Priority Queue

Work in order. `./test_zcl` green on every push.

### Defensive depth

1. **MCP input fuzzing** — fuzz every MCP tool with randomized/malformed inputs. Assert: no crashes, no panics, proper error envelopes on all bad input.

2. **RPC authentication hardening** — brute-force test: 1000 bad passwords, assert lockout. Timing attack test: assert constant-time comparison. Test cookie file permissions (0600).

3. **Memory leak audit** — valgrind full test suite. Fix any definite leaks. Document suppressions for vendor code. Target: zero leaks in zclassic23 code.

4. **Integer overflow audit** — audit all arithmetic on block heights, amounts (int64), sizes. Add `__builtin_add_overflow` guards where needed. Test with MAX_MONEY, MAX_HEIGHT edge cases.

### Wallet hardening

5. **Wallet backup verification** — test restore from backup. Create wallet, fund it, backup, destroy, restore, verify balance matches. End-to-end.

6. **HD wallet integration test** — create HD wallet from mnemonic, derive 100 addresses, send to several, restore from mnemonic on fresh node, verify all balances recovered.

7. **Concurrent wallet access** — 10 threads doing simultaneous sends, receives, key generation. Assert no corruption, no double-spends, no deadlocks.

### API completeness

8. **OpenAPI spec generation** — auto-generate `docs/openapi.yaml` from RPC metadata. Machine-readable API contract.

9. **MCP tool reference docs** — auto-generate `docs/MCP_REFERENCE.md` from router. Every tool documented with params, return types, examples.

10. **RPC compatibility test** — run Bitcoin Core's RPC test vectors against compatible commands. Document exact compatibility matrix.

### Operational readiness

11. **Graceful shutdown test** — SIGTERM during: block sync, UTXO flush, wallet write, RPC in-flight. Assert: clean WAL state, no corruption, fast restart.

12. **Disk full simulation** — fill disk to 99%, attempt block write, assert graceful degradation (pause sync, emit alert, resume when space freed).

13. **Log rotation** — `logrotate` config + SIGHUP handler for log reopen. No unbounded log growth.

14. **Systemd watchdog** — `sd_notify` heartbeat during operation. Systemd auto-restarts on hang. Wire into main loop.

15. **Production checklist** — `docs/PRODUCTION_CHECKLIST.md`. Pre-launch verification: ports, TLS, backups, monitoring, disk, memory, peer count, sync state.

### Stretch

- [ ] Electrum protocol server (lightweight wallet support)
- [ ] NAT traversal (UPnP + NAT-PMP)
- [ ] ARM64 cross-compilation
- [ ] Reproducible builds (bit-for-bit)

---

## Rules

1. Plans in `WAVE_N.md`. Tick items in the commit that closes them.
2. Agent status in `AGENT*.md` — AGENT1 doesn't edit Current Status.
3. Pull from `BACKLOG.md` when wave clears.
4. Self-certify with clear acceptance criteria.
5. `make zclassic23 test_zcl` before every push.
6. **No Docker. Ever.**
7. **Migrate on touch** — bare returns → LOG_FAIL, bare malloc → zcl_malloc.
8. **make lint must pass** — fatal checks enforced.

## Territory

**AGENT2:** consensus/storage/boot/fuzz/recovery/net-protocol, `lib/validation/*`, `lib/chain/*`, `lib/net/src/{compact_blocks,dandelion,addrman}.*`, `lib/storage/*`.
**AGENT3:** MCP/RPC/wallet/observability/docs/tools, `tools/mcp/**`, `lib/wallet/*`, `lib/rpc/*`, `lib/util/*`, `app/controllers/*`, `app/services/*`.
