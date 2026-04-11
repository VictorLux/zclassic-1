# AGENT2 — Boot Decomposition, Consensus Hardening & Resource Controls

**Worktree:** `~/zclassic23-2`
**Workflow:** push directly to `master` after `./test_zcl` passes
**Coordinator:** AGENT1 (main `~/zclassic23`)
**Peer:** AGENT3 (`~/zclassic23-3`) — RPC/wallet encryption/tracing, different files

> **⇒ Active plan: [`WAVE_6.md`](./WAVE_6.md).** The priority queue, carry-over, and new items are there now. When wave 6 clears, pull from [`BACKLOG.md`](./BACKLOG.md). AGENT1 no longer edits the `Current Status` section below — it is yours.

---

## Mission

Five waves of safety infrastructure are now in place — CSR, recovery_policy, db_txn, block_index_integrity, wallet_backup, crash harness, fuzzers, peer scoring. **Wave 5 hardens the parts of the node nothing has touched yet.** Fix the live fuzzer finding before it rots. Investigate the PHGR13 consensus bug that's kept the node stuck at h=2,014,948 for days. Pull `boot.c` apart now that the services it depends on are all in place. Then protect the mempool, disk, and database from resource exhaustion.

## Already done (don't redo)

- **CSR** 9 call sites; **recovery_policy** 4 wipe gates; **db_txn** 4 recovery paths (all wrapped)
- **block_index_integrity** with SHA3 sidecar + SQLite cross-check + quarantine + 11 tests
- **wallet_backup_service** hourly rotation + 11 tests + `wallet_backup_now()`
- **crash_recovery_test** harness + `make test-crash`
- **fuzz_block/script/p2p** libFuzzer harnesses + seeds + `make fuzz-ci`
- `./test_zcl` green on every push

## Wave 5 — 10 items, reach for stretch in the same session

### 1. Fix fuzzer finding #1 — `transaction_deserialize` vin leak

`lib/primitives/src/transaction.c:451` leaks `tx->vin` on partial-parse failures. `transaction_free` doesn't walk a partially-initialised vin array, so fuzzer input that fails in the middle of vin parsing leaks the N-1 already-parsed inputs.

Separate commit: `agent2: fix transaction_deserialize vin leak (fuzzer finding #1)`. Add a regression test in `lib/test/src/test_transaction.c` that synthesises a short-buffer-in-the-middle-of-vin and asserts no leak via `__sanitizer_print_memory_profile` or a simple before/after free counter.

Add to new `FUZZER_FINDINGS.md` (format below). Mark this one as **fixed**.

### 2. PHGR13 sync stall investigation — `lib/sapling/src/sprout.c`

The node has been stuck at **h=2,014,948** because peers reject historical blocks with `bad-txns-joinsplit-phgr13-invalid`. This is a known consensus regression in the Sprout PHGR13 verifier that nobody has debugged.

Your job: investigate, don't fix yet. Produce a dated `PHGR13_INVESTIGATION.md` at the repo root with:

1. **Reproduction**: pick one rejected block (suggestion: whatever block follows h=2,014,948) and run it through `verify_joinsplit()` in isolation with debug logging. Show the exact intermediate state where verification diverges.
2. **Diff against zclassicd**: use `~/zclassic-cpp` as reference. Compare the PHGR13 verifier path — which struct fields differ, which operation ordering, which field-arithmetic loops?
3. **Hypothesis**: one sentence. "Field reduction in X is wrong because Y."
4. **Fix sketch**: 5-line patch or pseudocode. Don't commit the fix in the same session — flag it in Current Status and I'll review.

This is open-ended research work. Spend up to one session on it; if you can't reach a hypothesis in a session, write down what you ruled out and move on.

### 3. Boot decomposition Phase A — `block_index_loader`

`boot.c` is **2640 lines**. AGENT1 is wiring the three queued services (`block_index_integrity`, `wallet_backup_service`, and replaying the 7 remaining `node_db_wipe_utxos` sites through `recovery_policy`) in a dedicated boot.c session. Once that lands (commit subject will start with `agent1: wire wave 4 services into boot`), pull your next rebase and start extracting:

