# Agent 5 — Persistence Silent-Error Cleanup (incl. R2.1 P0)

**Read first:** [`HARDENING_CHECKLIST.md`](../HARDENING_CHECKLIST.md) §P2.1–P2.3, §P2.6, §R2.1, §R2.5, §R2.10 and the wallet plan.

**Worktree:** `~/zclassic23-5`
**Branch:** `a5/persistence-silent-errors`
**Base:** `origin/master`
**Dependencies:** Agent 2's `lib/util/result.h`. Stub it locally (one-header copy of the fields from §5.1 of `WALLET_PERSISTENCE_PLAN.md`) until A2 merges, then rebase.

---

## Mission, one sentence

Kill the "silent self-heal into data loss" pattern at its two worst sites — the DB quarantine path and the coins_view atomicity gap — and make every other unchecked SQLite mutation loud.

---

## Priority

**R2.1 is P0.** Do it first. Its failure mode is identical to the wallet bug: on boot, the node silently wipes *everything* (UTXOs, wallet keys, block index, schema) and starts fresh. That is unacceptable. The rest of this agent's scope is P1.

---

## Scope

### Files you own

- `app/models/src/database.c` — quarantine path, schema migration helpers, unchecked `sqlite3_exec` / `node_db_exec` sites, IBD turbo-mode PRAGMAs
- `lib/coins/src/utxo_commitment.c`
- `lib/storage/src/coins_view_sqlite.c` — flush atomicity
- `lib/storage/src/dbwrapper.c` — LevelDB checksum default
- `lib/test/src/test_db_quarantine_halt.c` **(create)**
- `lib/test/src/test_coins_view_atomicity.c` **(create)**
- `lib/test/src/test_db_migration_idempotent.c` **(create)**

### Files you MUST NOT touch

- `lib/wallet/src/wallet_sqlite.c` or `app/models/src/wallet_key.c` (agents 2/3).
- `lib/validation/src/process_block.c`, `app/services/src/snapshot_sync_service.c`, `lib/validation/src/txmempool.c`, `lib/storage/src/disk_block_io.c` (agent 6).
- `Makefile`, systemd units (agent 4).
- `tools/mcp/*`, `lib/net/*`, `tools/wal_checkpoint.c` (agent 7).
- `lib/sapling/*` (agent 8).

---

## Deliverables

### D1 (P0). Kill silent full-DB wipe in `database.c:533-542`

Current code:

```c
if (!db_quick_check_ok(ndb->db)) {
    fprintf(stderr, "db: %s is malformed; rebuilding fresh SQLite state\n", path);
    sqlite3_close(ndb->db);
    ndb->db = NULL;
    db_quarantine_files(path);
    if (!db_open_raw(&ndb->db, path)) { …return false; }
}
/* falls through to create_schema → empty tables */
```

New behavior:

