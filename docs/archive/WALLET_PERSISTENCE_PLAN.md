# Wallet Persistence Fix — Master Plan

**Status:** design, not yet implemented.
**Owner (review):** Rhett.
**Owner (architecture):** Claude (plan + review).
**Owner (implementation):** two independent agents working in `~/zclassic23-2` and `~/zclassic23-3`. See `tasks/AGENT_2_WALLET_SQLITE.md` and `tasks/AGENT_3_WALLET_GUARDRAILS.md`.

---

## 1. The bug

`wallet_sqlite_open()` returns `false` at boot on a node with an existing `node.db`. The return is silent — no `LOG_FAIL`, no log line. Every downstream call (`wallet_sqlite_flush`, `wallet_sqlite_write_key`) begins with `if (!ws->open) return false;` and also returns silently. `boot.c` falls through to its "no keys found" path:

```c
// config/src/boot.c line 517
if (g_wallet.keystore.num_keys == 0) {
    ...
    wallet_top_up_key_pool(&g_wallet, DEFAULT_KEYPOOL_SIZE);  /* 100 fresh keys */
    if (g_wallet_sqlite.open)
        wallet_sqlite_flush(&g_wallet_sqlite, &g_wallet);      /* open is false → no-op */
    ...
}
```

Observed behaviour on rhett's node:

- `node.log` shows `"New wallet created."` on every boot (boot.c:483, the else-branch when `wallet_sqlite_open` failed).
- `listwalletkeys` consistently returns exactly 100 keys after every restart.
- The 100 keys have **zero overlap** with any of the three JSON backups under `~/wallet_backups/` and zero overlap with two prior wallet.dat files.
- `importprivkey <wif>` returns success, `listwalletkeys` briefly shows the new count, then a restart resets to 100 fresh keypool keys.
- `getnewaddress` returns an address; `dumpprivkey <that address>` works before restart, returns `"Private key for address is not known"` after restart.

Impact: **any ZCL sent to an address generated or imported between restarts becomes unspendable at the next restart.** This matches the "lost 0.4 ZCL" incident recorded in rhett's session memory.

### Why the silence

`DEFENSIVE_CODING.md §1` already identifies `wallet_sqlite.c` as needing migration away from raw `sqlite3_step` to the `AR_*` macros. `§2` prescribes a `zcl_result` return type replacing bare `bool` so every failure carries a code, a message, and a source location. This bug is the exact failure mode those rules were written to prevent — the fix is already on the roadmap.

---

## 2. Non-goals

- Not rewriting the wallet crypto.
- Not changing the SQLite schema of `wallet_keys` / `wallet_sapling_keys`.
- Not touching the Sapling note tree or anchor logic.
- Not changing the MCP protocol surface (new fields are additive).
- Not adding new storage engines.

---

## 3. Design principles

These are the rules the two agents work under. Any code that violates one is rejected in review.

1. **Fail loud, fail early.** No `return false` without `LOG_FAIL`. No silent early-return on misconfiguration.
2. **Rich error types.** Every service-layer function that can fail returns `struct zcl_result` (per `DEFENSIVE_CODING.md §2`), not `bool`. Thin `bool` shims are permitted only at the call-site while migration is in progress, and each must be TODO-annotated with the ticket number.
3. **Model owns invariants.** `wallet_key` model validates pubkey/privkey consistency in `before_save`; no controller or service bypasses.
4. **Controller rolls back on failure.** If persistence fails, the in-memory keystore is reverted. User-visible state (RPC response, in-memory balance, JSON backup) never reflects a write that did not hit disk.
5. **Boot refuses to proceed silently.** If persistence can't be initialised and the table has pre-existing rows, the daemon exits non-zero with instructions. Silent wiping is the exact footgun that caused the 0.4 ZCL loss.
6. **Canary on every boot.** Write-read-delete on a dedicated row before any RPC is accepted. If the canary fails, abort.
7. **Writes have an on-disk mirror.** Every successful key write triggers a JSON backup via the existing `wallet_backup_service`. Losing the SQLite file is recoverable from the JSON; the JSON never disagrees with the SQLite.
8. **Invariants cross-checked at boot.** `SELECT count(*) FROM wallet_keys` must equal the in-memory keystore count. Divergence is fatal.

---

