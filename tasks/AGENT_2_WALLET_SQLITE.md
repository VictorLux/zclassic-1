# Agent 2 — `wallet_sqlite.c` Result Types & Root-Cause Fix

## STATUS: MERGED (2026-04-17)

Branch `a2/wallet-sqlite-result-types` was reviewed and merged. Master has your 6 commits `52480abef..ccf0a6a11`. Root cause (wave-10 `wallet_watch_only` missing from `SCHEMA[]`) found and fixed.

**Next task:** [`AGENT_2_PERSISTENCE_HARDENING.md`](./AGENT_2_PERSISTENCE_HARDENING.md). Persistence silent-error cleanup across `database.c`, `utxo_commitment.c`, `coins_view_sqlite.c`, `dbwrapper.c` + the two observability nits from this PR's review. Same bug class you just fixed, wider blast radius. Mostly independent of Agent 3; one deliverable (kill silent full-DB wipe) waits on A3's state-halt primitive.

---

**Read first:** [`WALLET_PERSISTENCE_PLAN.md`](../WALLET_PERSISTENCE_PLAN.md). This doc assumes you have.

**Worktree:** `~/zclassic23-2`
**Branch:** `a2/wallet-sqlite-result-types`
**Base:** `origin/master`
**Parallel peer:** Agent 3 in `~/zclassic23-3` on branch `a3/wallet-controller-guardrails`. You do not coordinate directly; the interface in plan §5 is the contract.

---

## Mission, one sentence

Make `lib/wallet/src/wallet_sqlite.c` fail loudly with rich error types, find and fix the silent `wallet_sqlite_open` failure on rhett's node, and prove the fix with a round-trip integration test.

---

## Scope

### Files you own (edit freely)

