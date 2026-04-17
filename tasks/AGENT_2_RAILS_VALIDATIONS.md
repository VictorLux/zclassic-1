# Agent 2 — Rails-grade Model Validations (Track M)

**Pick up when:** your `a2/database-halt-on-corruption` PR (D1 silent-full-DB-wipe kill) has merged and you've pulled. Don't stand down.

**Read first:** [`RAILS_PARITY_PLAN.md`](../RAILS_PARITY_PLAN.md), then `app/models/include/models/activerecord.h` (the macros you're extending), `app/models/src/database_validators.c` (the registry you're growing).

**Worktree:** `~/zclassic23-2`
**Branch:** `a2/rails-validations-macros` (first of three PRs on this track)
**Base:** `origin/master`
**Directive:** keep pushing to master.

---

## Mission, one sentence

Make every model-level validation Rails-grade: uniqueness, format (regex-free matchers), richer numericality, custom named validators, associations, and lifecycle split (before_create/update, after_commit/rollback), so silent invalid writes become impossible without adding a deliberate `// bypass-validation-ok:` marker.

---

## Scope (Track M from the plan)

### PR 1 — `a2/rails-validations-macros`

**Files:**
- `app/models/include/models/activerecord.h` — new macros + hook slots
- `app/models/include/models/ar_matchers.h` (new) — format matcher declarations
- `app/models/src/ar_matchers.c` (new) — matcher implementations
- `lib/test/src/test_ar_matchers.c` (new) — unit tests for every matcher
- `lib/test/src/test_activerecord.c` — extend with uniqueness / format / lifecycle tests

**Deliverables:**

1. **M1a — `validates_format_of`.** Macro signature:
   ```c
   validates_format_of(errors, record, field, matcher)
   ```
   where `matcher` is a `bool (*)(const void *, size_t)`. Shipped matchers (all in `ar_matchers.c`, all pure functions — no regex):
   - `ar_matches_hex32` / `ar_matches_hex64` — exact-length hex strings
   - `ar_matches_txid` — 32-byte binary or 64-hex accepted
   - `ar_matches_base58_taddr` — starts with `t1`/`t3`, base58check length
   - `ar_matches_bech32_zaddr` — `zs1…` bech32 sapling
   - `ar_matches_onion_v3` — 56-char + `.onion`
   - `ar_matches_ipv4` / `ar_matches_ipv6`
   - `ar_matches_name_chars` — 1–63 chars, `[a-z0-9-]`, no leading/trailing hyphen
   - `ar_matches_scriptpubkey` — non-empty, ≤10000 bytes, no NULs at start

