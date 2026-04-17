# Agent 2 — Persistence Hardening (follow-up to wallet_sqlite migration)

**STATUS: PARTIALLY MERGED** — D2–D6 shipped in `7a955c0dd` (+ bonus node_health_service silent-return-1 fix that unblocked A3's D8). **D1 is still open.** A3's boot state machine (`8c54d100e`) is now on master, so D1 is unblocked — hook into the same `exit(1)` + structured-diagnostic pattern A3 used in `config/src/boot.c:515-527`. No named halt primitive exists; inline the print-and-exit, then when A2+A3 want a shared primitive that's a separate scope.

**Next after D1 ships:** [`AGENT_2_CONSENSUS_ATOMICITY.md`](./AGENT_2_CONSENSUS_ATOMICITY.md) (I will prep it when D1 lands).

**Directive: keep pushing to master.** Do not stand down between assignments.

**Read first:** [`WALLET_PERSISTENCE_PLAN.md`](../WALLET_PERSISTENCE_PLAN.md), [`HARDENING_CHECKLIST.md`](../HARDENING_CHECKLIST.md) §P2.1–P2.3, §R2.1, §R2.5, §R2.10, plus your own prior PR (merged as `52480abef..ccf0a6a11`).

**Worktree:** `~/zclassic23-2` (same one you just shipped from)
**Branch:** `a2/database-halt-on-corruption` (fresh branch for D1; the old `a2/persistence-hardening` is merged)
**Base:** `origin/master`
**Parallel peer:** Agent 3 has just merged `a3/wallet-controller-guardrails` and is picking up `a3/build-ci-deploy-hardening`. You are not blocked on them.

---

## Mission, one sentence

Kill the "silent self-heal into data loss" pattern in the rest of the persistence layer — same bug class you just eliminated in `wallet_sqlite` — and close the two observability nits from your own prior PR.

---

## Why you and not a fresh agent

You just shipped `zcl_result` and the `wallet_sqlite` migration. You know where the silent paths live; you know the `LOG_FAIL` + `ZCL_ERR` idiom; you know the `AR_STEP_*` layering rules. This work is exactly adjacent to what you just did, so ramp cost is near zero.

---

## Ordering — do not wait for A3

Directive: **keep pushing to master**. Don't stall on Agent 3.

Do D2, D3, D4, D5, D6 in the order below, then D1. One branch, one PR when ready. If A3's halt primitive (`node_state_halt` or equivalent) has landed by the time you get to D1, use it. If not, define a minimal local version (`static void db_halt(const char *reason) { fprintf(stderr, "DB HALT: %s\n", reason); _exit(2); }`) and let A3's fancier version supersede in a follow-up. Do not block.

If your PR is too large for a single review, split at your discretion — but each PR you open should stand on its own and target master directly.

---

## Scope

### Files you own

- `app/models/src/database.c` — schema migration helpers, quarantine path, IBD turbo PRAGMAs
- `lib/coins/src/utxo_commitment.c`
- `lib/storage/src/coins_view_sqlite.c` — flush atomicity, boot-time tip check
- `lib/storage/src/dbwrapper.c` — LevelDB checksum default
- `lib/wallet/src/wallet_sqlite.c` — D6 observability nits only
- `lib/test/src/test_db_quarantine_halt.c` **(create, D1)**
- `lib/test/src/test_db_migration_idempotent.c` **(create, D2)**
- `lib/test/src/test_coins_view_atomicity.c` **(create, D4)**

### Files you MUST NOT touch

- `app/controllers/src/wallet_*` (Agent 3)
- `app/services/src/wallet_backup_service.*` (Agent 3)
- `lib/wallet/src/wallet_canary.c` (Agent 3)
- `config/src/boot.c` — only D1 may touch it, and only minimally to wire the corruption-halt into Agent 3's state machine

---

## Deliverables

### D1 (P0, **ONLY REMAINING DELIVERABLE**). Kill silent full-DB wipe in `database.c:533-542`

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
1. Return a `struct zcl_result` with `code = DB_ERR_CORRUPT` (add to a new enum) from `node_db_open_r` — a rich-error open similar to your `wallet_sqlite_open_r`.
2. **Do not** quarantine or open fresh schema automatically.
3. Before returning, best-effort dump any readable rows of `wallet_keys` to `$datadir/wallet_keys.recovery.json` (same format `dumpprivkey` produces). This is a cheap insurance against edge-case corruption that only touches some tables.
4. `boot.c` must route this into the same abort pattern A3 uses for STATE_D/E/F at `config/src/boot.c:515-527` — structured diagnostic with code/message/source, then `exit(1)`. A3 did not introduce a named `node_state_halt` primitive; follow their inline pattern. If you want to factor a shared helper, do that in a separate PR.
5. New flag `-rebuild-fresh-db`: the only opt-in path that produces an empty schema in place of a corrupt one. Print a large banner warning when active; preserve the `.corrupt-<ts>` copy.

**Regression test** (`test_db_quarantine_halt.c`): create a small sqlite DB with known rows, flip random bytes to induce quick_check failure, open with `node_db_open_r`, assert `DB_ERR_CORRUPT` without file moves; assert `wallet_keys.recovery.json` exists when `wallet_keys` was readable pre-corruption.

### D2. Migrate `database.c` unchecked `sqlite3_exec` / `node_db_exec` sites — DONE (`7a955c0dd`)

Line references may shift as you edit; confirm each before committing.

| Lines | Operation | Fix |
|---|---|---|
| 720-727 | `CREATE TABLE IF NOT EXISTS schema_migrations` | Check return; `LOG_FAIL` on failure |
| 883-889 | v6 `ALTER TABLE` + `CREATE INDEX` (comment says "ignore errors") | Check return; tolerate only when errmsg contains `duplicate column name`; log anything else |
| 893 | `node_db_state_set(ndb, "schema_version", …)` return ignored | Check + halt if version persist fails |
| 1399 | `sqlite3_exec(DB_DROP_INDEXES[i])` | Check + LOG_ERR |
| 1407 | `sqlite3_exec(DB_CREATE_INDEXES[i])` | Check + LOG_ERR |
| 1414-1417 | IBD turbo PRAGMAs | Check each; if any fail, abort turbo mode and use safe defaults |

Helper:
```c
static int db_exec_checked(sqlite3 *db, const char *sql, const char *where) {
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) LOG_ERR("db", "%s: %s (sql=%s)", where, err ? err : "?", sql);
    sqlite3_free(err);
    return rc;
}
```

Also remove the `strstr(... "ALTER TABLE") || strstr(... "CREATE INDEX")` suppression around line 268-276; migrate the affected schemas to versioned migration blocks.

### D3. `utxo_commitment.c` silent false returns — DONE (`7a955c0dd`)

Lines 195, 211, 307, 322 — `if (sqlite3_prepare_v2(...) != SQLITE_OK) return false;` with no log. Replace each with `LOG_FAIL("utxo_cmt", "prepare %s: %s", sql, sqlite3_errmsg(db))`. Pattern is mechanical.

### D4. `coins_view_sqlite` flush atomicity (R2.5) — DONE (`7a955c0dd`)

`lib/storage/src/coins_view_sqlite.c` — the batch-write path wraps UTXO writes in a `SAVEPOINT` but the `coins_best_block` pointer is updated after `RELEASE SAVEPOINT`.

1. Move the `coins_best_block` update *inside* the savepoint, before release.
2. Add a boot-time integrity check in `coins_view_sqlite_open`: `SELECT MAX(height) FROM utxos` vs stored `coins_best_block`. Mismatch (either direction) → return `DB_ERR_TIP_MISMATCH`. **Do not auto-heal.** The memory rule is absolute: never wipe UTXOs above tip — halt and let the operator decide.
3. Log both values on every successful flush (`[coins] flush ok: max_height=X tip=Y utxos=N`).

**Regression test** (`test_coins_view_atomicity.c`): under `#ifdef ZCL_TESTING`, expose a `ZCL_COINS_VIEW_KILL_AFTER_SAVEPOINT` env hook that forces a `_exit(137)` between savepoint release and best-block update in a child process. Parent forks, child kills itself mid-write, parent reopens the DB and asserts `DB_ERR_TIP_MISMATCH` (not silent continuation, not wipe).

### D5. LevelDB checksums on by default — DONE (`7a955c0dd`)

`lib/storage/src/dbwrapper.c:73-78` — `leveldb_readoptions_set_verify_checksums(opts, 0)`. Flip to `1`. Expose `-leveldb-no-verify-checksums` flag for performance experiments; emit a one-time WARN when set.

### D6. Your own two observability nits — DONE (`7a955c0dd`)

From the review of your merged PR:

1. `lib/wallet/src/wallet_sqlite.c::wallet_sqlite_read_keys_r` — the `if (!pk_data || pk_len < 33 || !priv_data || priv_len < 32) continue;` silently drops a malformed row. Log it: `fprintf(stderr, "[wallet_sqlite] read_keys: skipping malformed row pk_len=%d priv_len=%d\n", pk_len, priv_len);` before `continue`.

2. `wallet_sqlite_flush_r` — when `n_key_fail > 0`, the code returns an aggregate error with only the first failure. Add a per-failure WARN log inside the loop so operators see every bad key, not just one.

Bundle these two into a single "observability" commit. Trivial diff.

---

## Done when

- [ ] `make lint` green.
- [ ] `make ci` green.
- [ ] One or more PRs targeting `master`, each self-contained. Don't wait on Agent 3 — stub their halt primitive locally if needed and let them supersede in a follow-up.
- [ ] `db_quick_check_ok` failure no longer creates fresh schema; halt + `.recovery.json` dump instead.
- [ ] `coins_best_block` is inside the savepoint; boot-time max-height check halts on mismatch.
- [ ] LevelDB checksums on by default.
- [ ] Tests: `test_db_quarantine_halt`, `test_coins_view_atomicity`, `test_db_migration_idempotent` all pass under `./test_zcl`.

---

## Gotchas

- **D1 boot.c interaction.** Agent 3's PR introduces a halt/abort primitive for STATE_D/E/F. Look for it on master at rebase time — do not duplicate.
- **Turbo-mode PRAGMAs** (`synchronous=OFF`, `cache_size=-524288`, `wal_autocheckpoint=0`) are performance-critical for IBD. On PRAGMA failure, fall back to safe defaults and continue — don't abort boot just because the *optimisation* failed. This is different from D1: D1 is about *integrity*, this is about *performance*.
- **LevelDB checksum flip** may reveal historical corruption. If master node shows new errors after this ships, they were there all along — investigate, don't revert.
- **`coins_best_block` is a hot path.** Benchmark before/after: moving it into the savepoint should not regress IBD throughput (both writes are already batched). If it does, the issue is upstream of this PR.
- **Recovery file format** in D1 must round-trip through `importprivkey` exactly. Reuse whatever `dumpprivkey` emits; don't invent a new format.

---

## Hand-off

```
cd ~/zclassic23-2
git push origin a2/persistence-hardening
gh pr create --title "a2: persistence silent-error cleanup + observability nits" \
             --body "$(cat <<'EOF'
## Summary
Phase 1 of HARDENING_CHECKLIST.md §P2.1-P2.3, §R2.5, §R2.10 plus the
two observability nits from the prior a2 PR review.

- database.c: every sqlite3_exec / node_db_exec now checked + logged
- strstr ALTER/INDEX suppression removed (D2)
- utxo_commitment.c: four silent-false returns now LOG_FAIL with sqlite errmsg (D3)
- coins_view_sqlite: coins_best_block moved inside savepoint; boot-time tip check halts on mismatch (D4)
- LevelDB checksums on by default, off requires -leveldb-no-verify-checksums (D5)
- wallet_sqlite: read_keys_r logs skipped rows; flush_r logs per-key failures (D6)

D1 (kill silent full-DB wipe) ships in a follow-up PR after Agent 3's
boot state machine lands.

## Plan
See HARDENING_CHECKLIST.md §P2.1-P2.3, §R2.5, §R2.10.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

When this PR merges, pull master and request the next assignment — don't stand down.