## 4. Architecture — Rails-way MVC

```
                    Request (RPC / CLI)
                            |
                            v
    +---------------------------------------------+
    |  CONTROLLER  — app/controllers/src/         |
    |    wallet_controller.c                      |
    |      rpc_importprivkey / rpc_getnewaddress  |
    |      - argument parsing                     |
    |      - orchestration                        |
    |      - ROLLBACK on persistence failure      |
    +---------------------------------------------+
                            |
                            v
    +---------------------------------------------+
    |  MODEL — app/models/src/                    |
    |    wallet_key.c  (one key = one row)        |
    |      db_wallet_key_save(ndb, pk, sk)        |
    |      - validate()                           |
    |      - before_save hooks                    |
    |      - AR_BEGIN_SAVE → INSERT OR REPLACE    |
    |      - after_save hooks (→ backup service)  |
    +---------------------------------------------+
                            |
                            v
    +---------------------------------------------+
    |  SERVICE — lib/wallet/src/                  |
    |    wallet_sqlite.c (low-level storage)      |
    |      returns struct zcl_result              |
    |    wallet_canary.c (NEW, boot self-test)    |
    |    wallet_backup_service.c (writes JSON)    |
    +---------------------------------------------+
                            |
                            v
    +---------------------------------------------+
    |  STORE — sqlite3 (node.db), JSON mirror     |
    +---------------------------------------------+
```

- **View**: RPC JSON responses. A new `persistence` block on `getwalletinfo` reports health (see §6).
- **Service boundary**: anything above the dashed line in `lib/wallet/` is pure C, no RPC context. Anything in `app/` may reference controllers.
- **Tests**: `lib/test/src/test_wallet_*.c` for unit; `lib/test/spec/spec_e2e_wallet_*.c` for end-to-end fork-a-daemon tests.

---

## 5. Shared interface (both agents code against this)

These signatures are the contract between Agent 2 and Agent 3. Agent 2 implements them in `wallet_sqlite.c`; Agent 3 consumes them from `wallet_controller.c` and from `boot.c` health reporting.

### 5.1 `lib/util/include/util/result.h` (new file, Agent 2 creates)

```c
/* Per DEFENSIVE_CODING.md §2. Replaces bare bool returns in service layer. */
struct zcl_result {
    bool        ok;               /* true on success, false on failure */
    int         code;             /* 0 on success; category-specific negative on failure */
    char        message[256];     /* human-readable; always populated on failure */
    const char *source_file;      /* __FILE__ at the point of failure */
    int         source_line;      /* __LINE__ at the point of failure */
};

#define ZCL_OK ((struct zcl_result){.ok = true, .code = 0})

/* Macro that fills message via snprintf; evaluates to a struct literal. */
#define ZCL_ERR(err_code, fmt, ...) \
    zcl_result_make((err_code), __FILE__, __LINE__, fmt, ##__VA_ARGS__)

struct zcl_result zcl_result_make(int code,
                                   const char *file, int line,
                                   const char *fmt, ...);

/* Short-circuit helper: log and return if result is not ok. */
#define ZCL_CHECK(res_expr) do { \
    struct zcl_result _zr = (res_expr); \
    if (!_zr.ok) { \
        LOG_FAIL("zcl_check", "%s:%d code=%d: %s", \
                 _zr.source_file, _zr.source_line, _zr.code, _zr.message); \
        return _zr; \
    } \
} while (0)
```

### 5.2 `lib/wallet/include/wallet/wallet_sqlite.h` — migrated API