Create `app/services/block_index_loader.{h,c}`:

```c
struct block_index_load_result {
    bool          ok;
    int           num_loaded;
    int64_t       tip_height;
    struct uint256 tip_hash;
    enum bii_verdict sidecar_verdict;
    char          error[256];
};

struct block_index_load_result
block_index_loader_load(const char *datadir,
                         struct node_db *db,
                         struct block_map *out_map);

bool block_index_loader_save(const char *datadir,
                              const struct block_map *map);
```

Move every `block_index.bin` read/write call in `boot.c` into the new service, plus the `bii_verify()` call that I'll have just wired in. Replace the boot.c site with a single `block_index_loader_load(...)` call. Commit: `agent2: extract block_index_loader from boot.c`.

Tests in `lib/test/src/test_block_index_loader.c` — 6+ cases (happy path, sidecar missing, hash mismatch, empty file, truncated file, write round-trip).

### 4. Boot decomposition Phase B — `chain_state_validator`

Create `app/services/chain_state_validator.{h,c}` owning the cross-check pass between `block_map`, SQLite `blocks`, `coins_best_block`, and `active_chain`:

```c
enum csv_verdict {
    CSV_CONSISTENT,
    CSV_TIP_UNREACHABLE,      /* tip hash not in block_map */
    CSV_HEIGHT_MISMATCH,      /* block_map vs SQLite blocks differ */
    CSV_COINS_DRIFT,          /* coins_best_block not in block_map */
    CSV_ACTIVE_CHAIN_STALE,   /* active_chain tip differs from best */
};

enum csv_verdict csv_validate(struct node_db *db,
                               struct block_map *bm,
                               struct active_chain *ac,
                               struct coins_view_cache *coins_tip,
                               char *err_out, size_t err_cap);
```

This replaces the scattered consistency checks currently inlined in `boot.c`'s `validate_coins_chain_agreement` path. Call it after `block_index_loader_load` and before any write. On any verdict ≠ `CSV_CONSISTENT`, emit `EV_CHAIN_STATE_INVALID` and refuse to boot unless `ZCL_ALLOW_INCONSISTENT_CHAIN=1`.

Tests: one per verdict + happy path = 5+ tests.

### 5. Boot decomposition Phase C — `utxo_recovery_service`

Create `app/services/utxo_recovery_service.{h,c}` owning the wipe/recover decision logic that currently lives in `boot.c`'s `validate_coins_chain_agreement` REIMPORT/WIPE_WAIT branch:

```c
enum urs_action {
    URS_NOTHING,              /* UTXOs and tip agree */
    URS_RESTORE_TIP,          /* use max(utxo.height) as tip */
    URS_WIPE_UTXOS,           /* only if recovery_policy allows */
    URS_REFUSE_AMBIGUOUS,     /* neither is clearly right */
};

enum urs_action urs_decide(struct node_db *db,
                            int64_t tip_height,
                            const struct uint256 *tip_hash);

/* Executes the chosen action inside a DB_TXN_SCOPE. */
bool urs_execute(struct node_db *db,
                  enum urs_action action,
                  struct recovery_policy *policy);
```

This is the service that should have existed on 2026-04-10 when `boot.c` unconditionally wiped 1.3M UTXOs. Now it makes the decision explicitly and gates it through `recovery_policy`.

Tests: each action path + policy refusal path = 8+ tests.

**Target after Phases A/B/C: `boot.c` under 1400 lines.** Every extraction must leave `./test_zcl` green.

### 6. Mempool DoS hardening — `app/services/mempool_limits.{h,c}`

Today the mempool has no size cap, no eviction policy, no tx expiry. A malicious peer can exhaust memory by flooding low-fee txs.

