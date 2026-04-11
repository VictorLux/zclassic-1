# AGENT2 — Chain State, Recovery & Database Hardening

**Worktree:** `~/zclassic23-2`
**Workflow:** push directly to `master` after `./test_zcl` passes
**Coordinator:** AGENT1 (main `~/zclassic23`)
**Peer:** AGENT3 (`~/zclassic23-3`) — working on MCP/models, different files

---

## Mission

Make every destructive operation in zclassic23 **fail closed**. The 2026-04-10 incident (1.3M UTXOs wiped) happened because multiple code paths had independent authority to delete data based on inconsistent metadata. The chain_state_repository closed the chain-tip side of that hole. This plan closes the remaining holes: call-site migration, the recovery policy layer, database transaction discipline, and the boot-time integrity checks that stop the node before it can hurt itself.

## Already done (don't redo)

- `app/services/src/chain_state_repository.{h,c}` — single-writer tip API with 9 rejection codes, 28 unit tests, concurrency coverage
- `csr_instance()` singleton + `boot.c` bootstrap
- `process_block.c` migrated (5 of 65 sites)
- AGENT2.md "Current Status" reflects the migration tally

## Next wave — ordered by impact

### 1. Finish Phase 1b call-site migration (60 sites remaining)

Same pattern as `process_block.c`. One commit per file. Build green after each.

**Priority order** (reordered by blast radius, not file count):

| Order | File                                           | Sites | Why it matters                                                          |
|-------|-----------------------------------------------|-------|--------------------------------------------------------------------------|
| 1     | `app/services/src/snapshot_sync_service.c`    | 5     | The historical snapshot path that caused the 2026-04-10 wipe             |
| 2     | `lib/validation/src/connect_block.c`           | 3     | Hot validation path; every reorg goes through here                       |
| 3     | `app/services/src/chain_restore_service.c`     | 1     | Boot-time restore — the last thing you want to be wrong                  |
| 4     | `config/src/boot_services.c` + `boot_index.c`  | 2 + 4 | AGENT1 shared — open a PR, I'll review                                   |
| 5     | `lib/net/src/msgprocessor.c`                   | 2     | Net-side tip updates during sync                                          |
| 6     | `app/controllers/src/blockchain_controller.c`  | 2     | RPC-driven state mutations                                                |
| 7     | `app/controllers/src/sync_controller.c`        | 1     | RPC-driven                                                                |
| 8     | `lib/coins/src/coins_view.c`                   | 2     | **Check first** — this is a lower layer CSR itself depends on. If it must bypass, document with a `/* Low-level: CSR-internal */` comment and leave it. |
| 9     | `app/models/src/database.c` + header           | 2     | Model layer — AGENT3 may be touching this soon, coordinate via commit timing |
| 10    | `config/src/boot.c` (27 sites)                 | 27    | **All AGENT1.** Don't touch — I'll do these in a series of review-first commits once you flag them in the appendix. |
| 11    | Test files (5 files, 8 sites)                  | 8     | Migrate last; may need a small `test_csr_helpers`                         |

### 2. Recovery Policy Layer — `app/services/recovery_policy.{h,c}`

The chain_state_repository stops tip *updates* that are inconsistent. It does **not** stop the UTXO wipes themselves. Build the policy layer so destructive operations ask permission:

```c
struct recovery_policy {
    int64_t max_utxo_wipe_rows;      /* default 1000  — env ZCL_MAX_UTXO_WIPE_ROWS */
    int64_t max_block_rollback;      /* default 100   — env ZCL_MAX_BLOCK_ROLLBACK */
    int64_t max_header_rewind;       /* default 1000  — env ZCL_MAX_HEADER_REWIND */
    bool    require_backup_verified; /* default false — env ZCL_REQUIRE_BACKUP_VERIFIED */
    bool    dry_run;                 /* default false — env ZCL_DRY_RUN */
    const char *operator_ack_file;   /* default /var/tmp/zcl-operator-ack */
};

enum policy_decision {
    POLICY_ALLOW,
    POLICY_REFUSE_TOO_LARGE,
    POLICY_REFUSE_NO_BACKUP,
    POLICY_REFUSE_DRY_RUN,
    POLICY_PROMPT_OPERATOR,  /* writes reason to operator_ack_file, waits for mtime change */
};

enum policy_decision policy_check_utxo_wipe(
    const struct recovery_policy *p,
    int64_t proposed_rows,
    const char *reason,        /* grep-able, e.g. "boot.validate_coins_chain_agreement.reimport" */
    struct event_bus *eb);

enum policy_decision policy_check_block_rollback(
    const struct recovery_policy *p,
    int64_t from_height,
    int64_t to_height,
    const char *reason,
    struct event_bus *eb);

void policy_load_from_env(struct recovery_policy *p);
```

