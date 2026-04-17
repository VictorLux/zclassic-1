# Agent 3 — Wallet Controller Guardrails, Canary, Boot State Machine

**STATUS: MERGED** — all 5 deliverables landed in commits `93ad65502..e4649ebbb` (canary table + self-test, backup service on_key_change trigger, controller rollback on persistence failure, boot state machine with STATE_D/E/F aborts, getwalletinfo persistence health block, test_wallet_canary + test_wallet_persistence_cycle).

**Your next task is [`AGENT_3_BUILD_CI_DEPLOY.md`](./AGENT_3_BUILD_CI_DEPLOY.md).** Note: **D8 was already fixed by Agent 2** as a bonus on their persistence-hardening PR (`7a955c0dd`). Skip D8. The rest of D1–D9 is still yours.

**Directive: keep pushing to master.** When that PR merges, request the next assignment — do not stand down.

---

## Original brief (below, kept for history)

**Read first:** [`WALLET_PERSISTENCE_PLAN.md`](../WALLET_PERSISTENCE_PLAN.md). This doc assumes you have.

**Worktree:** `~/zclassic23-3`
**Branch:** `a3/wallet-controller-guardrails`
**Base:** `origin/master`
**Parallel peer:** Agent 2 in `~/zclassic23-2` on branch `a2/wallet-sqlite-result-types`. You do not coordinate directly; the interface in plan §5 is the contract.

**Dependency:** Agent 2's `struct zcl_result` and the migrated `wallet_sqlite_*` signatures. You MUST code to the interface defined in plan §5 even before Agent 2 merges. Two ways to stay unblocked:

1. **Stub Agent 2's header locally.** Copy plan §5 signatures into `lib/wallet/include/wallet/wallet_sqlite.h` in your worktree with `/* TODO(agent2) */` markers. When Agent 2 merges, rebase, delete your stubs, pull theirs.
2. **Write against the types only.** Your controller code invokes `wallet_sqlite_write_key` and checks `r.ok`; the compiler won't care about the implementation until link time. Add a tiny `--allow-undefined` make target for your isolated tests, or mock the service layer in tests.

Either is acceptable. Prefer (1) if Agent 2 has pushed; prefer (2) if not.

---

## Mission, one sentence

Make every RPC / boot path that touches wallet persistence surface failures loudly, roll back on write failure, mirror writes to JSON, and refuse to proceed into silent keypool regeneration.

---

## Scope

### Files you own (edit freely)

- `app/controllers/src/wallet_controller.c`
- `app/controllers/src/wallet_diagnostic_controller.c` (only the `getwalletinfo` RPC formatter)
- `app/controllers/src/wallet_shielded_controller.c` (same flush-return-check pattern for `z_*` RPCs — identical fix)
- `app/controllers/src/wallet_rescan_controller.c` (same pattern for rescan)
- `app/services/src/wallet_backup_service.c`
- `app/services/include/services/wallet_backup_service.h`
- `lib/wallet/src/wallet_canary.c`  **(create)**
- `lib/wallet/include/wallet/wallet_canary.h`  **(create)**
- `config/src/boot.c` — the wallet initialisation region (lines ~459–547). Rewrite as state machine per plan §7. Leave Agent 2's `wallet_sqlite_open` call-site swap intact when rebasing.
- `lib/test/src/test_wallet_controller_rollback.c`  **(create)**
- `lib/test/spec/spec_e2e_wallet_restart.c`  **(create)**
- `lib/test/CMakeLists.txt` / `lib/test/Makefile` — wire in new tests

### Files you MUST NOT touch

- `lib/wallet/src/wallet_sqlite.c` / `wallet_sqlite.h` (Agent 2)
- `lib/util/include/util/result.h` / `result.c` (Agent 2 creates; you consume)
- `app/models/src/wallet_key.c` / `wallet_sapling_key.c` (Agent 2) — except for registering an `after_save` hook for backup, which is additive via `ar_register_after_save` and safe even with a parallel edit. If conflict, Agent 2's changes win; you rebase.

If you hit a file not listed, ask in PR description.

---

## Deliverables

### D1. Controller rollback on persistence failure

Target: `rpc_importprivkey` in `app/controllers/src/wallet_controller.c` (current line 636).

Rewrite per plan §5.4. Key changes vs. current code:

