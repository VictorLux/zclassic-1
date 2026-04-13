# Wave 11 — Active Work Plan

**Status:** live. Replaces wave 10.
**Previous:** `WAVE_10.md`
**Coordinator:** AGENT1

---

## Theme: Defensive Migration + Production Hardening

Wave 10 built the infrastructure. Wave 11 makes the codebase use it everywhere.
The #1 gap: 1,200+ bare `return false` and 189 bare `return -1` with zero context.
By end of wave 11, every error tells you why it happened.

---

## AGENT2 — Wave 11 Priority Queue

Work in order. `./test_zcl` green on every push.

### Defensive migration (highest priority)

1. **LOG_FAIL migration: validation layer** — migrate all bare `return false`/`return -1` in `lib/validation/src/*.c` to `LOG_FAIL`/`LOG_ERR`/`GUARD` macros. Include `log_macros.h` in every file. Count before/after in commit message.

2. **LOG_FAIL migration: consensus layer** — same for `lib/consensus/src/*.c`, `lib/chain/src/*.c`. Every rejection must log domain + reason.

3. **LOG_FAIL migration: storage layer** — same for `lib/storage/src/*.c`. Every SQLite error must log the failing query context.

4. **zcl_malloc migration: lib/** — replace bare `malloc`/`calloc` with `zcl_malloc`/`zcl_calloc` in all non-vendor lib/ files. Include `safe_alloc.h`. Count before/after.

5. ~~**make lint fatal**~~ ✓ — `make lint` now fatal. Checks: bare `return -1` in MCP handlers, bare `malloc`/`calloc` in app/tools code. `make ci` gates on lint. All current code passes clean.

### Consensus hardening

6. ~~**Compact blocks (BIP152)**~~ ✓ — `lib/net/src/compact_blocks.{h,c}`. SipHash-2-4 short txid relay, sendcmpct/cmpctblock/getblocktxn/blocktxn messages. Block relay uses compact blocks for opted-in peers. 14 tests.

7. ~~**Dandelion tx propagation**~~ ✓ — `lib/net/src/dandelion.{h,c}`. Dandelion++ stem/fluff phases for tx origin privacy. Epoch-based stem peer rotation (~10 min), 10% per-hop fluff probability, 30s embargo timeout with auto-fluff fail-safe, stempool dedup + eviction. Integrated into msgprocessor tx relay + inv handling. 14 tests.

8. ~~**Addrman bucket rebalancing**~~ ✓ — audited `lib/net/src/addrman.c`. Eclipse attack mitigation in `make_tried()`: tried→new eviction now checks if new-bucket occupant is still good before evicting, tries alternative buckets first. Added `addrman_consistency_check()` (5-point invariant verifier) and `addrman_get_bucket_stats()` for monitoring. 12 tests: empty/add/promotion consistency, bucket stats, single-source flood resistance, diverse-source distribution, tried collision consistency, select, attempt tracking, get_addr pct limit, terrible eviction, bucket determinism.

### Storage

9. **Block pruning service** — `app/services/src/block_pruning_service.{h,c}`. Prune blocks older than N (env `ZCL_PRUNE_KEEP_BLOCKS`, default 1000). Keep headers. Disk savings for non-archival nodes.

10. **Database schema migration framework** — `lib/storage/src/schema_migration.{h,c}`. Versioned migrations table, up/down functions, boot-time auto-migrate. No more ad-hoc ALTER TABLE.

### Testing

11. **Coverage push to 55%** — audit highest-LOC 0% files, write targeted tests. Priority: `process_block.c`, `transaction.c`, `addrman.c`, `msgprocessor.c`.

12. **1000-iteration fuzzer CI run** — `tools/fuzz_ci.sh` that runs all fuzzer harnesses for 1000 iterations. Add to `make ci`. Catch rare edge cases.

13. **Snapshot automation** — nightly snapshot if at-tip, rotate last 7. `tools/snapshot_cron.sh` + systemd timer.

---

## AGENT3 — Wave 11 Priority Queue

Work in order. `./test_zcl` green on every push.

### Defensive migration (highest priority)

1. ~~**LOG_FAIL migration: MCP handlers**~~ ✓ — 67 bare `return -1` → LOG_ERR + error body + zcl_malloc. Commit `481d008a6`.

2. ~~**LOG_FAIL migration: app controllers**~~ ✓ — 42 files, ~200 bare returns → LOG_FAIL/LOG_ERR/LOG_NULL with domain tags. All 42 files now include `log_macros.h`.

3. ~~**LOG_FAIL migration: app services**~~ ✓ — 25 files, ~150 bare returns → LOG_FAIL/LOG_ERR/LOG_NULL. Priority files hit: snapshot_sync_service.c (65), wallet_backup_service.c (25), header_sync_service.c (4), block_sync_service.c (2). All 25 files now include `log_macros.h`.

4. ~~**zcl_malloc migration: app/ + tools/**~~ ✓ — 130 bare malloc/calloc → zcl_malloc/zcl_calloc across 45 files. 0 bare malloc remaining in app/ or tools/. All files include `safe_alloc.h`.

5. ~~**LOG_FAIL migration: net layer**~~ ✓ — 427 bare returns → LOG_FAIL/LOG_ERR/LOG_NULL/GUARD across all 21 `lib/net/src/*.c` files. Priority files: msgprocessor.c (51), fast_sync.c (59), net.c (44), addrman.c (63), file_service.c (46). All files now include `log_macros.h`. 64 intentional control-flow returns left unchanged.

### Security

6. ~~**TLS for HTTP RPC**~~ ✓ — Optional TLS listener on 0.0.0.0:rpcport+1. Env `ZCL_RPC_TLS_CERT` + `ZCL_RPC_TLS_KEY` + optional `ZCL_RPC_TLS_PORT`. TLS 1.2+ via OpenSSL. Plain loopback listener unchanged. 3 tests.

7. ~~**Bloom filter (BIP37) audit**~~ ✓ — BIP37 gated behind `ZCL_ENABLE_BIP37=1`, default OFF. NODE_BLOOM no longer advertised unless enabled. filterload/filteradd/filterclear handlers reject with misbehavior(100) when off. Per-peer pfilter not allocated when off. rolling_bloom (addr_known) kept as-is — internal, not BIP37. 10 tests.

### Wallet

8. ~~**HD wallet Phase A (BIP32)**~~ ✓ — hierarchical deterministic key derivation. `lib/wallet/src/hd_keychain.{h,c}`. Master seed → account → change → address chain. Seed generation, path parsing ("m/44'/147'/0'/0/5"), private + public child derivation, xpub/xprv base58check serialization, BIP32 test vector 1 compliance. 20 tests.

9. **HD wallet Phase B (BIP39)** — mnemonic seed phrases. 12/24 word generation + validation. `lib/wallet/src/mnemonic.{h,c}`. 8+ tests.

10. **HD wallet Phase C (BIP44)** — derivation paths `m/44'/147'/0'/change/index`. Wire into `getnewaddress` when HD wallet exists. 6+ tests.

11. **Multisig P2SH support** — `createmultisig`, `addmultisigaddress` RPC. Script building + signing. 8+ tests.

### Tooling & ops

12. **Release builder** — `tools/release.sh`. Tagged binary + SHA3-256 hash + detached signature. Reproducible build verification.

13. **MCP tool reference generator** — auto-generate `docs/MCP_REFERENCE.md` from router metadata. Run as `make docs`.

14. **zcl-cli improvements** — add subcommands for common ops: `zcl-cli status`, `zcl-cli send`, `zcl-cli balance`. Wrap RPC calls with better UX.

### Stretch

- [ ] NAT traversal (UPnP + NAT-PMP)
- [ ] MCP TLS transport
- [ ] Chaos fault injection framework
- [ ] Electrum protocol server (for lightweight wallets)

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
10. **Migrate on touch** — if you modify a file, migrate its bare returns to LOG_FAIL/LOG_ERR and bare mallocs to zcl_malloc before pushing.

## Territory

**AGENT2:** consensus/storage/boot(via queue)/fuzz/recovery/net-protocol, `lib/sapling/src/sprout.c`, `lib/validation/*`, `lib/chain/*`, `lib/net/src/{compact_blocks,dandelion,addrman}.*`.
**AGENT3:** MCP/RPC/wallet/observability/docs/tools, `tools/mcp/**`, `lib/net/peer_*`, `lib/wallet/*`, `lib/rpc/*`, `lib/util/{log_json,trace,alerts}.*`, `app/controllers/*`, `app/services/*`.