Tests in `lib/test/src/test_recovery_policy.c` — at least 15 cases (each decision, env override, missing ack file, prompt-operator path with a fake ack file, concurrent calls).

Then wire it in front of the 7 `node_db_wipe_utxos()` call sites in `boot.c`. Since boot.c is AGENT1's pen, expose `policy_check_utxo_wipe()` in the header and I'll call it from my boot.c commits — your job is to build the policy module and its tests.

### 3. Database transaction discipline — `app/models/db_txn.{h,c}`

Every destructive multi-table operation today runs as individual SQL statements. A crash mid-sequence leaves partial state. Build a lightweight transaction wrapper:

```c
struct db_txn;

struct db_txn *db_txn_begin(struct node_db *db, const char *label);
bool           db_txn_commit(struct db_txn *txn);
void           db_txn_rollback(struct db_txn *txn);  /* idempotent */

/* RAII-style: if the txn is still open when the caller exits without
 * committing, rollback and log EV_DB_TXN_LEAKED. Implement via a
 * sentinel field + a wrapper macro. */
#define DB_TXN_SCOPE(db, label) \
    __attribute__((cleanup(db_txn_auto_rollback))) struct db_txn *_txn = db_txn_begin((db), (label))
```

Then wrap every destructive sequence in `snapshot_sync_service.c`, `chain_restore_service.c`, and `boot.c`'s recovery paths in a `DB_TXN_SCOPE`. One txn per logical recovery step, committed only if all cross-checks pass.

Tests in `lib/test/src/test_db_txn.c` — commit, rollback, leak detection, nested (reject nesting), concurrent txns on different databases.

### 4. Block-index integrity — `app/services/block_index_integrity.{h,c}`

`block_index.bin` is the single file most likely to betray the node. Give it a deterministic format with self-checks:

- **Magic + version + SHA3-256** over the body, verified on load
- **Reverse cross-check**: for the declared tip hash, look it up in the SQLite `blocks` table and verify `height` matches. This is exactly the bug class that nuked the UTXOs on 2026-04-10 — the on-disk index said `h=60` but SQLite said `h=3,073,476`.
- On mismatch, emit `EV_BLOCK_INDEX_CORRUPT` with both heights and **refuse to boot** unless `ZCL_ALLOW_CORRUPT_INDEX=1` is set. Better to fail loudly at boot than to hand a bad tip to the wipe paths.
- **Don't auto-delete the file on corruption** — rename it to `block_index.bin.corrupt.<timestamp>` so an operator (or AGENT1) can inspect.

Tests: simulate corruption in each field (magic, version, hash, tip entry), verify the refusal behavior, verify the rename-on-corrupt path.

### 5. Boot decomposition (stretch, only after 1–4 land)

`config/src/boot.c` is **2,640 lines**. Once the above are in place, extract these services (I'll pair with you on the boot.c edits):

- `block_index_loader.{h,c}` — owns `block_index.bin` load and validation (uses #4)
- `chain_state_validator.{h,c}` — the cross-check pass before any tip mutation
- `utxo_recovery_service.{h,c}` — orchestrates wipe/recover decisions via `recovery_policy` (uses #2)

Target: reduce `boot.c` to **<800 lines** — pure orchestration, zero business logic.

---

## Rules (unchanged)

- Rebase/pull before every session. Push directly to `master` when `./test_zcl` is green.
- One commit per logical step. `agent2:` prefix. Clean reason strings in event emissions.
- **Never touch** `tools/mcp_server.c`, `tools/mcp/**`, or `app/models/*` validators (AGENT3's pen).
- **Never touch** `config/src/boot.c` unless AGENT1 has signed off (singleton bootstrap was the one exception).
- If a migration reveals that a call site can't supply a required field, flag it in the commit message — don't fake a value.
- Update "Current Status" each session.

## Definition of done for this wave

- [ ] All 65 chain-tip call sites migrated through `csr_commit_tip()`
- [ ] `recovery_policy` service + tests, 7 wipe sites behind the gate
- [ ] `db_txn` wrapper + all destructive recovery paths inside a scope
- [ ] `block_index_integrity` validation on load + refusal-to-boot on mismatch
- [ ] `./test_zcl` still green — now with 28 + new policy/txn/integrity tests
- [ ] A deliberately-corrupted `block_index.bin` causes a clean boot refusal, not a UTXO wipe

---

## Current Status

*(Update each session. Keep it short.)*

- **2026-04-11 wave 1** — Phase 1 service + singleton + boot bootstrap + `process_block.c` (5/65 sites) + 3 singleton tests. Site count corrected to 65 real sites (2 false hits in appendix).
- **2026-04-11 wave 2** — **New plan, five deliverables above.** Start with priority 1: `snapshot_sync_service.c` (5 sites). That's the path the 2026-04-10 incident ran through — migrating it is the highest ROI move on the list.