- Call `db_wallet_key_save` (new model function from Agent 2) instead of raw `wallet_sqlite_write_key`.
- Check its `zcl_result`. On non-ok: set RPC error body with code+message, `LOG_FAIL`, return.
- ONLY add to `ctx->wallet->keystore` AFTER persistence succeeds.
- Readback verification: call `wallet_sqlite_read_single_key` immediately after save. Compare returned privkey byte-for-byte. Mismatch → `db_wallet_key_delete` + keystore rollback + error.
- Trigger `wallet_backup_service_on_key_change()` after readback passes.
- Preserve existing post-save UTXO scan (`db_utxo_list_for_address`) — move it to run AFTER readback succeeds.

Same pattern, same file, for:
- `rpc_getnewaddress` — but the key came from keypool; rollback means "put it back in the pool".
- `rpc_keypoolrefill` — on flush failure, shrink keypool back.
- `rpc_importaddress` (if present).
- Each `wallet_shielded_controller.c` key import / z_getnewaddress.

### D2. Wallet canary

Create `lib/wallet/src/wallet_canary.c` and `lib/wallet/include/wallet/wallet_canary.h` per plan §5.3.

```c
struct zcl_result wallet_canary_run(sqlite3 *db);
```

- Uses a dedicated `wallet_canary` table (INTEGER PRIMARY KEY id=1, BLOB probe, INTEGER ts).
- Schema init in `app/models/src/database.c` — add the CREATE TABLE alongside the other wallet tables at line ~148.
- Each invocation: BEGIN → INSERT OR REPLACE with 32 fresh random bytes + `time(NULL)` → SELECT probe → memcmp → COMMIT → DELETE on failure path.
- Return `ZCL_OK` / `WSQL_CANARY_WRITE_FAIL` / `WSQL_CANARY_READ_MISMATCH`.

Called from boot after `wallet_sqlite_open` succeeds. Failure transitions to STATE_E per plan §7.

### D3. Boot state machine

Rewrite `config/src/boot.c` wallet init region per plan §7.

- Replace the current flat `if num_keys == 0 { topup; flush }` block with explicit `enum wallet_boot_state` handling.
- STATE_D, STATE_E, STATE_F abort paths print the exact message in plan §7 and call `exit(1)` after `fprintf(stderr, ...)`.
- Preserve the wallet-backup-service-start path at the end of the init region.
- No change to the LevelDB legacy-wallet migration code unless it also fails silently — audit and fix if so.

### D4. `wallet_backup_service` extensions

Current service runs on a timer. Add:

```c
/* Triggers an immediate backup cycle. Rate-limited to once per
 * WALLET_BACKUP_MIN_INTERVAL_SECONDS (default 30). Safe to call from
 * any thread. Idempotent. Logs via LOG_ERR on failure (best-effort). */
void wallet_backup_service_on_key_change(void);
void wallet_backup_service_on_keypool_topup(void);
```

