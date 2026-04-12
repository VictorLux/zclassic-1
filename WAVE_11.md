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

5. **make lint fatal** — change `make lint` from advisory to fatal. Add checks for: bare `return -1` in MCP handlers, bare `malloc` in app/ code, missing `LOG_*` on error returns in new code. Gate `make ci` on it.

### Consensus hardening

6. **Compact blocks (BIP152)** — `lib/net/src/compact_blocks.{h,c}`. Short txid relay, getblocktxn/blocktxn messages. Bandwidth reduction for connected miners/peers.

7. **Dandelion tx propagation** — `lib/net/src/dandelion.{h,c}`. Stem phase (1 peer) + fluff phase (normal relay). Privacy upgrade for tx origin hiding.

8. **Addrman bucket rebalancing** — audit `lib/net/src/addrman.c`. Ensure proper new/tried bucket distribution, eviction on collision, test with adversarial peer sets.

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

1. **LOG_FAIL migration: MCP handlers** — migrate all 64 bare `return -1` in `tools/mcp/controllers/*.c` to `LOG_ERR` + set `res->error`/`res->error_message`. Every MCP failure must explain itself to the caller.

2. **LOG_FAIL migration: app controllers** — same for `app/controllers/src/*.c`. Every `return false`/`return -1` gets `LOG_FAIL`/`LOG_ERR` with domain tag.

3. **LOG_FAIL migration: app services** — same for `app/services/src/*.c`. Priority: `snapshot_sync_service.c` (100 bare returns), `block_sync_service.c`, `header_sync_service.c`.

4. **zcl_malloc migration: app/ + tools/** — replace bare `malloc`/`calloc` in all app/ and tools/ files. Include `safe_alloc.h`. Count before/after.

5. **LOG_FAIL migration: net layer** — same for `lib/net/src/*.c`. Priority: `msgprocessor.c` (55 bare returns), `fast_sync.c` (59), `net.c` (50).

### Security

6. **TLS for HTTP RPC** — optional TLS listener for non-loopback RPC. Env `ZCL_RPC_TLS_CERT` + `ZCL_RPC_TLS_KEY`. Reuse OpenSSL already linked.

7. **Bloom filter (BIP37) audit** — audit `lib/bloom/`. Either deprecate (privacy leak) or gate behind `ZCL_ENABLE_BIP37=1` env var. Default off.

### Wallet

8. **HD wallet Phase A (BIP32)** — hierarchical deterministic key derivation. `lib/wallet/src/hd_keychain.{h,c}`. Master seed -> account -> change -> address chain. 10+ tests.

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
