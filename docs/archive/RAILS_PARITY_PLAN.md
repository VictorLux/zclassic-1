# Rails Parity Plan — ZClassic23 ActiveRecord + Validations

**Goal:** close the gap between our current AR scaffolding and Rails-grade persistence so every model, every write, every RPC endpoint, and every boot path is validated, logged, and rollback-safe by construction — not by calling-code discipline.

**Status of the groundwork (already shipped — keep):**
- `app/models/include/models/activerecord.h` — 11 `validates_*` macros, before/after_validate, before/after_save, before/after_destroy, async after hooks, CRUD macro set (AR_ADHOC_SAVE, AR_CACHED_DESTROY, AR_QUERY_*), router with global + per-route filters.
- `app/models/src/database_validators.c` — 19 tables registered with `db_register_all_validators()`.
- `app/models/src/db_txn.c` — scoped transactions with RAII auto-rollback + `EV_DB_TXN_LEAKED` instrumentation.
- `lib/util/src/result.c` — `struct zcl_result` with code/message/source_file/source_line.
- 10 models have `before_save` hooks wired (block, peer, wallet_tx, wallet_utxo, tx_index, mempool, file_service, contact, store_product, store_order, zslp_token, zslp_balance).

**Audit (2026-04-17) — gaps vs Rails:**

