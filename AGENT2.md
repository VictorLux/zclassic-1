# AGENT2 — Disaster Recovery, Fuzz Testing & Boot Decomposition

**Worktree:** `~/zclassic23-2`
**Workflow:** push directly to `master` after `./test_zcl` passes
**Coordinator:** AGENT1 (main `~/zclassic23`)
**Peer:** AGENT3 (`~/zclassic23-3`) — MCP/security/observability, different files

---

## Mission

The safety rails are now in place — `chain_state_repository` validates tip mutations, `recovery_policy` caps destructive operations, `db_txn` gives scoped transactions, the validator registry fails bad rows. **Wave 3 proves the rails actually work under stress.** Kill the node at random points during sync and see if it recovers. Throw garbage at the block parser until something crashes. Pull the 2,640-line `boot.c` apart so it stops being a landmine.

## Already done (don't redo)

- `chain_state_repository` — 9 CSR call sites migrated, rejection codes cover stale index + UTXO drift
- `recovery_policy` — env tunables, 4 wipe sites gated (3 in snapshot_sync, 1 in sync_controller)
- `db_txn` — RAII scope wrapper, 5 events, 14 tests. **Still unwired in production paths.**
- `process_block.c` (5 sites), `snapshot_sync_service.c` (2 tip sites + 3 wipe gates), `chain_restore_service.c` (1 site), `msgprocessor.c` (1 site)

## Wave 3 — ordered by impact

### 1. Wire `db_txn` into destructive recovery paths (carry-over)

The wrapper is sitting unused. Start here — it's small, high-impact, and unblocks everything else.

**Target paths** (each gets one `DB_TXN_SCOPE` wrapping the destructive sequence):

- `app/services/src/snapshot_sync_service.c::snapsync_begin_receive` — drop-utxos + seed-chain writes must be atomic. Label: `"snapsync.begin_receive"`.
- `app/services/src/snapshot_sync_service.c::snapsync_finalize` — final UTXO batch writes + commitment update. Label: `"snapsync.finalize"`.
- `app/services/src/chain_restore_service.c::chain_restore_execute` — tip + header rewrite. Label: `"chain_restore.execute"`.
- `app/controllers/src/sync_controller.c::import_utxos_reimport` — the path gated by `policy_check_utxo_wipe`. Label: `"sync_controller.import_utxos_reimport"`.

For each: commit on success; let the cleanup handler roll back on any error path. Verify via a new test that an induced mid-sequence failure rolls back cleanly (write to an in-memory sqlite, abort halfway, assert no partial rows).

### 2. Block-index integrity — `app/services/block_index_integrity.{h,c}` (carry-over, critical)

This is the single largest remaining gap in boot-time safety. `block_index.bin` can still silently disagree with SQLite, and that's exactly what caused the 2026-04-10 incident.

**Format** (use a sidecar `block_index.bin.sha3` for backward compat):

```c
/* 48 bytes at the start of block_index.bin.sha3: */
struct block_index_sidecar {
    uint8_t  magic[4];         /* "BIIX" */
    uint32_t version;          /* 1 */
    uint64_t body_size;        /* size of block_index.bin at write time */
    uint8_t  body_sha3[32];    /* SHA3-256 of block_index.bin contents */
};

enum bii_verdict {
    BII_OK,
    BII_SIDECAR_MISSING,       /* first-run after upgrade — warn, accept */
    BII_SIDECAR_STALE,         /* body_size differs */
    BII_HASH_MISMATCH,         /* corruption or truncation */
    BII_TIP_HEIGHT_MISMATCH,   /* block_map tip hash → SQLite height disagrees */
    BII_TIP_MISSING_IN_SQL,    /* block_map tip hash not in SQLite `blocks` */
};

enum bii_verdict bii_verify(const char *datadir, struct node_db *db,
                             const struct block_index *declared_tip,
                             char *err_out, size_t err_cap);

bool bii_write_sidecar(const char *datadir);
void bii_quarantine_corrupt(const char *datadir, enum bii_verdict v);
```

**Quarantine behavior**: on `BII_HASH_MISMATCH` or `BII_TIP_*_MISMATCH`, rename both files to `block_index.bin.corrupt.<unix_ts>` and `block_index.bin.sha3.corrupt.<unix_ts>` — **do not delete**. Emit `EV_BLOCK_INDEX_CORRUPT` with the verdict and heights.

**Boot behavior**: refuse to boot unless `ZCL_ALLOW_CORRUPT_INDEX=1` is set. Better to fail loudly than to hand a stale tip to code that can wipe UTXOs.

Tests: `lib/test/src/test_block_index_integrity.c` — simulate each verdict (missing, stale size, hash flip, height mismatch, SQLite-missing), verify quarantine rename, verify refuse-to-boot.

**Boot.c wiring**: expose `bii_verify()` in the header. I'll call it from `boot.c` in a follow-up commit — you build the service + tests only.

### 3. Crash recovery test harness — `tools/crash_recovery_test.c`

The recovery policy, db_txn, and CSR are all defensive mechanisms we *believe* work. Prove it by forking the node and SIGKILLing it at random points, then rebooting and asserting integrity.

```
tools/crash_recovery_test.c:
  1. Start zclassic23 with a small isolated datadir
  2. Drive it through a sync scenario (via addnode or pre-staged data)
  3. Sleep N ms where N is a uniformly random value
  4. SIGKILL the process
  5. Restart the process
  6. Run `zcl_dataintegrity`, `zcl_utxocommitment`, `zcl_getblockcount`
  7. Assert: no UTXO count decrease, no tip regression, commitment matches
  8. Loop 100 iterations with different delays
  9. Report pass/fail distribution
```