- `lib/wallet/src/wallet_sqlite.c`
- `lib/wallet/include/wallet/wallet_sqlite.h`
- `lib/util/include/util/result.h`  **(create)**
- `lib/util/src/result.c`  **(create)**
- `app/models/src/wallet_key.c`
- `app/models/include/models/wallet_key.h`
- `app/models/src/wallet_sapling_key.c` (same migration, same pattern)
- `lib/test/src/test_wallet_persistence_cycle.c`  **(create)**
- `lib/test/src/test_wallet_sqlite_open_errors.c`  **(create)**
- `lib/test/CMakeLists.txt` / `lib/test/Makefile` — wire in new tests
- `Makefile` top-level — if adding new util source, ensure it builds
- `config/src/boot.c` — **one surgical change only**: update the call site of `wallet_sqlite_open` to consume `struct zcl_result` instead of `bool`. Do NOT restructure boot state machine (that is Agent 3's scope).

### Files you MUST NOT touch

- `app/controllers/src/wallet_controller.c` (Agent 3)
- `app/controllers/src/wallet_diagnostic_controller.c` (Agent 3)
- `app/services/src/wallet_backup_service.*` (Agent 3)
- `lib/wallet/src/wallet_canary.c` / `.h` — Agent 3 creates
- Anything under `core/`, `db/`, `explorer/`, `mvc/` unless grepping reveals a caller of `wallet_sqlite_open` / `wallet_sqlite_flush` / `wallet_sqlite_write_key` / `wallet_sqlite_read_keys` you must migrate.

If you hit a caller in a file not listed above, migrate it with a minimal diff (signature change + `ZCL_CHECK` or explicit `if (!r.ok) { LOG_FAIL(...); return; }`) and add a line in your PR description explaining.

---

## Deliverables

### D1. `lib/util/result.h` + `result.c`

Per plan §5.1. Implement:

- `struct zcl_result` (fields as specified).
- `zcl_result_make(int code, const char *file, int line, const char *fmt, ...)` — fills `message` via `vsnprintf`, truncating safely to 255 bytes + NUL.
- `#define ZCL_OK` constant.
- `#define ZCL_ERR(code, fmt, ...)` macro invoking `zcl_result_make`.
- `#define ZCL_CHECK(expr)` short-circuit that `LOG_FAIL`s and returns the non-ok result.

Unit test: `lib/test/src/test_zcl_result.c` — message populated, source_file/line correct, truncation at 255 chars.

### D2. Migrate `wallet_sqlite.c` to `zcl_result`

Per plan §5.2.

1. Rewrite the public API signatures. Keep data behaviour identical (same SQL, same invariants, same INSERT OR REPLACE semantics).
2. Replace every `return false` / `return true` with appropriate `ZCL_OK` or `ZCL_ERR(WSQL_*, "%s", ...)`.
3. Every `sqlite3_prepare_v2` / `sqlite3_step` / `sqlite3_exec` failure returns a distinct `WSQL_*` code with the `sqlite3_errmsg(db)` concatenated into the message.
4. Migrate internal `sqlite3_step` calls to `AR_STEP_DONE` / `AR_STEP_ROW` per `DEFENSIVE_CODING.md §1`. If a specific internal remains raw, top-file `#define ZCL_AR_RAW_SQL` is acceptable only with a comment explaining which lines it protects and why a model wrapper isn't appropriate.
5. Add `wallet_sqlite_self_test` per plan §5.2.
6. Add `wallet_sqlite_read_single_key` per plan §5.2.
7. Add `wallet_sqlite_get_health` per plan §5.2.

**Invariant checks in `wallet_sqlite_write_key`:**
- Reject if `pk->size` not in {33, 65}.
- Reject if `key->vch` is all zeros OR `key` is invalid.
- Reject if `hash160(pk) != pubkey_get_id(pk)`.
- Return `WSQL_INVARIANT_*` on any such.

### D3. Migrate `wallet_key` model to AR conventions

`app/models/src/wallet_key.c` currently has `db_wallet_key_delete` but no `_save`. Add:

```c
struct zcl_result db_wallet_key_save(struct node_db *ndb,
                                     const struct pubkey *pk,
                                     const struct privkey *key);
```

Use `AR_BEGIN_SAVE` / `AR_FINISH_SAVE` per `DEFENSIVE_CODING.md §1`. Register `before_save` validator: pubkey/privkey consistency. `after_save` is a no-op in Agent 2's scope; Agent 3 will register a backup-service hook later (that is a register-additional-callback operation, not a replacement).

### D4. Diagnose the silent `wallet_sqlite_open` failure

Reproduction path (on rhett's node):
1. `systemctl --user restart zclassic23`
2. Once booted, `grep "New wallet created\|Wallet loaded" ~/.zclassic-c23/node.log | tail -5`

Currently prints `New wallet created` every boot. After your fix: either prints `Wallet loaded: <N> keys, ...` OR the daemon aborts with STATE_D per plan §7.

**Your job:** with the new rich errors wired up, run the reproduction. Note the specific `WSQL_*` code. Fix the underlying cause.

Hypotheses to investigate (narrow by logging, don't guess):
- `g_node_db.open` is false at the call site (check init order in boot.c — `node_db_open` vs `wallet_sqlite_open`).
- One of the `sqlite3_prepare_v2` calls fails because a statement references a table that doesn't exist yet (schema init order).
- The `wallet_keys` schema migration in `app/models/src/database.c:148` hasn't run before `wallet_sqlite_open` tries to prepare `SELECT ... FROM wallet_keys`.

Document the root cause in your commit message and in a one-paragraph comment at the top of `wallet_sqlite_open` so a future reader understands why the order matters.

### D5. Tests

- `lib/test/src/test_wallet_persistence_cycle.c` per plan §8.1.
- `lib/test/src/test_wallet_sqlite_open_errors.c` per plan §8.1.
- `lib/test/src/test_zcl_result.c` from D1.
- Each test uses `:memory:` sqlite where possible to stay fast.
- All must pass under `./test_zcl` after `make -j$(nproc)`.

### D6. `boot.c` call-site swap

One-line-ish diff in `config/src/boot.c` where `wallet_sqlite_open(&g_wallet_sqlite, g_node_db.db)` is called. New code:

```c
struct zcl_result open_r = wallet_sqlite_open(&g_wallet_sqlite, g_node_db.db);
if (!open_r.ok) {
    fprintf(stderr,
        "wallet_sqlite_open failed: code=%d %s (%s:%d)\n",
        open_r.code, open_r.message, open_r.source_file, open_r.source_line);
    /* Do NOT regenerate keypool here. Agent 3's boot state machine will
     * decide abort vs. proceed based on whether wallet_keys has rows. */
}
if (open_r.ok) {
    wallet_sqlite_read_keys(&g_wallet_sqlite, &g_wallet);
    ...
}
```

Do not alter the `num_keys == 0` branch or the keypool generation logic. Agent 3 rewrites that region into a state machine.

---

## Done when

- [ ] `git log --oneline` on `a2/wallet-sqlite-result-types` shows a clean sequence (5–15 commits, logical units).
- [ ] `make lint` green (no raw `sqlite3_step` in `wallet_sqlite.c` without `ZCL_AR_RAW_SQL` + comment).
- [ ] `make ci` green locally (1572+ existing tests + new ones).
- [ ] New tests run under `./test_zcl` and all pass.
- [ ] A comment at the top of `wallet_sqlite_open` names the root cause in one paragraph.
- [ ] `node.log` on a test boot prints either `Wallet loaded: N keys` OR a STATE_D-style abort with a specific `WSQL_*` code. Not `New wallet created.` on a node that has a non-empty `wallet_keys` table.
- [ ] Commit message for the diagnosis commit includes: "Root cause: <specific reason>" and "Reproduces on: <command>".
- [ ] PR description links `WALLET_PERSISTENCE_PLAN.md §2` (the bug) and §9 (acceptance).
- [ ] PR title: `a2: wallet_sqlite result types, root-cause fix, persistence cycle test`

---

## Commit style

- Small commits. One logical change each.
- Subject line imperative: `migrate wallet_sqlite_open to zcl_result`, not `migrated` or `migrating`.
- Body explains *why*, not *what* (the diff shows what).
- Include `Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>` on the final commits (follow the project's existing convention — see `git log --oneline` for recent patterns).

Suggested commit sequence:

1. `add zcl_result type per DEFENSIVE_CODING §2`
2. `wallet_sqlite: migrate open() to zcl_result`
3. `wallet_sqlite: migrate read_keys/write_key to zcl_result`
4. `wallet_sqlite: migrate flush() to zcl_result`
5. `wallet_sqlite: add self_test canary + get_health`
6. `wallet_sqlite: add invariant checks on write_key`
7. `wallet_key model: add db_wallet_key_save via AR_BEGIN_SAVE`
8. `boot: wire new wallet_sqlite_open signature`
9. `test: wallet persistence cycle + sqlite open errors`
10. `wallet_sqlite: fix root cause of silent open failure (state description)`

---

## Gotchas

- `AR_BEGIN_SAVE` and friends live in `app/models/include/models/activerecord.h`. Library code in `lib/` should NOT include that header; model layer code in `app/models/` does. Keep layering clean.
- The existing `wallet_sqlite_flush` calls have NO transaction error handling: `sqlite3_exec(ws->db, "BEGIN", NULL, NULL, NULL);` — errors silently ignored. Migrate these to check return and emit `WSQL_TXN_BEGIN_FAIL` / `WSQL_TXN_COMMIT_FAIL`.
- Encrypted-at-rest path (`is_wks1_blob`) must still work — don't break it. Existing tests in `test_wallet_sqlite_enc.c` cover this. Your changes must keep them passing.
- If `wallet_sqlite_flush` is called while `ws->open == false` today, it returns false. Keep that invariant: return `ZCL_ERR(WSQL_DB_NOT_OPEN, ...)`.
- The current `wallet_sqlite_flush` writes keys even if validation would reject them (it just iterates the keystore). Your migration can tighten this — iterate, validate via model layer, skip invalid with WARN log.

---

## Hand-off

When complete:

```
cd ~/zclassic23-2
git push origin a2/wallet-sqlite-result-types
gh pr create --title "a2: wallet_sqlite result types, root-cause fix, persistence cycle test" \
             --body "$(cat <<'EOF'
## Summary
Implements Agent 2 scope of WALLET_PERSISTENCE_PLAN.md.

- zcl_result type per DEFENSIVE_CODING.md §2
- wallet_sqlite.c migrated from bare bool to rich errors
- Root cause of silent wallet_sqlite_open failure: <fill in>
- Fix: <fill in>
- Tests: test_wallet_persistence_cycle, test_wallet_sqlite_open_errors, test_zcl_result

## Plan
See WALLET_PERSISTENCE_PLAN.md §5, §8, §9.

## Test plan
- [x] make lint
- [x] make ci
- [x] ./test_zcl (new tests pass)
- [x] node.log on test node shows "Wallet loaded: N keys" not "New wallet created"

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

Stop there. Do not merge. Claude will review, request changes if needed, and merge.