1. `db_quick_check_ok` detects corruption → return `ZCL_ERR(DB_ERR_CORRUPT, "quick_check failed on %s", path)`. **Do not quarantine automatically.** Do not create fresh schema.
2. Caller in `config/src/boot.c` (the node startup flow) must interpret `DB_ERR_CORRUPT` as STATE_D-style halt: log the quarantine filename the operator *would* need, but do not move files. Wait for explicit operator recovery.
3. Introduce a new CLI flag `-rebuild-fresh-db` that is the *only* way the node will produce an empty DB in place of a corrupt one. Flag must log a giant warning and preserve a `.corrupt-<ts>` copy before wiping.
4. If `db_quick_check_ok` fails on *table*-level corruption but `wallet_keys` is readable, emit it to a recovery file `$datadir/wallet_keys.recovery.json` (same format `dumpprivkey` produces) before halting. This gives the operator a direct recovery path without the node needing to guess.
5. `boot.c` may be edited minimally for the STATE_D halt + the flag parse. Do NOT restructure the boot flow (agent 3's scope).

**Regression test** (`test_db_quarantine_halt.c`): create a tiny sqlite DB, fuzz-flip some bytes, open with `node_db_open`, assert it returns `DB_ERR_CORRUPT` without creating or moving files; assert `wallet_keys.recovery.json` exists when the `wallet_keys` table was readable pre-corruption.

### D2. Migrate `database.c` unchecked `sqlite3_exec` / `node_db_exec` sites

Verified sites (line numbers may shift as you edit):

| Line(s) | Operation | Required change |
|---|---|---|
| 720-727 | `CREATE TABLE IF NOT EXISTS schema_migrations` | Check return; on failure, LOG_FAIL + return DB_ERR_MIGRATION |
| 883-889 | v6 `ALTER TABLE` + `CREATE INDEX` with "ignore errors" comment | Check return; tolerate only `SQLITE_ERROR` + `duplicate column` substring; log everything else |
| 893 | `node_db_state_set(ndb, "schema_version", ...)` return ignored | Check return; halt if schema_version cannot be persisted |
| 1399 | `sqlite3_exec(DB_DROP_INDEXES[i])` | Check + LOG_ERR |
| 1407 | `sqlite3_exec(DB_CREATE_INDEXES[i])` | Check + LOG_ERR |
| 1414-1417 | IBD turbo-mode PRAGMAs | Check each; if any fail, refuse turbo mode and fall back to safe defaults |

Every migrated site must preserve behavior on success and fail loud on failure. Prefer a helper:

```c
static int db_exec_checked(sqlite3 *db, const char *sql, const char *context) {
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        LOG_ERR("db", "%s: %s (sql=%s)", context, err ? err : "?", sql);
        sqlite3_free(err);
    }
    return rc;
}
```

### D3. Remove `strstr` ALTER/INDEX suppression (P2.3)

`database.c:268-276` skips any schema entry matching `"ALTER TABLE"` or `"CREATE INDEX"`. Rip it out. Any such statement must live in a numbered migration block (`if (current_ver < N)`) and run once.

### D4. `utxo_commitment.c` silent false returns (P2.2)

Lines 195, 211, 307, 322 — every `sqlite3_prepare_v2` that returns non-OK currently returns `false` with no context. Convert to `LOG_FAIL("utxo_cmt", "prepare %s: %s", sql, sqlite3_errmsg(db))` style. UTXO commitment bugs cause silent FlyClient disagreement; they must be loud.

### D5. `coins_view_sqlite` flush atomicity (R2.5)

`lib/storage/src/coins_view_sqlite.c:283-501` — the batch write path.

1. Move the `coins_best_block` write *inside* the `SAVEPOINT`, before `RELEASE`.
2. Add a boot-time integrity check in `coins_view_sqlite_open`:
   - `SELECT MAX(height) FROM utxos` vs the stored `coins_best_block`.
   - If `MAX(height) > coins_best_block`, **halt** with `DB_ERR_TIP_MISMATCH` (do not auto-wipe; memory rule).
   - If `MAX(height) < coins_best_block`, log + halt same way (UTXO missing above recorded tip).
3. Log both numbers on every successful flush so operators can correlate across restarts.

**Regression test** (`test_coins_view_atomicity.c`): open DB, write a batch that advances tip + adds UTXOs, `SIGKILL` the writer via fork/kill between savepoint-release and best-block update (use a test-only `ZCL_COINS_VIEW_KILL_AFTER_SAVEPOINT` env hook — behind `#ifdef ZCL_TESTING`), restart, assert halt with `DB_ERR_TIP_MISMATCH` (not silent continuation, not wipe).

### D6. LevelDB checksums on-by-default (R2.10)

`lib/storage/src/dbwrapper.c:73-78` currently sets `leveldb_readoptions_set_verify_checksums(opts, 0)`. Flip default to `1`. Expose an off-switch behind a new CLI flag `-leveldb-no-verify-checksums` for performance experiments; emit a one-time WARN log when the flag is active.

### D7. Migration idempotency test

`test_db_migration_idempotent.c`: open DB, run migrations, close, re-open (no change), assert `node_db_schema_version` returned the same value both times; assert `CREATE INDEX` failures from re-runs do not surface as errors (because they are gated by `IF NOT EXISTS`).

---

## Done when

- [ ] `db_quick_check_ok` failure halts the node with `DB_ERR_CORRUPT`, never creates fresh schema silently.
- [ ] `-rebuild-fresh-db` flag is the only code path that produces an empty DB in place of a corrupt one.
- [ ] Every `sqlite3_exec` and `node_db_exec` call in `database.c` checks its return value and either halts or logs + continues per D2 table.
- [ ] `strstr` suppression gone.
- [ ] `coins_best_block` write is inside the savepoint; boot-time tip-vs-max-height check halts on mismatch.
- [ ] LevelDB checksums on by default.
- [ ] 3 new tests pass under `./test_zcl`.
- [ ] `make lint` still green.
- [ ] PR title: `a5: persistence silent errors — kill silent wipe + coins atomicity`

---

## Gotchas

- Your D1 change depends on `boot.c` knowing how to handle `DB_ERR_CORRUPT`. Coordinate minimally with Agent 3's STATE_D state machine — reuse its abort reason enum if it already exists on `origin/master` by the time you start.
- Do NOT conflate R2.1 (silent full-wipe) with the legitimate operator-gated `utxo_recovery_service.c` path (which is *correctly* gated by `ZCL_MAX_UTXO_WIPE_ROWS` + operator prompt). The difference is explicit consent. Preserve the recovery service.
- IBD turbo-mode PRAGMA `synchronous=OFF` is performance-critical for initial sync. Keep the PRAGMA; just check its return. If it fails, log loudly but continue — unlike a corrupt DB, a failed PRAGMA is not data-losing.
- SQLite's `ALTER TABLE ... ADD COLUMN` returns `SQLITE_ERROR` with "duplicate column name: X" message when the column exists. Whitelist that exact substring in D2's v6 handling to preserve current tolerance.

---

## Hand-off

```
cd ~/zclassic23-5
git push origin a5/persistence-silent-errors
gh pr create --title "a5: persistence silent errors — kill silent wipe + coins atomicity" \
             --body "$(cat <<'EOF'
## Summary
Implements HARDENING_CHECKLIST.md §P2.1-P2.3, §R2.1 (P0), §R2.5, §R2.10.

- database.c:533-542 silent full-wipe replaced with DB_ERR_CORRUPT halt
- -rebuild-fresh-db is the ONLY opt-in for a fresh empty DB
- wallet_keys.recovery.json emitted on table-level corruption
- 7 unchecked sqlite3_exec sites in database.c fixed
- utxo_commitment.c silent false returns replaced with LOG_FAIL
- coins_view_sqlite: coins_best_block moved inside savepoint, boot-time tip check
- LevelDB checksums on by default, off requires -leveldb-no-verify-checksums
- 3 new regression tests

## Plan
See HARDENING_CHECKLIST.md §P2, §R2.1, §R2.5, §R2.10.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```