```c
struct mempool_limits {
    int64_t max_bytes;              /* env ZCL_MEMPOOL_MAX_BYTES, default 300MB */
    int64_t max_tx_count;           /* env ZCL_MEMPOOL_MAX_TXS, default 50000 */
    int64_t expiry_seconds;         /* env ZCL_MEMPOOL_EXPIRY, default 72 hours */
    int64_t min_relay_fee_zatoshi;  /* env ZCL_MIN_RELAY_FEE, default 100 */
};

/* Called after every mempool_add. Evicts lowest-fee-per-byte txs
 * until we're under max_bytes and max_tx_count. */
int mempool_limits_enforce(struct txmempool *pool,
                            const struct mempool_limits *lim);

/* Called periodically (scheduler tick). Drops txs older than expiry. */
int mempool_limits_expire(struct txmempool *pool,
                           const struct mempool_limits *lim);
```

Wire into `lib/validation/src/txmempool.c` acceptance path and add a scheduler tick. Emit `EV_MEMPOOL_EVICT` / `EV_MEMPOOL_EXPIRE`.

Tests in `lib/test/src/test_mempool_limits.c` — 10+ cases (overflow eviction, expiry sweep, lowest-fee-first, exact-limit boundary, min-relay-fee rejection).

### 7. Disk space monitor — `app/services/disk_monitor.{h,c}`

A full disk is the second-most-common operational failure mode. Today nothing notices until SQLite starts returning errors.

```c
struct disk_monitor {
    const char *datadir;
    int64_t     warn_free_bytes;   /* env ZCL_DISK_WARN_BYTES, default 5GB */
    int64_t     refuse_free_bytes; /* env ZCL_DISK_REFUSE_BYTES, default 1GB */
    int         poll_seconds;      /* env ZCL_DISK_POLL, default 60 */
};

void disk_monitor_start(const struct disk_monitor *cfg);
void disk_monitor_stop(void);
int64_t disk_monitor_free_bytes(const char *path);
```

Background thread polls `statvfs()` every N seconds. Below `warn_free_bytes`: emit `EV_DISK_LOW`. Below `refuse_free_bytes`: emit `EV_DISK_CRITICAL` and tell the mempool to reject new txs, tell the block processor to pause new block acceptance.

Tests in `lib/test/src/test_disk_monitor.c` — 6+ cases (threshold triggers, recovery, stop/start, poll interval).

### 8. Database maintenance scheduler — `app/services/db_maintenance.{h,c}`

SQLite databases degrade without periodic VACUUM, ANALYZE, WAL checkpoint. Build a scheduler that runs these on a schedule tuned for the node's traffic pattern.

```c
struct db_maintenance_schedule {
    int wal_checkpoint_minutes;     /* default 15 */
    int analyze_hours;              /* default 24 */
    int vacuum_days;                /* default 7 */
};

void db_maintenance_start(struct node_db *db,
                           const struct db_maintenance_schedule *s);
void db_maintenance_stop(void);
bool db_maintenance_run_now(struct node_db *db,
                             const char *op);  /* "wal"|"analyze"|"vacuum" */
```

VACUUM holds the whole DB lock — only run it when the node is at-tip and idle (check sync state first). ANALYZE and WAL checkpoint are cheap and always OK.

Emit `EV_DB_MAINTENANCE_{START,DONE,FAILED}` with operation name and duration.

Tests in `lib/test/src/test_db_maintenance.c` — 6+ cases.

### 9. (Stretch) Snapshot exporter automation

`export_snapshot` is a separate binary today. Make it a service that auto-generates snapshots nightly if the node is at-tip, stores them in `~/snapshots/`, rotates (keep last 7), and publishes over the file service so other nodes can fetch via fast-sync. Big ticket — only attempt if items 1–8 are done.

### 10. (Stretch) Continue CSR migration

Drive the remaining ~56 unmigrated call sites to resolution. Most are test files or documented no-ops; audit each one and either migrate or add a `/* CSR-internal: low-level, bypass intentional */` comment with justification. Target: the appendix site count goes to **zero**.

---

## New coordination artifacts

### BOOT_QUEUE.md

When you want a `boot.c` edit, add an entry here instead of flagging it in a commit message. I batch-apply the queue once per wave cycle.

```
## Queue
- [ ] agent2: wire `wallet_backup_start(&g_wallet_backup_cfg, &g_wallet)` after `wallet_init` in boot.c:~1420
- [ ] agent2: replace `bii_verify` call site with the new `block_index_loader_load` result type
```