2. **M1b — `validates_uniqueness_of`.** Macro signature:
   ```c
   validates_uniqueness_of(ndb, errors, record, field, table, column)
   ```
   Expands to a prepared `SELECT 1 FROM <table> WHERE <column>=? LIMIT 1`. On hit, `ar_errors_add(errors, #field, "has already been taken")`. Must still work with a SQL UNIQUE INDEX behind it (defence in depth — SQLite catches concurrent inserts we can't).

3. **M1c — numericality fillers.**
   - `validates_greater_than(errors, record, field, min)` (exclusive)
   - `validates_less_than(errors, record, field, max)` (exclusive)
   - `validates_only_integer(errors, value)` for runtime-string sources
   - `validates_inclusion_of_str(errors, record, field, choices, n_choices)` with `strcmp`

4. **M1d — `validates_associated`.** Macro:
   ```c
   validates_associated(ndb, errors, record, fk, parent_table, parent_column)
   ```
   Runs `SELECT 1 FROM <parent_table> WHERE <parent_column>=?`. Misses populate `ar_errors` with `"refers to missing <parent_table>"`.

5. **M1e — named custom validators (`validate :method`).** New struct:
   ```c
   struct ar_named_validator {
       const char *name;
       bool (*fn)(const void *record, struct ar_errors *errors);
   };
   ```
   Plus `AR_RUN_VALIDATORS(list, n_list, record, errors)` macro so each model's `validate()` is a table of named checks, not one monolith. Tests assert *which* named check failed.

6. **M4 — lifecycle split.** Extend `struct ar_callbacks`:
   ```c
   ar_before_cb before_create[AR_MAX_CALLBACKS];
   ar_before_cb before_update[AR_MAX_CALLBACKS];
   ar_after_cb  after_create[AR_MAX_CALLBACKS];
   ar_after_cb  after_update[AR_MAX_CALLBACKS];
   ar_after_cb  after_commit[AR_MAX_CALLBACKS];
   ar_after_cb  after_rollback[AR_MAX_CALLBACKS];
   int n_before_create, n_before_update, n_after_create, n_after_update;
   int n_after_commit, n_after_rollback;
   ```
   Route via whether an `_exists(pk)` check at save-time returns true. `after_commit` / `after_rollback` fire from `db_txn_commit` / `db_txn_rollback` / `db_txn_auto_rollback` — emit via a per-txn callback queue on the `db_txn` struct.

7. **Tests (`test_activerecord.c` additions):**
   - uniqueness: insert one row, attempt duplicate, assert `ar_errors` has `"has already been taken"`.
   - format: every matcher positive + negative case.
   - associated: parent missing → fails; parent present → passes.
   - lifecycle: forced rollback confirms `after_commit` did NOT fire and `after_rollback` did.
   - named validator: corrupted field hits the right `name` in `ar_errors`.

**Done when:** macros compile; every matcher has unit tests; `make ci` green; no caller changes (this PR is macros + tests only).

---

### PR 2 — `a2/rails-validations-apply`

**Files:**
- every `app/models/src/*.c` file with a `db_*_validate` function (15 models)
- `app/models/src/database.c` — add missing UNIQUE indexes where a validator exists but SQL didn't
- `lib/test/src/test_db_validators.c` — extend with property-based fuzz

**Deliverables:**

Apply the new macros to every model per plan M5 + M6. For each model:
- Add `validates_format_of` for every string field that carries consensus meaning.
- Add `validates_uniqueness_of` for every field with a UNIQUE INDEX.
- Add `validates_associated` for every documented `belongs_to` (see `activerecord.h:32-36`).

Fields to cover (from the plan):
- `wallet_keys.pubkey` (UNIQUE + hex64), `.address` (UNIQUE + t-addr)
- `blocks.hash` (UNIQUE + hex64), `.height` (UNIQUE)
- `utxos(txid,vout)` composite UNIQUE + `.txid` hex64
- `peers(ip,port)` UNIQUE + `.ip` format
- `onion_announcements.onion` UNIQUE + format
- `names.name` UNIQUE + name_chars
- `contacts.address` format
- `tx_index.txid` UNIQUE + hex64
- `mempool.txid` UNIQUE + hex64
- `store_orders.order_id` UNIQUE + format
- `zslp_tokens.token_id` UNIQUE + format

Associations (M6):
- `wallet_utxo → wallet_key` via pubkey
- `tx_index → block` via height
- `utxo → tx_index` via txid
- `sapling_note → sapling_key` via ivk
- `store_order → store_product` via product_id

`dependent_destroy` hooks fire `before_destroy` on parent → cascade delete children. Start with `wallet_key → wallet_utxo`, `block → tx_index`, `tx_index → utxo`.

**Tests:**
- extend `test_db_validators.c` with a fuzz loop: generate N random `struct db_<model>` records with bytes scrambled in one field; assert validator rejects them; assert no crash on any input.

**Done when:** every model in the plan's M5/M6 table has the validators applied; `make ci` green; new tests pass.

---

### PR 3 — `a2/rails-validations-lifecycle`

**Files:**
- `lib/util/include/util/result.h` — no change (already has `zcl_result`)
- every `app/models/src/*.c` — add `_save_r` / `_find_r` variants returning `zcl_result`
- `app/models/src/database.c` — add `updated_at` columns via migration + `ar_touch` helper
- `lib/test/src/test_after_commit_semantics.c` (new)
- `lib/test/src/test_timestamps.c` (new)

**Deliverables:**

1. **M3 — zcl_result rollout.** For every `db_<model>_save` / `_find_by_*`, add a `_r` variant returning `struct zcl_result`. Keep the `bool` variants for backwards compatibility — they wrap `_r` and drop the message. Priority order in the plan M3.

2. **M7 — timestamps.**
   - Add `updated_at INTEGER NOT NULL DEFAULT 0` column to: wallet_keys, utxos, peers, wallet_transactions, mempool, zslp_balances, store_orders, contacts. One schema migration block, idempotent.
   - Ship `ar_touch(record)` helper — sets `updated_at = now_seconds()` if the record has the field. Called from each model's `before_save` hook.
   - `validates_timestamps(record)` — enforces `created_at > 0`, `updated_at >= created_at` when both present.

3. **M4 integration test.** `test_after_commit_semantics.c`:
   - Insert row inside `db_txn_begin("t")`, register `after_commit` hook that sets a flag, commit → assert flag set.
   - Insert row, register `after_rollback` hook, rollback → assert the rollback flag set, commit flag NOT set.
   - Regression for the exact footgun: currently `AR_FINISH_SAVE` fires `after_save` synchronously, but if the enclosing txn rolls back, the hook already ran.

**Done when:** every model has `_save_r` + `_find_r`; `updated_at` populates automatically on save; `test_after_commit_semantics` asserts the correct commit/rollback firing semantics.

---

## Files you MUST NOT touch

- `app/controllers/src/*.c` — Agent 3's Track C.
- `app/services/src/*.c` — Agent 3's Track C.
- `tools/mcp/*` — Agent 3's Track C.
- `config/src/boot.c` — unless needed for after_commit wiring, in which case document carefully.

If you need a controller change to land a model change cleanly, leave a `// TODO(agent3): migrate to db_<model>_save_r` breadcrumb and keep the bool variant working.

---

## Done when all three PRs merge

- [ ] `make lint` green.
- [ ] `make ci` green.
- [ ] `db_validator_count() == count(CREATE TABLE)` in `database.c` (no orphans).
- [ ] Every model with a SQL UNIQUE INDEX has a paired `validates_uniqueness_of`.
- [ ] Every consensus-bearing string field has a `validates_format_of`.
- [ ] `test_after_commit_semantics` passes under forced rollback.
- [ ] `test_db_validators` fuzz: 10k random corruptions, zero crashes, >99% caught by validators.

---

## Hand-off (each PR)

```
cd ~/zclassic23-2
git push origin <branch>
gh pr create --title "<title>" --body "$(cat <<'EOF'
## Summary
See RAILS_PARITY_PLAN.md Track M, PR <N>.

## Plan
<3 bullets>

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

When all three merge, pull and request the next assignment. Do not stand down.