```c
/* Error codes for wallet_sqlite layer. Negative to avoid collision with SQLITE_*. */
enum wallet_sqlite_err {
    WSQL_OK                      = 0,
    WSQL_NULL_ARG                = -100,
    WSQL_DB_NOT_OPEN             = -101,  /* underlying sqlite3* was NULL/closed */
    WSQL_ALREADY_OPEN            = -102,
    WSQL_PREPARE_FAIL            = -103,  /* sqlite3_prepare_v2 failed on some stmt */
    WSQL_SCHEMA_MISSING          = -104,  /* wallet_keys table does not exist */
    WSQL_CANARY_WRITE_FAIL       = -110,
    WSQL_CANARY_READ_MISMATCH    = -111,
    WSQL_WRITE_FAIL              = -120,
    WSQL_READ_FAIL               = -121,
    WSQL_TXN_BEGIN_FAIL          = -130,
    WSQL_TXN_COMMIT_FAIL         = -131,
    WSQL_INVARIANT_PUBKEY        = -140,
    WSQL_INVARIANT_PRIVKEY       = -141,
    WSQL_INVARIANT_HASH_MISMATCH = -142,
};

struct wallet_sqlite_health {
    bool    open;                    /* subsystem open */
    bool    canary_ok;               /* last self-test passed */
    int64_t canary_last_ok_ts;       /* unix time of last successful self-test */
    int     row_count;               /* SELECT count(*) FROM wallet_keys */
    int     keystore_count;          /* in-memory count passed in by caller */
    bool    mismatch;                /* row_count != keystore_count */
    char    last_error[256];         /* from most recent failed call, if any */
};

/* Open/prepare. New rich-error signature. Replaces the old `bool` return. */
struct zcl_result wallet_sqlite_open(struct wallet_sqlite *ws, sqlite3 *db);

/* Canary: write → read → compare → delete on wallet_canary table. */
struct zcl_result wallet_sqlite_self_test(struct wallet_sqlite *ws);

/* Single-key write. Wrapped by app/models/src/wallet_key.c. */
struct zcl_result wallet_sqlite_write_key(struct wallet_sqlite *ws,
                                          const struct pubkey *pk,
                                          const struct privkey *key);

/* Read all keys into keystore. */
struct zcl_result wallet_sqlite_read_keys(struct wallet_sqlite *ws,
                                          struct wallet *w);

/* Read a single key by pubkey. Used by controller readback verification. */
struct zcl_result wallet_sqlite_read_single_key(struct wallet_sqlite *ws,
                                                const struct pubkey *pk,
                                                struct privkey *out_key);

/* Full flush (keys + txs + sapling + scripts + scan_height). */
struct zcl_result wallet_sqlite_flush(struct wallet_sqlite *ws,
                                      struct wallet *w);

/* Snapshot of current health. Non-destructive. Safe to call any time. */
struct wallet_sqlite_health wallet_sqlite_get_health(struct wallet_sqlite *ws,
                                                     int keystore_count);
```

Backwards compatibility during migration: Agent 2 MAY keep the old `bool`-returning names as thin wrappers around the new ones, but those wrappers MUST `LOG_FAIL` on non-ok and MUST be marked `ZCL_DEPRECATED`. Ideal: no wrappers; migrate all callers in one pass.

### 5.3 `lib/wallet/include/wallet/wallet_canary.h` (new)

```c
/* Canary is a distinct table (wallet_canary) from wallet_keys.
 * Schema:
 *   CREATE TABLE IF NOT EXISTS wallet_canary (
 *     id INTEGER PRIMARY KEY CHECK (id=1),
 *     probe BLOB NOT NULL,
 *     ts   INTEGER NOT NULL
 *   )
 *
 * Self-test:
 *   1. generate 32 random bytes
 *   2. INSERT OR REPLACE into wallet_canary with id=1
 *   3. SELECT probe FROM wallet_canary WHERE id=1
 *   4. assert equal
 *
 * Never returns stale data — always writes fresh probe first.
 */
struct zcl_result wallet_canary_run(sqlite3 *db);
```

### 5.4 Controller contract (Agent 3 implements)

`rpc_importprivkey` flow:

```
1. parse WIF          → on fail, set error, LOG_FAIL, return
2. decode privkey     → on fail, set error, LOG_FAIL, return
3. derive pubkey      → on fail, set error, LOG_FAIL, return
4. validate invariants (pubkey_hash == hash160(pubkey))
                      → on fail, set error, LOG_FAIL, return
5. call db_wallet_key_save (model layer)
                      → if non-ok: DO NOT touch keystore, return error
                      → if ok: add to keystore
6. readback: wallet_sqlite_read_single_key
                      → if readback does not match written key:
                        - db_wallet_key_delete (model layer)
                        - remove from keystore
                        - set error, LOG_FAIL, return
7. trigger after_save hook → JSON backup
8. scan utxos table for this address, copy into wallet_utxos (existing code path)
9. return address
```

Note: current controller adds to keystore BEFORE persistence. That order inverts here.