Create the file with a skeleton if it doesn't exist. Never commit a `boot.c` edit yourself.

### FUZZER_FINDINGS.md

One row per discovery. Format:

```
| ID | Location | Severity | Found by | Owner | Fix commit |
|----|----------|----------|----------|-------|------------|
| 1  | lib/primitives/src/transaction.c:451 | MED leak | agent2 fuzz_block run 2 | agent2 | agent2: fix ... |
```

Add the transaction leak as row 1 in the same commit as item #1 above (once fixed).

---

## Rules

- Rebase/pull before every session. Push direct to `master` after `./test_zcl` green.
- One commit per logical step. `agent2:` prefix.
- **Never touch** AGENT3 territory: `tools/mcp/**`, `lib/net/peer_scoring.*`, `lib/wallet/wallet_encrypted.*` (upcoming), `lib/util/log_json.*`, `lib/rpc/rpc_middleware.*` (upcoming), `app/models/database_validators.*`.
- **Never touch** `config/src/boot.c` unless I've signed off. Use `BOOT_QUEUE.md`.
- **Separate discovery from repair.** Fuzzer/crash-harness findings go in `FUZZER_FINDINGS.md`, fixes are separate commits.
- **Reach down for stretch.** When you finish the priority list, work items #9 and #10 in the same session. Don't wait for a new plan.
- **Trade work.** If you finish everything and AGENT3 is still grinding, take on anything from their plan in files you don't collide on. Test files are always safe.
- Update "Current Status" each session.

## Definition of done for wave 5

- [ ] Fuzzer finding #1 fixed + regression test
- [ ] `PHGR13_INVESTIGATION.md` with reproduction, diff, hypothesis, fix sketch
- [ ] `block_index_loader`, `chain_state_validator`, `utxo_recovery_service` extracted from boot.c
- [ ] `boot.c` under **1400 lines**
- [ ] `mempool_limits`, `disk_monitor`, `db_maintenance` services + tests
- [ ] `BOOT_QUEUE.md` and `FUZZER_FINDINGS.md` in use
- [ ] `./test_zcl` still green

---

## Current Status

*(Update each session. Keep it short.)*

