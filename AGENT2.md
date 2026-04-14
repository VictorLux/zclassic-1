# AGENT2 — Wave 21: Defensive Coding & Rails-Way Hardening

**Working directory:** `~/zclassic23-2`
**Coordinator:** Agent1 (~/zclassic23)
**Workflow:** `git pull origin master` before starting, `git push origin master` when done with each task.
**Run `make -j$(nproc) && make test && make lint` before every push.**

---

## Context

The sync pipeline bugs are being fixed by Agent1 (activation, batching, pprev walks). Your focus now shifts to **defensive coding** — making the codebase more robust so bugs like these are caught earlier or can't happen.

Read `DEFENSIVE_CODING.md` first. It defines the patterns we want enforced everywhere.

**CRITICAL: Do NOT modify `lib/validation/src/process_block.c` — Agent1 owns this file. Pulling your pprev NULL guard change caused a regression (rejected all deep chains after LDB import). Agent1 has fixed it.**

---

## Task 1: Wire before_save / after_save Hooks on ALL Models

Only 3 of 13 models have hooks wired: utxo, block, wallet_tx. Add hooks to the remaining 10.

### Models to wire (in `app/models/src/`):

| Model | before_save | after_save |
|-------|-------------|------------|
| `wallet_key.c` | Validate key length, check addr format | Emit `EV_WALLET_KEY_SAVED` |
| `peer.c` | Validate IP format, port range | Emit `EV_PEER_SAVED` |
| `store.c` | Validate price >= 0, name non-empty | Emit `EV_MODEL_SAVED` |
| `contact.c` | Validate name non-empty | Emit `EV_MODEL_SAVED` |
| `zslp.c` | Validate token_id format, amount >= 0 | Emit `EV_MODEL_SAVED` |
| `mempool_entry.c` | Validate txid format, fee >= 0 | Emit `EV_MODEL_SAVED` |
| `tx_index.c` | Validate txid non-null, height >= 0 | Emit `EV_MODEL_SAVED` |
| `file_service.c` | Validate hash non-null, size > 0 | Emit `EV_MODEL_SAVED` |
| `onion_announcement.c` | Validate onion addr format | Emit `EV_MODEL_SAVED` |
| `mmb_leaf_store.c` | Validate height >= 0 | Emit `EV_MODEL_SAVED` |

Follow the pattern in `utxo.c`:
1. `DEFINE_MODEL_CALLBACKS(name)` macro
2. `static bool name_before_save(void *record, void *ctx)` — validate fields
3. `static void name_after_save(void *record, void *ctx)` — emit event
4. `ar_register_before_save(cbs, name_before_save)` in init function

### Test:
For each model, verify the existing tests still pass and add at least one test that confirms before_save rejects invalid data.

---

## Task 2: Replace Raw sqlite3_step in Controllers with Model Methods

102 raw `sqlite3_step()` calls exist in `app/controllers/src/`. These bypass ActiveRecord validations.

### Priority files (most raw calls):
1. `explorer_stats.c` (21 calls) — convert to model queries
2. `explorer_factoids.c` (17 calls) — convert to model queries
3. `blockchain_controller.c` (12 calls) — convert to model queries
4. `sync_controller.c` (9 calls) — convert to model queries
5. `explorer_controller.c` (10 calls) — convert to model queries

### Pattern:
```c
// BEFORE (bad — bypasses AR):
sqlite3_prepare_v2(db, "SELECT * FROM utxos WHERE height=?", ...);
sqlite3_bind_int(stmt, 1, height);
while (sqlite3_step(stmt) == SQLITE_ROW) { ... }

// AFTER (good — goes through model):
// Add to utxo.h:
struct db_utxo *utxo_find_by_height(sqlite3 *db, int height, size_t *count);
// Then in controller:
size_t count;
struct db_utxo *utxos = utxo_find_by_height(db, height, &count);
```

For read-only queries (SELECT), you can add `// raw-sql-ok: read-only query` to suppress lint. But writes MUST go through AR.

---

## Task 3: Implement struct zcl_result for Service Functions

Implement the `struct zcl_result` pattern from `DEFENSIVE_CODING.md`.

### Step 1: Create `lib/util/include/util/result.h`
Use the definition from DEFENSIVE_CODING.md section 2.

### Step 2: Migrate 3 key service functions to use it:
1. `sync_watchdog_check()` — currently returns `enum watchdog_recovery_type`
2. `syncsvc_build_stall_recovery()` — currently returns `bool`
3. `activation_request_connect()` — currently returns void with out param

For now, just add `struct zcl_result` as an ADDITIONAL out param so callers can log the reason. Don't change the return type of existing functions yet — that's too disruptive.

```c
// Example: add result param to existing function
bool syncsvc_build_stall_recovery(struct sync_stall_recovery *recovery,
                                  const struct main_state *ms,
                                  ...,
                                  struct zcl_result *result);  // NEW
```

---

## Task 4: Add log_json to Critical Error Paths

Only 69 `log_json` calls exist (mostly in net/). Add structured logging to the 10 most critical error paths in the sync/validation pipeline.

### Files to add log_json calls:
1. `app/services/src/chain_activation_controller.c` — on activation failure
2. `app/services/src/sync_watchdog_service.c` — on every recovery action
3. `app/services/src/header_sync_service.c` — on chains_from_tip failure
4. `config/src/boot.c` — on UTXO load failure, block index load failure
5. `lib/storage/src/coins_view_sqlite.c` — on flush failure

### Pattern:
```c
log_json("error", "activation_failed",
         "height", tip_h,
         "reason", state->reject_reason,
         "file", __FILE__,
         "line", __LINE__);
```

---

## Build & Test

After EACH task:
```bash
git pull origin master
make -j$(nproc) 2>&1 | tail -20
make test 2>&1 | tail -10
make lint 2>&1 | tail -10
git add <specific files> && git commit -m "wave 21 task N: description"
git push origin master
```

## Boundary: Files You MUST NOT Touch
- `lib/validation/src/process_block.c` (Agent1 — DO NOT TOUCH)
- `lib/net/src/msg_headers.c` (Agent1)
- `lib/net/src/msgprocessor.c` (Agent1)
- `lib/net/src/download.c` (Agent1)