Run it against a pre-seeded `~/.zclassic-c23-crashtest` datadir so iterations are fast. Add a `make test-crash` target.

This is the test that would catch a regression where, say, someone refactored `db_txn` to skip a rollback on SIGTERM and we lose recovery safety.

### 4. Block / script / P2P message fuzz harness — `tools/fuzz/*.c`

libFuzzer-compatible entry points for the three biggest attack surfaces:

- `tools/fuzz/fuzz_block.c` — `check_block()` / `contextual_check_block_header()` on the input bytes
- `tools/fuzz/fuzz_script.c` — `eval_script()` on a random script + stack
- `tools/fuzz/fuzz_p2p.c` — `msg_process()` on a random net message buffer

Build target: `make fuzz` produces `fuzz_block`, `fuzz_script`, `fuzz_p2p` binaries linked against `-fsanitize=fuzzer,address,undefined`. Seed corpuses from `lib/test/fuzz_seeds/*.bin` — include a few real mainnet blocks and p2p captures so the fuzzer starts from realistic inputs.

Add a `make fuzz-ci` target that runs each fuzzer for 60 seconds with a small timeout. CI never discovers new bugs with zero-second fuzzing; 60s × 3 = 3 minutes of cheap coverage.

Don't panic over existing bugs the fuzzer finds — just file them under the `Current Status` "known issues" section and fix the most dangerous ones in Wave 4.

### 5. Boot decomposition — `boot.c` to <1000 lines (carry-over, stretch)

Once items 1–4 land, tackle `boot.c` (currently **2,640 lines**). Extract these services, one commit each. Each extraction must leave `./test_zcl` green:

- `app/services/block_index_loader.{h,c}` — owns `block_index.bin` + sidecar load, calls `bii_verify()`
- `app/services/chain_state_validator.{h,c}` — the pre-boot cross-check pass (SQLite tip vs in-memory tip vs coins_best)
- `app/services/utxo_recovery_service.{h,c}` — orchestrates wipe/recover decisions via `recovery_policy`
- `app/services/wallet_bootstrap_service.{h,c}` — the wallet-side boot steps (key load, scan height reconciliation)

Each extraction: move functions into the new file, declare the entry point in the header, replace the boot.c call site with the single entry point, add service-level tests. I'll review each PR before you push to master.

Target: `boot.c` to orchestration only, **<1000 lines**, zero business logic.

---

## Rules

- Rebase/pull before every session. Push direct to `master` after `./test_zcl` green.
- One commit per logical step. `agent2:` prefix.
- **Never touch** AGENT3 territory: `tools/mcp/**`, `app/models/database_validators.*`, any `tools/mcp/controllers/*`.
- **Boot.c edits** for wiring (`bii_verify` call site, service extractions) — open one commit at a time and ping me in the commit message with `(AGENT1 review)` in the subject. I'll merge if it's clean.
- When the crash-recovery harness finds a real bug, log it in Current Status under "known issues" — don't silently fix it in the same commit. Separate bug discovery from bug repair.
- Update "Current Status" each session.

## Definition of done for wave 3

- [ ] All 4 destructive recovery paths wrapped in `DB_TXN_SCOPE`
- [ ] `block_index_integrity` service + tests, boot refuses on mismatch (AGENT1 wires the boot.c call site)
- [ ] `tools/crash_recovery_test` + `make test-crash` target, passes 100 iterations
- [ ] `tools/fuzz/fuzz_{block,script,p2p}` + `make fuzz-ci`, runs for 60s each in CI without crashing
- [ ] `config/src/boot.c` under **1000 lines**
- [ ] `./test_zcl` still green on every commit

---

## Current Status

*(Update each session. Keep it short.)*

- **2026-04-11 wave 1** — Phase 1 CSR + singleton + boot bootstrap + `process_block.c` (5 sites). Site count corrected to 65.
- **2026-04-11 wave 2** — `recovery_policy` + 21 tests; wipe gates on `sync_controller` (1) + `snapshot_sync_service` (3). `db_txn` service + 14 tests (unwired). CSR migrations continued (snapshot_sync 2, chain_restore 1, msgprocessor 1). Full `./test_zcl` green.
- **2026-04-11 wave 3** — **New plan, five deliverables above.** Start with #1 (wire `db_txn` into the four recovery paths). Then #2 (block_index_integrity) since that's the remaining hole between boot and the wipe gates. #3 and #4 (crash harness + fuzzers) can run in parallel once #2 lands.
- **2026-04-11 wave 3 session 1** — #1 done: wired `db_txn` into `snapsync_begin_receive`, `snapsync_finalize` (both paths), and `sync_controller.import_utxos_reimport`. `chain_restore_execute` flagged as not applicable — it only mutates in-memory state via `csr_commit_tip` and does no SQLite writes, so a db_txn wrap would be a cosmetic no-op (flagged for AGENT1 in commit message). Added 2 rollback-verification tests that exercise real row-level rollback via `node_db_state_set`. #2 done: new `block_index_integrity` service (sidecar SHA3 + SQLite cross-check + quarantine) with 11 tests covering every verdict, plus `EV_BLOCK_INDEX_CORRUPT` event. Boot wiring deferred to a follow-up commit for AGENT1 review. `./test_zcl` green on every commit.
