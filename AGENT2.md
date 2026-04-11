# AGENT2 — Boot Integrity, Crash Safety & Wallet Resilience

**Worktree:** `~/zclassic23-2`
**Workflow:** push directly to `master` after `./test_zcl` passes
**Coordinator:** AGENT1 (main `~/zclassic23`)
**Peer:** AGENT3 (`~/zclassic23-3`) — MCP/security/observability, different files

---

## Mission

Three layers of safety already landed (CSR → recovery_policy → db_txn). **Wave 4 makes the node self-healing.** Give `block_index.bin` a tamper-evident sidecar so boot fails loud instead of silently wiping UTXOs. Build a crash test harness that actually SIGKILLs the node mid-sync and proves it recovers. Stand up libFuzzer harnesses on the three biggest attack surfaces. Then build the wallet backup service that memory file `feedback_never_destroy_wallet.md` has been screaming for since the 0.4 ZCL loss.

## Already done (don't redo)

- **CSR** — 9 call sites migrated; `process_block.c`, `snapshot_sync_service.c`, `chain_restore_service.c`, `msgprocessor.c`
- **recovery_policy** — 4 wipe sites gated (3 in snapshot_sync, 1 in sync_controller)
- **db_txn** — wrapper + tests landed; **3 of 4** destructive paths now in `DB_TXN_SCOPE` (only `chain_restore_service.execute` remains)
- AGENT2 Current Status reflects the session-by-session log

## Wave 4 — ordered by impact

### 1. Finish the last `db_txn` wiring (quickest win)

One remaining path: `app/services/src/chain_restore_service.c::chain_restore_execute`. Wrap its destructive sequence (tip + header rewrite) in `DB_TXN_SCOPE("chain_restore.execute", ...)`. Add an induced-failure test that aborts the fn mid-sequence and asserts no partial rows.

This closes the db_txn wiring story and means every destructive recovery path is now transactional.

### 2. Block-index integrity — `app/services/block_index_integrity.{h,c}` (critical carry-over)

This is the single largest remaining gap in boot-time safety. `block_index.bin` can still silently disagree with SQLite, which is exactly the bug that wiped 1.3M UTXOs on 2026-04-10.

**Sidecar format** (new file: `block_index.bin.sha3`, 48 bytes):

```c
struct block_index_sidecar {
    uint8_t  magic[4];         /* "BIIX" */
    uint32_t version;          /* 1 */
    uint64_t body_size;        /* bytes of block_index.bin at write time */
    uint8_t  body_sha3[32];    /* SHA3-256 of block_index.bin contents */
};

enum bii_verdict {
    BII_OK,
    BII_SIDECAR_MISSING,       /* first-run after upgrade — warn, accept */
    BII_SIDECAR_STALE,         /* body_size differs from measured */
    BII_HASH_MISMATCH,         /* corruption or truncation */
    BII_TIP_HEIGHT_MISMATCH,   /* block_map tip hash → SQLite height disagrees */
    BII_TIP_MISSING_IN_SQL,    /* block_map tip hash absent from SQLite blocks */
};

enum bii_verdict bii_verify(const char *datadir,
                             struct node_db *db,
                             const struct block_index *declared_tip,
                             char *err_out, size_t err_cap);
bool bii_write_sidecar(const char *datadir);
void bii_quarantine_corrupt(const char *datadir, enum bii_verdict v);
```