- **2026-04-11 wave 1** — Phase 1 CSR + singleton + boot bootstrap + `process_block.c` (5 sites). Site count corrected to 65.
- **2026-04-11 wave 2** — `recovery_policy` + 21 tests; wipe gates on `sync_controller` (1) + `snapshot_sync_service` (3). `db_txn` service + 14 tests (unwired). CSR migrations continued. Full `./test_zcl` green.
- **2026-04-11 wave 3** — `db_txn` wired into 3 of 4 recovery paths; db_txn rollback verification tests landed.
- **2026-04-11 wave 3 session 1** — `chain_restore_execute` flagged N/A (in-memory only). `block_index_integrity` service landed (sidecar SHA3 + SQLite cross-check + quarantine) with 11 tests + `EV_BLOCK_INDEX_CORRUPT`. Boot wiring deferred to AGENT1.
- **2026-04-11 wave 4 session 1** — #1 done for real (chain_restore_execute DB_TXN_SCOPE via csr_instance). #3 done: `tools/crash_recovery_test.c` + `make test-crash`. #4 done: `tools/fuzz/fuzz_{block,script,p2p}.c` + `make fuzz` + `make fuzz-ci` + `make fuzz-ci-leaks`. **Fuzzer finding #1**: `transaction_deserialize` vin leak at `lib/primitives/src/transaction.c:451` filed for separate fix.
- **2026-04-11 wave 4 session 2** — #5 done: `wallet_backup_service` + 11 tests. Background pthread copies wallet_* tables via ATTACH + CREATE TABLE AS SELECT, round-trip row-count verify, rotation, refuses source-datadir backup, `wallet_backup_now()` exposed. Encryption hooks ready for phase 2. Boot wiring flagged for AGENT1.
- **2026-04-11 wave 4 session 3** — **Fuzzer finding #1 FIXED.** `transaction_alloc(_, _, 0)` used to leak a 1-byte calloc stub overwritten by `transaction_deserialize`. Zero-size now means NULL. Regression test added. All three fuzzers run **zero leaks, zero crashes** under ASAN+LSAN (395k + 1.47M + 38k execs each ~20s). `make fuzz-ci-leaks` green for the first time.
- **2026-04-11 wave 5 (AGENT1 COORDINATOR)** — **New plan, 10 deliverables above + new `BOOT_QUEUE.md` and `FUZZER_FINDINGS.md` artifacts.** Wave 5 items **already done**: #1 (fuzzer fix — landed in wave 4 session 3, nice). Remaining priority: **#2 PHGR13 investigation** (the consensus bug that's been staring at us since wave 1). #3/#4/#5 (boot decomposition) wait for AGENT1's boot.c wire-up session. #6–#8 (mempool_limits, disk_monitor, db_maintenance) are independent and can run any time. Reach for #9 (snapshot automation) and #10 (CSR migration to zero). Update `FUZZER_FINDINGS.md` to mark finding #1 as FIXED with the commit hash.
- **2026-04-11 wave 5 session 1** — Carry-over + items **#1**, **#7**, **#8** landed. **Wallet backup Phase 2** (wave 4 carry-over): `wallet_backup_encrypt_file`/`_decrypt_file` with PBKDF2-HMAC-SHA256 + ChaCha20-Poly1305 (rolled own AEAD wrapper because the shared `chacha20poly1305_encrypt` has a 2KB stack buffer too small for SQLite backup files), RFC-6070 PBKDF2 vectors, and 12 round-trip/tamper/wrong-password tests. **#1** FUZZER_FINDINGS.md row #1 flipped to FIXED with commit hash + accurate root cause (it was a zero-size calloc stub, not a vin walk). **#7** `disk_monitor` service: background statvfs poll + edge-triggered `EV_DISK_{LOW,CRITICAL,OK}` events + lock-free `disk_monitor_is_critical()` for the mempool/block hot path, 17 tests. **#8** `db_maintenance` scheduler: pthread ticks `wal_checkpoint`/`analyze`/`vacuum` on independent schedules with a caller-supplied vacuum gate, `run_now` synchronous entry point used by both scheduler and operators, 19 tests. Boot wiring for #7 (armed before first SQLite open) + #8 queued in `BOOT_QUEUE.md`. New user preference: **don't use `/tmp` for new test scratch files** — new tests write to `./test-tmp/<pid>_*` instead. `./test_zcl` green on every commit.
- **2026-04-11 wave 6 session 1** — Three wave-6 items landed: (a) **#6 mempool_limits** — `app/services/mempool_limits.{h,c}` with env-tunable caps, fee-per-byte eviction, expiry sweep, `EV_MEMPOOL_EVICT`/`EV_MEMPOOL_EXPIRE`, background pthread, idempotent start/stop. Seam into `lib/validation/src/txmempool.c`: new `tx_mempool_collect_views()` snapshot + new `tx_mempool_set_post_add_hook()` called unlocked after adds. 12 tests. (b) **addrman persistence robustness** — `app/services/addrman_integrity.{h,c}` mirrors `block_index_integrity`: 48-byte ADIX sidecar (magic+version+body_size+sha3) written next to `peers.dat` after atomic rename, verified on every load, quarantine-and-start-fresh on mismatch because peers.dat contents directly steer outbound peer selection. Wired into `connman_save_addrman` + `connman_load_addrman` (added `fsync` before rename while I was there). 11 tests. New `EV_ADDRMAN_CORRUPT` event. (c) **checkpoint enforcement audit** — finding: checkpoints ARE consulted in `contextual_check_block_header` and enabled by default. Extracted `checkpoints_hash_at_height` / `_last_height` / `_validate_header` helpers in `lib/chain/checkpoints.{h,c}`, refactored the inlined for-loop at `check_block.c:188` to call the new API, 8 unit tests in `test_chain.c` covering match/miss/reject/NULL-safety + a regression check that the mainnet chainparams are loaded with non-zero hashes. Boot wiring for mempool_limits queued in `BOOT_QUEUE.md`. `./test_zcl` green on every commit.