Internal implementation: signal a condition variable watched by the existing backup thread. On signal, run the backup now; reset the timer. Rate limit to protect against hot loops (1000 `getnewaddress` in a script shouldn't write 1000 JSONs — debounce to 1 write per 30s).

After Agent 2 merges `wallet_key.c` with `AR_BEGIN_SAVE`, register `wallet_backup_service_on_key_change` as an `after_save` callback on the `wallet_key` model:

```c
/* In some init function called once during boot, AFTER wallet_init(). */
ar_register_after_save(&wallet_key_callbacks, wallet_backup_service_on_key_change_trampoline);
```

Trampoline adapts the AR callback signature to the service's void-void signature.

### D5. View — `getwalletinfo` persistence block

Per plan §6. In `wallet_diagnostic_controller.c` (or wherever `rpc_getwalletinfo` lives — grep):

```c
/* Add after existing fields in the getwalletinfo response object. */
struct wallet_sqlite_health h = wallet_sqlite_get_health(ctx->wallet_db,
    (int)ctx->wallet->keystore.num_keys);

json_push_kv_object(result, "persistence");
json_push_kv_bool(result, "healthy",
    h.open && h.canary_ok && !h.mismatch);
json_push_kv_bool(result, "open",           h.open);
json_push_kv_bool(result, "canary_ok",      h.canary_ok);
json_push_kv_int (result, "canary_last_ok_ts", h.canary_last_ok_ts);
json_push_kv_int (result, "row_count",      h.row_count);
json_push_kv_int (result, "keystore_count", h.keystore_count);
json_push_kv_bool(result, "mismatch",       h.mismatch);
json_push_kv_str (result, "last_error",     h.last_error);
/* close object */
```

Adjust to actual json helper APIs (grep for patterns in existing controllers).

### D6. Tests

- `lib/test/src/test_wallet_controller_rollback.c` per plan §8.1 — mock service layer, assert keystore + backup untouched on failure.
- `lib/test/spec/spec_e2e_wallet_restart.c` per plan §8.2 — fork a daemon, restart cycle, verify key survives. This is the regression test for the current bug.

The e2e test is the strongest proof. It should:
1. Fork child process that runs `zclassic23 -datadir=/tmp/e2e-$$ -rpcport=<random>` with fresh datadir.
2. Wait for RPC ready.
3. `importprivkey K1`.
4. `dumpprivkey <addr1>` → assert returns K1.
5. SIGTERM child, wait for clean exit (max 30s).
6. Fork child again with SAME datadir.
7. Wait for RPC ready.
8. `dumpprivkey <addr1>` → MUST return K1. If not, fail.
9. Cleanup datadir.

This is the test that would have prevented the bug from shipping.

---

## Done when

- [ ] `make lint` green.
- [ ] `make ci` green (including new tests).
- [ ] `./test_zcl` all green.
- [ ] Integration test `spec_e2e_wallet_restart` passes in CI.
- [ ] On rhett's node (after both A2 and A3 merged): `getwalletinfo` returns `persistence.healthy: true` with `canary_last_ok_ts` within the last minute.
- [ ] PR title: `a3: wallet controller guardrails, canary, boot state machine, e2e restart test`
- [ ] PR description links plan §6, §7, §9.

---

## Commit style

Same conventions as Agent 2. Suggested sequence:

1. `add wallet_canary table + self-test routine`
2. `wallet_backup_service: add on_key_change trigger, debounced`
3. `controllers: check flush/save result, rollback keystore on failure (importprivkey)`
4. `controllers: same rollback pattern for getnewaddress + keypoolrefill`
5. `controllers: shielded + rescan RPC rollback`
6. `boot: explicit state machine, abort on STATE_D/E/F`
7. `view: getwalletinfo exposes persistence health`
8. `test: wallet controller rollback unit test`
9. `test: spec_e2e_wallet_restart — regression test for boot-loss bug`

---

## Gotchas

- **Rate-limit the backup writer.** If the user runs a 1000-address import script, naive implementation writes 1000 JSONs and hammers disk. Debounce to 1 write per 30s; last write wins.
- **Don't let the canary pollute backups.** Explicitly skip `wallet_canary` table in `wallet_backup_service`.
- **Lock ordering.** Controller may hold `w->cs` (wallet mutex). Callbacks from `after_save` run synchronously inside that lock. Any I/O done in the callback (e.g. backup write) must NOT take a lock the controller already holds. Use a queue + background thread if in doubt.
- **Existing `wallet_backup_service` thread lifecycle.** Already started in `config/src/boot.c` line ~555. Don't double-start. Add your new signal path using its existing thread.
- **Boot abort vs systemd restart loop.** Your STATE_D/E/F aborts will hit `Restart=on-failure` → systemd re-execs → same abort. This is correct: operator intervention is required. Log the recovery instructions clearly. Current `zclassicd-rhett.service` has `RestartSec=30` which gives time to read logs before re-exec.
- **e2e test in CI.** May need a helper script to spawn the daemon. Look at existing `lib/test/spec/` for precedents before writing from scratch.
- **getwalletinfo is on the MCP surface.** Adding a new field is additive, safe. Do not rename or remove existing fields. Per `CLAUDE.md`, every MCP handler must set an error body — your additions shouldn't regress this.

---

## Hand-off

When complete:

```
cd ~/zclassic23-3
git push origin a3/wallet-controller-guardrails
gh pr create --title "a3: wallet controller guardrails, canary, boot state machine, e2e restart test" \
             --body "$(cat <<'EOF'
## Summary
Implements Agent 3 scope of WALLET_PERSISTENCE_PLAN.md.

- Controller rollback on persistence failure (importprivkey, getnewaddress, z_*)
- Wallet canary table + boot-time self-test
- Boot state machine with explicit abort on STATE_D/E/F
- wallet_backup_service gains on_key_change trigger (debounced)
- getwalletinfo exposes persistence health block
- Tests: test_wallet_controller_rollback, spec_e2e_wallet_restart (regression test for current bug)

## Plan
See WALLET_PERSISTENCE_PLAN.md §4 (MVC diagram), §5.4 (controller contract), §7 (boot state machine).

## Dependency
Requires a2/wallet-sqlite-result-types merged (for zcl_result and migrated wallet_sqlite_* signatures).

## Test plan
- [x] make lint
- [x] make ci
- [x] ./test_zcl (new tests pass)
- [x] spec_e2e_wallet_restart passes (this is the regression test for the bug)
- [x] live smoke: getwalletinfo on test node shows persistence.healthy: true

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

Stop there. Do not merge. Claude will review, request changes if needed, and merge after Agent 2 is in.