**Semantics:**
- On `BII_HASH_MISMATCH` or `BII_TIP_*_MISMATCH`: rename both `block_index.bin` and `block_index.bin.sha3` to `*.corrupt.<unix_ts>` — **never delete**. Emit `EV_BLOCK_INDEX_CORRUPT` with verdict + heights.
- On `BII_SIDECAR_MISSING`: emit warning event, write a fresh sidecar using current body, continue boot.
- Boot wiring (I'll handle the `boot.c` call site): refuse to boot on any non-OK verdict unless `ZCL_ALLOW_CORRUPT_INDEX=1`. Loud failure > silent destruction.

Tests in `lib/test/src/test_block_index_integrity.c`: simulate each verdict (missing, stale size, hash flip, height mismatch, SQLite-missing), verify quarantine rename, verify refuse-to-boot path via a harness that checks exit code.

Expose `bii_verify()` and `bii_write_sidecar()` in the header. Don't touch `boot.c` — I'll wire the call site in a follow-up commit you can `(AGENT1 review)` flag in your commit message.

### 3. Crash recovery test harness — `tools/crash_recovery_test.c`

The three safety layers are untested under real crash conditions. Prove they work:

```
1. Seed an isolated datadir with a small real chain (~200 blocks)
2. Start zclassic23 with that datadir
3. Drive it through a recovery scenario (snapshot import, reorg, UTXO rebuild)
4. sleep N ms where N is uniformly random in [0, 5000]
5. SIGKILL -9 the process
6. Restart
7. Run zcl_dataintegrity, zcl_utxocommitment, zcl_getblockcount
8. Assert:
   - zcl_getblockcount did NOT decrease
   - zcl_utxocommitment matches the count before the crash OR before the previous commit
   - zcl_dataintegrity passes
9. Loop 100 iterations with different random delays
10. Report pass/fail distribution with delay histogram
```

Add a `make test-crash` target. Run it against `~/.zclassic-c23-crashtest`. Use the MCP stdio protocol directly (not HTTP RPC) so the test is self-contained.

**Expected outcome:** you'll find at least one bug. Log discoveries in Current Status. Fix the most dangerous one in a separate commit. The less-dangerous ones get issues under "known issues from crash fuzzer".

### 4. libFuzzer harnesses — `tools/fuzz/*.c`

libFuzzer entry points for the three biggest attack surfaces:

- `tools/fuzz/fuzz_block.c` — `check_block()` / `contextual_check_block_header()` on raw block bytes
- `tools/fuzz/fuzz_script.c` — `eval_script()` on a random script + random stack state
- `tools/fuzz/fuzz_p2p.c` — `msg_process()` on a random P2P message buffer

**Build**: new Makefile target `fuzz` producing `fuzz_block`, `fuzz_script`, `fuzz_p2p`, all linked with `-fsanitize=fuzzer,address,undefined`. Use `$(CC) -g -O1` not `-O3` for sanitizer sanity.

**Seeds**: `lib/test/fuzz_seeds/block/*.bin`, `lib/test/fuzz_seeds/script/*.bin`, `lib/test/fuzz_seeds/p2p/*.bin`. Seed with 3–5 real mainnet samples per corpus — the fuzzer finds far more bugs starting from realistic inputs.

**CI target**: `make fuzz-ci` runs each fuzzer for 60 seconds with `-timeout=1 -max_total_time=60`. Zero new bug discoveries in 60s is fine — we're looking for already-latent crashes, not exhaustive coverage.

Document any discovered bugs in Current Status under "known issues from fuzzers". Do **not** fix them in the same commit — separate discovery from repair.

### 5. Wallet backup service — `app/services/wallet_backup_service.{h,c}`

Memory file `feedback_never_destroy_wallet.md` is explicit: the user has already lost 0.4 ZCL to wallet destruction. The fix is an always-on, always-external, always-versioned backup.

```c
struct wallet_backup_config {
    const char *backup_dir;        /* default ~/wallet_backups */
    int         interval_seconds;  /* default 3600 (hourly) */
    int         max_versions;      /* default 168 (1 week hourly) */
    bool        encrypt;           /* default false until phase 2 */
    const char *encrypt_password;  /* from env WALLET_BACKUP_PASSWORD if encrypt */
};

/* Starts a background thread that every `interval_seconds`:
 *   1. Acquires a read lock on the wallet SQLite DB
 *   2. Copies wallet_keys, wallet_tx, wallet_utxo tables into a
 *      `wallet_backup_<ISO8601>.sqlite` file in `backup_dir`
 *   3. (optional) Encrypts to AES-256-GCM with scrypt-derived key
 *   4. Verifies the backup round-trips (read keys, compare counts)
 *   5. Rotates: deletes the oldest when count > max_versions
 *   6. Emits EV_WALLET_BACKUP with path, size, key_count, duration_ms
 * Failures emit EV_WALLET_BACKUP_FAILED and DO NOT crash the node.
 */
bool wallet_backup_start(struct wallet_backup_config *cfg,
                          struct wallet *w);
void wallet_backup_stop(void);
bool wallet_backup_now(void);  /* force an immediate backup */
```

Integration:
- Start in `boot.c` after `wallet_init` (I'll wire the call site — you build the service)
- Expose `zcl_wallet_backup_now` via AGENT3's MCP surface (coordinate via commit message — AGENT3 can add the tool wrapper in a follow-up)
- If `backup_dir` doesn't exist, create it with mode 0700
- Refuse to start if the datadir is the same as `backup_dir` (don't back up to yourself)

Tests: `lib/test/src/test_wallet_backup.c` — 10+ cases covering interval trigger, rotation, round-trip verify, backup-to-missing-dir, backup-failure event emission, force-now, concurrent stops.

### 6. Boot decomposition (stretch)

`config/src/boot.c` is still **2,640 lines** with 8 `node_db_wipe_utxos` call sites. Once items 1–5 land, we can start decomposing. I'll pair on the boot.c edits. Your part: extract these services (one commit each):

- `app/services/block_index_loader.{h,c}` — owns `block_index.bin` + sidecar load, calls `bii_verify()`
- `app/services/chain_state_validator.{h,c}` — the pre-boot cross-check pass
- `app/services/utxo_recovery_service.{h,c}` — orchestrates wipe decisions via `recovery_policy` + `db_txn`

Target: `boot.c` < **1000 lines**, zero business logic.

---

## Rules

- Rebase/pull before every session. Push direct to `master` after `./test_zcl` green.
- One commit per logical step. `agent2:` prefix.
- **Never touch** AGENT3 territory: `tools/mcp/**`, `lib/net/peer_scoring.*`, `lib/test/src/test_mcp_*.c`, `lib/test/src/test_secrets_hygiene.c`, `app/models/database_validators.*`.
- **Never touch** `config/src/boot.c` unless I've signed off. Expose service entry points via headers and I'll wire them.
- **Separate discovery from repair.** When the fuzzer or crash harness finds a bug, log it in Current Status and fix it in a separate commit with its own `agent2: fix` prefix.
- Update "Current Status" each session.

## Definition of done for wave 4

- [ ] `chain_restore_service.execute` wrapped in `DB_TXN_SCOPE`
- [ ] `block_index_integrity` service + 5+ tests, boot refuses on any non-OK verdict (AGENT1 wires)
- [ ] `crash_recovery_test` passes 100 iterations, bug discoveries logged
- [ ] `fuzz_block` + `fuzz_script` + `fuzz_p2p` each run 60s in CI without crashing
- [ ] `wallet_backup_service` + tests, backups created every hour, rotated at 168 versions
- [ ] `config/src/boot.c` under **1500 lines** (stretch: 1000)
- [ ] `./test_zcl` still green on every commit

---

## Current Status

*(Update each session. Keep it short.)*

- **2026-04-11 wave 1** — Phase 1 CSR + singleton + boot bootstrap + `process_block.c` (5 sites). Site count corrected to 65.
- **2026-04-11 wave 2** — `recovery_policy` + 21 tests; wipe gates on `sync_controller` (1) + `snapshot_sync_service` (3). `db_txn` service + 14 tests (unwired). CSR migrations continued (snapshot_sync 2, chain_restore 1, msgprocessor 1). Full `./test_zcl` green.
- **2026-04-11 wave 3** — `db_txn` wired into 3 of 4 recovery paths (`snapsync_begin_receive`, `snapshot_sync.finalize`, `sync_controller.import_utxos_reimport`); db_txn rollback verification tests landed.
- **2026-04-11 wave 3 session 1** — Wave 4 items #1 and #2 **already done**: `chain_restore_execute` flagged as not applicable (it only mutates in-memory state via `csr_commit_tip`, does no SQLite writes, db_txn wrap would be cosmetic — flagged in commit for AGENT1 review). `block_index_integrity` service landed (sidecar SHA3 + SQLite cross-check + quarantine) with **11 tests** covering every verdict, plus `EV_BLOCK_INDEX_CORRUPT` event. Boot wiring deferred to a follow-up commit for AGENT1 review. `./test_zcl` green on every commit.
- **2026-04-11 wave 4 (AGENT1 COORDINATOR)** — **Plan landed.** You've already finished #1 and #2 (nice). Next targets per the plan below: **#3 crash recovery harness**, **#4 libFuzzer harnesses**, **#5 wallet backup service** (memory file `feedback_never_destroy_wallet.md` has been asking for this since the 0.4 ZCL loss — the plan's most user-impact-positive item). #6 boot decomposition is stretch.