---

## 6. View — observability contract

`getwalletinfo` response gains a new `persistence` block:

```json
{
  "walletversion": ...,
  "balance": ...,
  "persistence": {
    "healthy": true,
    "open": true,
    "canary_ok": true,
    "canary_last_ok_ts": 1776400000,
    "row_count": 100,
    "keystore_count": 100,
    "mismatch": false,
    "last_error": ""
  }
}
```

Existing fields are unchanged. `persistence.healthy` is an aggregate: `open && canary_ok && !mismatch`.

---

## 7. Boot state machine

`config/src/boot.c` wallet initialisation replaces the current flat flow with an explicit state machine. All states exit either to `STATE_READY` (normal operation) or `STATE_ABORT` (exit 1).

| State | Condition | Action |
|---|---|---|
| A | node.db absent | create node.db, generate keypool, flush, STATE_READY |
| B | node.db exists, `wallet_keys` table missing | CREATE TABLE, generate keypool, flush, STATE_READY |
| C | node.db exists, `wallet_keys` non-empty, open OK | load keys, self-test, STATE_READY |
| D | node.db exists, `wallet_keys` non-empty, **open FAILS** | print error with code+message, exit 1 |
| E | open OK but `wallet_sqlite_self_test` fails | print error with code+message, exit 1 |
| F | keystore loaded but `row_count != keystore_count` after load | print diagnostics, exit 1 |

Rationale for D–F: the current code silently enters the "generate fresh keypool" path, which is what corrupts user wallets. Refusing to proceed preserves the disk state so the operator can investigate.

The abort path prints:

```
FATAL: wallet persistence initialisation failed.
       code=<WSQL_*>
       message=<zcl_result.message>
       source=<file:line>
       node.db contains <N> existing wallet_keys rows — REFUSING to regenerate.
       To recover: see WALLET_PERSISTENCE_RECOVERY.md
```

---

## 8. Tests (required to pass before either PR merges)

### 8.1 Unit tests

- `lib/test/src/test_wallet_persistence_cycle.c` (NEW, Agent 2)
  - open DB with empty schema → verify open OK, canary OK, row_count 0.
  - write 3 keys → verify row_count 3.
  - close, reopen → verify row_count 3 and read_keys returns all 3 with matching privkeys.
  - corrupt wallet_canary probe → verify self_test returns WSQL_CANARY_READ_MISMATCH.

- `lib/test/src/test_wallet_sqlite_open_errors.c` (NEW, Agent 2)
  - pass NULL sqlite3* → returns WSQL_DB_NOT_OPEN with non-empty message.
  - drop `wallet_keys` table then open → WSQL_SCHEMA_MISSING.
  - close db mid-open → WSQL_PREPARE_FAIL.
  - each code has a unique, non-empty `message`.

- `lib/test/src/test_wallet_controller_rollback.c` (NEW, Agent 3)
  - stub `wallet_sqlite_write_key` to return WSQL_WRITE_FAIL.
  - call rpc_importprivkey → assert: keystore unchanged, JSON backup NOT written, RPC returns error code and non-empty message.

### 8.2 Integration

- `lib/test/spec/spec_e2e_wallet_restart.c` (NEW, Agent 3)
  - fork a daemon with fresh datadir.
  - RPC: importprivkey K1; verify dumpprivkey returns K1.
  - send SIGTERM, wait for clean exit.
  - fork daemon again with same datadir.
  - RPC: dumpprivkey for K1's address → must equal K1.
  - This test would have caught the current bug.

### 8.3 Build / lint

- `make lint` green — no raw `sqlite3_step` in `wallet_sqlite.c` (Agent 2 migrates to `AR_*` macros or adds `#define ZCL_AR_RAW_SQL` with a justification comment).
- `make ci` green — all 1572+ existing tests pass plus the new ones above.
- No new compiler warnings at `-Wall -Wextra`.

---

## 9. Acceptance criteria (system-level)

The fix is complete when all of the following hold on a deployment:

1. `importprivkey <wif>` → restart daemon → `dumpprivkey <addr>` returns the same WIF.
2. `getnewaddress` → restart → `dumpprivkey <addr>` returns a valid WIF.
3. `node.log` contains `Wallet loaded: <N> keys, ...` (not `New wallet created.`) on a node with an existing `node.db`.
4. `getwalletinfo` reports `persistence.healthy: true`, `mismatch: false`, and a fresh `canary_last_ok_ts`.
5. Deleting `node.db-wal` while daemon is stopped does NOT cause key loss (provided a checkpoint happened before stop — separate WAL hygiene tracked in `tools/wal_checkpoint`).
6. Making any persistence call fail (by e.g. `chmod -w node.db` and a write attempt) surfaces as:
   - `LOG_FAIL` line in `node.log` with code and message,
   - non-ok JSON response with same code and message,
   - no keystore mutation.

---

## 10. Risks and mitigations

| Risk | Mitigation |
|---|---|
| Migration breaks existing callers | Agent 2 migrates all callers in the same PR. `make ci` covers them. |
| Canary pollutes backups | Canary uses dedicated `wallet_canary` table, never `wallet_keys`. Not included in JSON backup. |
| Abort-on-fail breaks first-boot on a clean node | STATE A and STATE B explicitly handle empty/absent DB. Abort only fires if `wallet_keys` has rows AND open failed. |
| Boot abort loops | systemd `Restart=on-failure` will retry forever. Add `RestartSec=30` (already present) and an explicit panic log to make the cause visible. Operator intervention is intended; this is preferable to silent data loss. |
| Deadlock between wallet cs mutex and new rollback path | Controller takes mutex, calls model, model calls service. Service NEVER takes the wallet cs. Lock order: controller → wallet cs → (no service layer lock). |
| JSON backup write failure masks SQLite success | after_save backup is best-effort and logs via LOG_ERR (not LOG_FAIL). SQLite write is authoritative. |

---

## 11. Workflow for the implementing admin

1. Hand `tasks/AGENT_2_WALLET_SQLITE.md` to Agent 2, working in `~/zclassic23-2`.
2. Hand `tasks/AGENT_3_WALLET_GUARDRAILS.md` to Agent 3, working in `~/zclassic23-3`.
3. Both agents branch off current `master` HEAD (70d2904bd at time of writing).
4. Agents push branches when done; open PRs against `origin/master`.
5. Claude (this session or successor) pulls the branches, reviews, runs acceptance tests §9, merges.
6. After both merged, smoke-test on rhett's live node: destroy `~/.zclassic-c23/node.db`, restart, observe keypool generation + persistence cycle.

Agent isolation:

- Agent 2 files: `lib/wallet/**`, `app/models/src/wallet_key.c`, `app/models/src/wallet_sapling_key.c`, `lib/util/include/util/result.h`, `lib/util/src/result.c`, boot.c *open-call site only (one line)*, new tests under `lib/test/src/test_wallet_persistence_cycle.c` and `test_wallet_sqlite_open_errors.c`.
- Agent 3 files: `app/controllers/src/wallet_controller.c`, `app/controllers/src/wallet_diagnostic_controller.c` (getwalletinfo formatter), `app/services/src/wallet_backup_service.{c,h}`, new `lib/wallet/src/wallet_canary.c`, `lib/wallet/include/wallet/wallet_canary.h`, `config/src/boot.c` state-machine block, tests `test_wallet_controller_rollback.c` + `spec_e2e_wallet_restart.c`.

The one file touched by both is `config/src/boot.c`. Agent 3 owns the state machine; Agent 2 only swaps the `wallet_sqlite_open` call signature. Trivially resolvable merge.

---

## 12. Rollback plan

If either PR ships and regresses production:

- `git revert` the merge commit on master.
- Rebuild with `make -j$(nproc)` and `make deploy` (per rhett's CLAUDE.md).
- No data migration needed — new code only adds tables (`wallet_canary`), never alters existing schema.
- Pre-existing `wallet_keys` rows are untouched on revert.

---

## 13. Out of scope (explicitly deferred)

- Encrypted wallet-at-rest key rotation — already handled by existing `is_wks1_blob` path.
- HD seed recovery from mnemonic — separate ticket.
- Sapling key persistence — uses same `wallet_sqlite_flush` path; same fix applies. Agent 2 migrates that loop too (it's in `wallet_sqlite_flush` already).
- Auto-repair on corrupt `wallet_canary` — beyond scope; abort is correct.
- UI / explorer changes — backend only.