| Area | Rails | Us | Gap |
|---|---|---|---|
| validates_uniqueness_of | yes | **no** | Need DB-querying macro with UNIQUE-index fallback |
| validates_format_of (regex) | yes | **no** | Need `validates_format_of` + matchers (hex, base58, bech32, t-addr, z-addr, onion, IP, txid, name) |
| validates_numericality_of | yes | partial | Have min/max/non_negative/positive; missing greater_than, less_than, only_integer |
| validates_inclusion_of (string) | yes | partial | Have int version only |
| validates_associated | yes | **no** | No cascade validation to belongs_to parent |
| custom `validate :method` | yes | **no** | Single monolithic validate() per model; no registry of named checks |
| before_create vs before_update | yes | **no** | Only `before_save` (can't distinguish) |
| after_commit / after_rollback | yes | **no** | `after_save` fires even if commit rolls back |
| dirty tracking (changed?, was) | yes | **no** | No snapshot-on-load |
| named scopes | yes | **no** | Ad-hoc `db_*_where_*` functions per model |
| find_or_create_by | yes | **no** | Each model rolls its own |
| dependent: :destroy | yes | **no** | No FK cascade — manual delete chains |
| timestamps (created_at/updated_at) | yes | partial | ~4 tables have `created_at`; almost none have `updated_at` |
| eager loading (includes:) | yes | n/a | Not needed at our scale |
| strong parameters | yes | partial | 16 / 42 controllers use `sp_require`/`sp_permit` |
| foreign keys in schema | yes | **none** | 45 CREATE TABLE/INDEX, 0 `FOREIGN KEY` clauses |
| `zcl_result` adoption | n/a | 5% | 2 models + 1 lib; 17 models + 40 controllers still return bare `bool` |
| raw sqlite in app/ | n/a | 131 step + 118 exec | `blockchain_controller.c` alone has 90 raw sqlite calls |
| Rails-shaped error JSON | yes | partial | No uniform `{errors: {field: [messages]}}` envelope |

---

## Deliverable tracks

Work splits cleanly into two tracks that can proceed in parallel. Each track maps to one long-running branch per agent, rolled out as 3–5 small PRs each.

### Track M — Model layer (Agent 2)

Owns: every file under `app/models/src/*.c`, `app/models/include/models/*.h`, `lib/util/include/util/result.h`, `lib/wallet/**` validate-path files.

1. **M1. Rails-grade validation macros.** Add to `activerecord.h`:
   - `validates_uniqueness_of(ndb, errors, record, field, table, column)` — runs `SELECT 1 FROM <table> WHERE <column>=? LIMIT 1` before INSERT, fills `ar_errors` on hit. Paired with a matching SQL UNIQUE INDEX (belt-and-suspenders).
   - `validates_format_of(errors, record, field, matcher)` — takes a function pointer matcher. Ship matchers: `ar_matches_hex64`, `ar_matches_hex32`, `ar_matches_base58_taddr`, `ar_matches_bech32_zaddr`, `ar_matches_onion_v3`, `ar_matches_ipv4`, `ar_matches_ipv6`, `ar_matches_txid`, `ar_matches_name_chars`. Each matcher is a pure C function — no regex dependency.
   - `validates_numericality_of_greater_than/less_than/only_integer` — fill out the numeric gap.
   - `validates_inclusion_of_str(errors, record, field, choices, n_choices)` — string variant of existing int macro.
   - `validates_associated(ndb, errors, record, fk, parent_table, parent_column)` — asserts FK row exists.

2. **M2. `validate :method` style — named custom validators.** Each model gains a `struct db_<model>_validators { bool (*fn)(const record *, struct ar_errors *); const char *name; }` table so tests / introspection / metrics can see *which* check failed, not just *that* validation failed. `ar_errors_add` already keys errors by field — add a `name` slot too.

3. **M3. zcl_result rollout to every model save/load.** Every `db_<model>_save` / `db_<model>_find*` function gets a `_r` variant returning `struct zcl_result` (existing `bool` wrapper stays for BC until call sites migrate). Priority order: wallet_tx, wallet_utxo, utxo, block, tx_index, peer, mempool, zslp_balance, zslp_token, store_order, store_product, sapling_key, sapling_note, wallet_script, contact, file_service, onion_announcement, mmb_leaf_store, chain_snapshot.

4. **M4. Lifecycle split — `before_create` / `before_update` / `after_commit` / `after_rollback`.** Add the four new hook slots to `ar_callbacks`. `AR_BEGIN_SAVE` and `AR_FINISH_SAVE` branch on "did this row exist before?" using the existing `_exists()` helpers. `after_commit` / `after_rollback` plug into `db_txn` — the event emits on real commit/rollback, not on `AR_FINISH_SAVE`.

5. **M5. Model-level uniqueness + format audit.** Apply the new macros to every obvious field:
   - `wallet_keys.pubkey` — UNIQUE + hex64 format.
   - `wallet_keys.address` — UNIQUE + t-addr format.
   - `blocks.hash` — UNIQUE + hex64.
   - `blocks.height` — UNIQUE (we already have the INDEX, add the validation path).
   - `utxos (txid, vout)` — composite UNIQUE (existing) + format check on txid.
   - `peers (ip, port)` — UNIQUE (have it) + format check on ip.
   - `onion_announcements.onion` — UNIQUE + format check.
   - `names.name` — UNIQUE + format check (1-63 chars, LDH subset).
   - `contacts.address` — format check.
   - `tx_index.txid` — UNIQUE + hex64.
   - `mempool.txid` — UNIQUE + hex64.
   - `store_orders.order_id` — UNIQUE + format.
   - `zslp_tokens.token_id` — UNIQUE + format.

6. **M6. Associations + referential integrity.** For each `belongs_to` documented in `activerecord.h`:
   - Add a `validates_associated` call on save.
   - Add `dependent_destroy` pattern: when parent is destroyed, children go too. Start with `wallet_key → wallet_utxo`, `block → tx_index`, `tx_index → utxo`, `wallet_key → sapling_note`. No SQL FK clauses (SQLite enforces poorly); use AR hooks.

7. **M7. `timestamps` helper.** `validates_timestamps(record)` enforces `created_at > 0` and (if present) `updated_at >= created_at`. Add `updated_at` to tables that mutate: wallet_keys, utxos, peers, wallet_transactions, mempool, zslp_balances, store_orders, contacts. Auto-populated by a new `ar_touch(record)` helper called from `before_save`.

**Track M branch:** `a2/rails-validations` — ships as 3 PRs:
- `a2/rails-validations-macros` — M1 + M2 + matchers (no caller changes).
- `a2/rails-validations-apply` — M5 + M6 (apply to every model).
- `a2/rails-validations-lifecycle` — M3 + M4 + M7 (zcl_result + create/update/commit/rollback split + timestamps).

### Track C — Controller / service layer (Agent 3)

Owns: `app/controllers/src/*.c`, `app/services/src/*.c`, `tools/mcp/**`, `config/src/boot.c` for state-machine integration, `lib/rpc/**` for error envelope.

1. **C1. Strong parameters across all 42 controllers.** Every RPC/REST/MCP entrypoint starts with `sp_require(params, "name")` + `sp_permit(params, {"field1","field2",...})`. Enforced by lint: any public handler without a `sp_require` on entry fails `make lint`. Current 16 that already use it are the template.

2. **C2. Rails-style uniform error envelope.** Every RPC error returns `{"error":{"code":"validation_failed","fields":{"address":["can't be blank","is not a t-addr"]}}}`. Ship `rpc_errors_from_ar_errors(struct ar_errors *)` that converts model validator output to this shape. Retire ad-hoc error strings in controllers.

3. **C3. Controller AR migration.** Migrate raw `sqlite3_step` / `sqlite3_exec` in `app/controllers/src/` to AR_QUERY_* or `db_<model>_find_*` helpers. Priority: blockchain_controller.c (90 raw calls), explorer_stats.c (23), explorer_factoids.c (17), explorer_controller.c (13). Anything that can't migrate gets `// raw-sql-ok: read-only-explorer, agent-3` annotation + justification.

4. **C4. `before_action` filters for every mutating handler.** Use the existing router `before_filters` array (`ar_route_add_filter`) to wire:
   - `require_wallet_unlocked` on every `send*`, `signrawtransaction`, `dumpprivkey`, `importprivkey`.
   - `require_chain_synced` on `sendtoaddress`, mining handlers.
   - `require_tor_bootstrapped` on `/directory.json`, `onion_status`.
   - `rate_limit_rpc` globally via `ar_router_add_filter`.

5. **C5. MCP handler parity.** Every MCP handler (`tools/mcp/**`) sets a response body on error, maps errors to the same envelope C2 produces, and participates in the same lint gate C1 enforces. Current gap: several handlers `return -1;` with no body per `HARDENING_CHECKLIST §P1.5`.

6. **C6. After-commit hooks for notifications.** Wire `after_commit` for:
   - wallet_tx → emit `EV_WALLET_TX_COMMITTED` → refresh wallet balance cache + push to WebSocket.
   - block → emit `EV_BLOCK_CONNECTED` (already exists) ONLY on commit, not on `AR_FINISH_SAVE` (which fires before commit if save is inside a txn).
   - peer save → debounced addrman persist.

7. **C7. RPC schema docs from validators.** `make docs` generates OpenAPI schema for every RPC handler by introspecting the registered validators and strong-param lists. Closes the drift between code and `zcl_openapi`.

**Track C branch:** `a3/rails-controllers` — ships as 3 PRs:
- `a3/rails-strong-params` — C1 + C2 (mechanical sweep).
- `a3/rails-before-actions` — C4 + C6 (wire filters + after_commit hooks).
- `a3/rails-controllers-ar` — C3 + C5 + C7 (AR migration + MCP parity + docs).

---

## Acceptance gates (must pass before closing the parity)

1. `make lint` exits 0 with raw-sqlite check set to FAIL (Agent 3's D2 from build/CI/deploy).
2. Every public `db_<model>_save` has a matching `db_<model>_save_r` that returns `zcl_result`.
3. Every model with a UNIQUE SQL index has a matching `validates_uniqueness_of` call.
4. Every string field that carries consensus meaning (addresses, txids, hashes, names) has a `validates_format_of` call.
5. Every controller handler that mutates state calls `sp_require` + `sp_permit` on params.
6. Every RPC error returns the C2 envelope.
7. `db_register_all_validators()` size equals `count(CREATE TABLE)` — no orphan tables.
8. `test_db_validators.c` passes a property-based fuzz: generate random field corruption, assert validator catches it, assert no crash.
9. `after_commit` hooks fire only after a real commit; `test_after_commit_semantics` regression proves it via a forced rollback.
10. No new `return false;` in `app/models/src/` or `app/controllers/src/` without a `LOG_FAIL` on the same or adjacent line (lint).

---

## Non-goals (to keep scope tight)

- Eager loading / `includes:` — not at our scale.
- Polymorphic associations — not needed.
- STI (single-table inheritance) — not needed.
- A full ORM query DSL — `AR_QUERY_*` macros stay as-is; no `where` chaining.
- Rails-style migrations with `rails db:migrate` CLI — we have SCHEMA[] + `schema_migrations` table, that's fine.

---

## Ordering and parallelism

- Agent 2 starts Track M after their in-flight D1 (silent-full-DB-wipe kill) lands.
- Agent 3 starts Track C after their in-flight build/CI/deploy PR lands.
- No ordering between tracks — both can run in parallel. Track C depends on Track M1 macros only for C2's `rpc_errors_from_ar_errors`; until M1 lands, C2 can use the existing `ar_errors` shape unchanged.

Keep pushing to master. Do not stand down.
