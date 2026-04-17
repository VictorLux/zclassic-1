# ZClassic23 Hardening Checklist

**Snapshot date:** 2026-04-17
**Status:** zclassic23 running @ block 3,080,656, 19 peers, balance 0 (wallet-persistence bug still live — agents 2/3 in flight). zclassicd manual instance holds `.zclassic` lock; systemd `zclassicd-rhett` restart-looping on lock conflict.

This doc is the output of a four-front defensive audit (persistence, network/Tor, consensus/sync, ops/CI). It is *pedantic by design*: every item names file:line, classifies blast radius, and sketches a minimal fix. Items marked "in flight" are being addressed in worktrees `~/zclassic23-2` and `~/zclassic23-3` — don't redo them.

## Priority scheme

| Tier | Meaning |
|------|---------|
| **P0** | Blocks accepting real ZCL. Don't send funds to this node until fixed. |
| **P1** | Silent data loss, consensus drift, or deploy-safety regression. |
| **P2** | Hangs, DoS vectors, observability holes. |
| **P3** | Test coverage, tool safety, style. |

---

## P0 — Wallet persistence (in flight)

See [`WALLET_PERSISTENCE_PLAN.md`](./WALLET_PERSISTENCE_PLAN.md) and `tasks/AGENT_2_WALLET_SQLITE.md` / `tasks/AGENT_3_WALLET_GUARDRAILS.md`.

Acceptance gate for sending funds: `getwalletinfo.persistence.healthy == true` AND a round-trip `import → SIGKILL → restart → dumpprivkey` integration test is green.

---

## P1 — Build, CI, deploy (the smallest diffs, biggest blast radius)

### P1.1 Enable `-DZCL_AR_ENFORCE`
- **Where:** `Makefile` (CFLAGS, around line 37)
- **Now:** The enforcement flag is documented in `DEFENSIVE_CODING.md` but never actually set. Compiler warnings/errors for raw `sqlite3_step` in app code are not emitted.
- **Fix:** Add `-DZCL_AR_ENFORCE` to `CFLAGS`. Compile. Expect errors from files listed in P2.1 — fix or mark `// raw-sql-ok` with justification.
- **Blast radius if unfixed:** Any future code can reintroduce the exact class of bug we are fixing right now.

### P1.2 `make deploy` must lint, WAL-checkpoint, and verify RPC
Current (`Makefile:298-302`):
```
deploy: zclassic23
	install -m 644 deploy/zclassic23.service …
	systemctl --user daemon-reload
	systemctl --user restart zclassic23
	sleep 2 && systemctl --user is-active zclassic23 && echo "Deployed."
```
Problems:
- No lint gate. Bad code ships.
- No WAL checkpoint before restart → window where SIGTERM arrives with unflushed WAL.
- `is-active` is true if the process exists for >2s. A binary that segfaults on first RPC call still prints "Deployed."

**Fix sketch:**
```
deploy: lint zclassic23
	./tools/wal_checkpoint $(HOME)/.zclassic-c23/node.db
	install -m 644 deploy/zclassic23.service …
	systemctl --user daemon-reload
	systemctl --user restart zclassic23
	for i in 1 2 3 4 5 6 7 8 9 10; do \
	    curl -sS -u … 127.0.0.1:18232 -d '{"method":"getblockcount"}' && break; \
	    sleep 1; \
	done
	./tools/zcl-rpc getblockcount | grep -q '^[0-9]' || { echo "DEPLOY FAILED"; exit 1; }
	echo "Deployed + RPC live."
```

### P1.3 Raw-sqlite lint must FAIL, not WARN
- **Where:** `Makefile:506-514` — `check-raw-sqlite` target emits `WARNING:` and then prints `OK:`, exits 0.
- **Fix:** Replace the WARNING path with `echo "FAIL: …"; exit 1`. Depends on P1.1 land and violators migrated.
- **Violators currently visible** (sample): `lib/wallet/src/wallet_sqlite.c:224,236,301,313,356,371,393,407,430,447`; `app/controllers/src/{wallet_shielded,api,wallet_view_projection,store}_controller.c`. Agents 2/3 will clean `wallet_sqlite.c`; the rest remain.

### P1.4 Sync `deploy/zclassic23.service` with live hardening
- **Drift:** I added `OOMScoreAdjust=-500` and `MemoryHigh=6G` to the live unit at `~/.config/systemd/user/zclassic23.service` but the deploy template in `deploy/zclassic23.service` **does not have them**. On next `make deploy` the hardening silently regresses.
- **Fix:** Port both keys into `deploy/zclassic23.service`. While there, also add:
  - `TimeoutStopSec=300` (WAL flush window)
  - `KillMode=control-group` (ensure Tor pthread dies with parent)
  - `StartLimitIntervalSec=300` + `StartLimitBurst=3` (stop restart thrash)
  - `RestartSec=10` (already set — keep)
  - optional: `WatchdogSec=60` (requires `sd_notify` heartbeat in main loop — defer)

---

## P1 — Persistence-layer silent errors (analogous to the wallet bug)

### P2.1 Unchecked `sqlite3_exec` / `node_db_exec` in `app/models/src/database.c`
Verified line references:
| Line(s) | Operation | Problem |
|---|---|---|
| 720-727 | `CREATE TABLE IF NOT EXISTS schema_migrations` via `node_db_exec` with return ignored | If create fails, `node_db_schema_version()` returns 0, **all migrations re-apply** next boot |
| 883-889 | Schema v6 `ALTER TABLE … ADD COLUMN address` + `CREATE INDEX` (comment says "ignore errors — column may already exist") | Intentional, but unlogged. If ALTER fails for a *new* reason (disk full), state is silently wrong |
| 893 | `node_db_state_set(ndb, "schema_version", &v, sizeof(v))` return ignored | If write fails, counter diverges → next boot re-applies v6 |
| 1399 | `sqlite3_exec(DB_DROP_INDEXES[i])` | Silent failure: indexes stay → turbo-mode IBD has no effect; or indexes gone but marked rebuilt → queries 10× slower |
| 1407 | `sqlite3_exec(DB_CREATE_INDEXES[i])` | Same |
| 1414-1417 | `PRAGMA synchronous=OFF`, `cache_size`, `wal_autocheckpoint=0` | Silent failure: turbo mode partial, unbounded WAL |

**Fix pattern:**
```c
int rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
if (rc != SQLITE_OK) {
    LOG_FAIL("db", "exec %s: %s", sql, errmsg ? errmsg : "?");
    sqlite3_free(errmsg);
}
```
In places where errors are intentionally tolerable (v6 ALTER on existing column), explicitly whitelist by error code: `if (rc != SQLITE_OK && !strstr(errmsg, "duplicate column"))`.

### P2.2 `lib/coins/src/utxo_commitment.c` silent false on `sqlite3_prepare_v2`
- **Lines:** 195, 211, 307, 322 — `if (sqlite3_prepare_v2(...) != SQLITE_OK) return false;` with no `LOG_FAIL` or context.
- **Blast radius:** UTXO commitment rolls back silently → FlyClient verifiers next time compute the wrong commitment → peers disagree.
- **Fix:** `LOG_FAIL("utxo_cmt", "prepare %s: %s", sql, sqlite3_errmsg(db))` on every such path.

### P2.3 Schema `strstr` suppression
- **Where:** `app/models/src/database.c:268-276` silently skips any SCHEMA entry matching `"ALTER TABLE"` or `"CREATE INDEX"`.
- **Blast radius:** Failed schema changes go unseen. Combined with P2.1 this can create replicas with different effective schemas on different versions.
- **Fix:** Migrate all ALTER/INDEX statements to versioned migration blocks. Remove the suppression.

---

## P1 — Consensus / sync atomicity

### P2.4 `ConnectBlock` flush → tip update ordering
- **Where:** `lib/validation/src/process_block.c:1167-1233`.
- **Now:** `coins_view_cache_flush` at 1167; if it fails, flow reaches `update_tip()` at 1233 anyway. Block is marked `VALID_SCRIPTS`, tip advances, but its UTXOs never landed.
- **Fix:** Treat flush failure as fatal for that tip update. `if (!flush_ok) { state_halt_with_reason(REASON_CORRUPT_FLUSH); return; }`.
- **Blast radius:** CONSENSUS_SPLIT — next block's validation fails indeterministically based on which UTXOs happened to survive.

### P2.5 Snapshot SHA3 verified AFTER chunks are written
- **Where:** `app/services/src/snapshot_sync_service.c:454-551`.
- **Now:** Chunks land in DB, then SHA3 verify; on mismatch, wipe policy triggers (535). Crash between wipe and turbo-mode exit → half-wiped UTXO set.
- **Fix:** Stream-hash SHA3-256 as chunks arrive. Write chunks to a staging table (or memory buffer bounded by snapshot-size config). Only `INSERT INTO utxos SELECT * FROM utxos_staging` if final hash matches commitment.

### P2.6 `snapsync_commit_tip` not through CSR atomic barrier
- **Where:** `app/services/src/snapshot_sync_service.c:474-492, 1401`.
- **Now:** Regular block sync's `process_block_commit_tip()` routes tip + `coins_best_block` through the CSR atomic barrier (the fix for the 2026-04-10 UTXO wipe). Snapshot sync does not.
- **Blast radius:** DATA_LOSS — snapshot completes, crash between commits → replay on restart redundantly writes UTXOs that were already there, corrupting count / referential integrity.
- **Fix:** Route snapshot finalization through the same CSR barrier.

### P2.7 Mempool unbounded ancestor / descendant depth
- **Where:** `lib/validation/src/txmempool.c:228-232`.
- **Now:** 300 MB size cap, no count / chain-depth cap.
- **Attack:** 1 M chained 250-byte zero-fee txns stall script verification workpool for hours.
- **Fix:** Bitcoin-Core-style caps (25 ancestors, 25 descendants, 101 KB ancestor size, 101 KB descendant size).

---

## P2 — Network defensive (verified)

### P3.1 `download.c:317` negative age skips timeout
- **Code:** `int64_t age = now - s->request_time; if (age < dl_get_request_timeout_secs()) continue;`
- **Failure mode:** If `request_time` is corrupted to a future value (clock skew, replay, overflow from a 32→64 cast), `age < positive_timeout` is always true; the slot never times out; block download hangs.
- **Fix:** `if (age < 0) age = 0;` before the compare, OR `if (age < 0 || age < timeout) continue;`.

### P3.2 Tor bootstrap failure is silent
- **Where:** `lib/net/src/tor_integration.c:202-244`.
- **Now:** Tor pthread exits → `atomic_store(&g_tor_thread_done, true)` → main node keeps serving clearnet but `.onion` is dead. No event, no health-red.
- **Fix:** On thread exit, emit `EV_TOR_BOOTSTRAP_FAILED` (new event type). `zcl_onion_status` must return `healthy=false`. Optionally: exponential-backoff restart of the Tor thread.

### P3.3 `addrman` path accepts peer-supplied localhost addresses
- **Where:** `lib/net/src/msgprocessor.c:750-751` `process_addr()`.
- **Now:** A peer can send us an `addr` message claiming `127.0.0.1:8033` is another node. We insert into addrman. Later outbound selection may try to dial it.
- **Fix:** In `addrman_add`, reject any `127.0.0.0/8`, `::1/128`, and RFC1918 ranges *unless* the learning source is an already-trusted peer.
- **Blast radius:** Confusion of outbound slots; possible self-connect loop.

---

## P2 — Systemd hardening

Already live on my node (`~/.config/systemd/user/zclassic23.service`): `OOMScoreAdjust=-500`, `MemoryHigh=6G`, `Restart=on-failure`, `RestartSec=10`.

Missing (add to both `deploy/zclassic23.service` and live):
- `TimeoutStopSec=300`
- `KillMode=control-group`
- `StartLimitIntervalSec=300` + `StartLimitBurst=3`
- `PrivateTmp=yes` (safe; no shared /tmp usage)
- `NoNewPrivileges=yes`
- `ProtectSystem=strict` with `ReadWritePaths=%h/.zclassic-c23 %h/zclassic23/vendor/tor` (verify Tor data dir path)
- Later: `WatchdogSec=60` after plumbing `sd_notify(0, "WATCHDOG=1")` into the main event loop

For `zclassicd-rhett.service` (currently in a restart loop due to manual-zclassicd lock conflict):
- Add `ExecStartPre=/usr/bin/pgrep -x zclassicd` check that refuses to start if one already exists.
- Or just stop the manual instance (see P0-ops below).

---

## P3 — Tests

### P3.4 Turn on `crash_recovery_test` in `make ci`
- **Where:** `tools/crash_recovery_test.c` exists but only runs when `ZCL_CRASH_DATADIR` is set.
- **Fix:** `make ci` should seed a throwaway datadir, run the test, tear down. This one test class would have caught the wallet-persistence bug in pre-commit.

### P3.5 Controller-layer integration tests
- **Gap:** `lib/wallet/src/wallet_sqlite.c` has `test_wallet_sqlite_enc.c`. `app/controllers/src/wallet_controller.c` has no dedicated test. Yet the bug lives at the controller layer (ignored return from `wallet_sqlite_flush`).
- **Fix:** `lib/test/src/test_wallet_controller_rpc.c` — cover `rpc_getnewaddress`, `rpc_importprivkey`, `rpc_sendmany`; each asserts that a subsequent `dumpprivkey` after simulated restart returns the key.

### P3.6 Adversarial-peer fuzz harness
- **Gap:** No fuzzer for malformed P2P messages.
- **Fix:** Existing `FUZZER_FINDINGS.md` suggests prior work; revive and wire into `make ci` with a bounded runtime (e.g., 60s per fuzz corpus).

---

## P3 — Tool safety

### P4.1 `tools/wal_checkpoint.c` has unguarded `DELETE`
- **Lines:** 51, 61-62 — `sqlite3_exec(db, "DELETE FROM utxos", ...)`.
- **Fix:** This path should never be in `wal_checkpoint` — it's name-shadowed with a destructive sibling. Either rename the tool or require `--wipe-utxos` flag + interactive confirmation + `COUNT(*) < 1000` guard (memory rule: never DELETE above tip without count check).

### P4.2 Audit all `tools/*` for destructive side effects
- Grep `DELETE`, `DROP`, `remove_all`, `unlink`, `rm ` in `tools/*.c` and `tools/*.sh`. Each destructive tool needs a `--force` flag + what-would-happen dry-run mode.

---

## Not-a-bug (audit false positives — recording so we don't re-chase)

- **`lib/net/src/compact_blocks.c:420`**: `LOG_FAIL` macro expands to `return false`, so the `num_short > MAX_COMPACT_BLOCK_TXNS` guard actually returns before the malloc. Overflow path is blocked. Confirmed in `lib/util/include/util/log_macros.h:27-33`.
- **`app/services/src/header_sync_service.c:576`**: `last_useful_headers_time` *is* updated on every good batch in `syncsvc_note_headers_received` at line 333. The `[headers] interval=120s` log spam is benign (peers connected but not actively delivering; `stale_count=0` because zero were delivered, not because peer was bad).

---

## Ownership / next-wave assignment proposal

When agents 2 and 3 land:

| Agent | Worktree | Scope |
|---|---|---|
| Agent 4 | `~/zclassic23-4` | **P1.1 – P1.4** (build/CI/deploy). ~200 LOC diff. Smallest, highest leverage. Do first. |
| Agent 5 | `~/zclassic23-5` | **P2.1 – P2.3** (database.c + utxo_commitment silent errors). Must coordinate with agent 2's migrations. |
| Agent 6 | `~/zclassic23-6` | **P2.4 – P2.7** (consensus atomicity). Invasive — bg-validation + CSR barrier. Requires the most care. |
| Agent 7 | `~/zclassic23-7` | **P3.1 – P3.3** (network) + **P3/P4 systemd + tool safety**. |

Each agent reads this file first, then its own `tasks/AGENT_N_*.md`. PR titles mirror the section IDs (`a4: P1.1-P1.4 build/CI/deploy hardening`).

---

## Operational right-now

- **zclassicd lock conflict (blocking).** Manual zclassicd PID 3273469 (49 min uptime, started with `-daemon`) holds `~/.zclassic/.lock`. Systemd `zclassicd-rhett` is in 30-second restart-loop. Decision pending from rhett: `kill 3273469 && systemctl --user reset-failed zclassicd-rhett`, OR `systemctl --user disable --now zclassicd-rhett` and keep the manual instance.
- **zclassic23 memory headroom.** `Memory: 1.7G (high: 6.0G, peak: 5.2G, swap: 462M peak: 1.7G)` — the 5.2G peak is concerning; `MemoryHigh=6G` means we're inside the throttle band. Measure whether any unbounded cache grew during snapshot sync; if so, cap it.
- **WAL health.** `node.db-wal` is 2.5 MB, healthy. No action.

---

## Acceptance: "rock solid" defined

The node is **not** rock-solid until all of these hold simultaneously:

1. [ ] `getwalletinfo.persistence.healthy == true` for ≥ 7 days across at least one scheduled restart and one unscheduled SIGKILL.
2. [ ] `make lint` fails on raw `sqlite3_step` in app code, unlogged error returns in services, and unchecked mallocs.
3. [ ] `make ci` includes `crash_recovery_test` with SIGKILL + restart + state-survived assertion.
4. [ ] `make deploy` won't announce success if post-restart RPC is unreachable for ≥ 30 s.
5. [ ] All P1 items closed. P2 items tracked with owners. P3 items scheduled.
6. [ ] Tor bootstrap failure surfaces in `zcl_status.health`.
7. [ ] Mempool caps: no chain > 25 ancestors.
8. [ ] Every destructive `tools/*` requires a `--force` flag.

Only after all eight boxes check do we route test funds through this node.

---

# Round 2 — Extended audit (2026-04-17, same day)

Covers subsystems the first pass skipped: MCP server, Sapling crypto, block-file storage, crash-recovery paths, and what `make lint` actually catches *today*. All findings below have been line-verified by reading the code, not just trusting the audit summary. False positives recorded at the end.

## R2.1 — CATASTROPHIC: silent full-DB wipe on SQLite corruption

**File:** `app/models/src/database.c:533-542`
```c
if (!db_quick_check_ok(ndb->db)) {
    fprintf(stderr, "db: %s is malformed; rebuilding fresh SQLite state\n", path);
    sqlite3_close(ndb->db);
    ndb->db = NULL;
    db_quarantine_files(path);             // renames *.db → *.db-corrupt-<ts>
    if (!db_open_raw(&ndb->db, path)) { …return false; }
}
/* falls through to create_schema(ndb) — empty DB */
```
This is the same class of bug as the wallet-persistence one: a silent "self-heal" that throws away everything. Violates the explicit memory rule: **NEVER wipe UTXOs above tip; reset tip first**. On an OOM-induced partial corruption it nukes:
- `wallet_keys` (every unspent address, rerolls keypool)
- `utxos` (UTXO set — triggers full resync)
- `blocks`, `block_index`, `schema_migrations` (everything)

**Priority:** P0 — same tier as the wallet bug. Fix must land before any node self-heals corruption.
**Fix direction:**
1. Split the response by what `quick_check` reports — table-level corruption vs global.
2. Before quarantine, read out `wallet_keys` into a recovery file (encrypted-at-rest preserved).
3. Never open a fresh schema in the same boot; prompt via an explicit `--rebuild-fresh-db` flag or halt STATE_D-style.
4. Integration test: fuzz-flip bytes in `node.db`, restart, assert that `wallet_keys` survive OR that node halts (never silently wipes).

## R2.2 — CRASH: Sapling witness buffer underread (attacker-controllable)

**File:** `lib/sapling/src/sapling_prover_c23.c:167-171`
```c
uint8_t depth = witness[0];
if (depth != 32) return false;
for (int i = 0; i < 32; i++) {
    memcpy(wit.auth_path[i], witness + 1 + i * 33, 32);
    wit.auth_path_bits[i] = witness[1 + i * 33 + 32] != 0;
}
```
`zclassic_sapling_spend_proof` has no `witness_len` parameter. If caller supplies a buffer shorter than 1057 bytes (1 + 32×33), this reads past the end. Inputs come from z_sendmany / receive parsing — network-reachable on the proving path, possibly attacker-reachable on the verifying side.
**Priority:** P1. Add `size_t witness_len` parameter; check `witness_len >= 1 + depth * 33` before the loop. Add a regression test with a 100-byte witness.

## R2.3 — CRYPTO_LEAK: compiler may optimize away secret zeroing

**File:** `lib/sapling/src/note_encryption.c:126,127,150,151`
```c
memset(dhsecret, 0, 32);   // ← compiler may elide
memset(key, 0, 32);        // ← compiler may elide
```
`dhsecret` and `key` are ECDH-derived shielded-note decryption keys. After use they must be scrubbed; plain `memset` on a soon-to-be-deallocated stack variable is a classic dead-store the optimizer removes. The codebase already has `memory_cleanse` (used correctly in `groth16_prover.c:598,788-789`).
**Priority:** P1. 4-line mechanical fix: replace 4 `memset` calls. Grep `lib/sapling/` for `memset.*, 0, 32` to catch analogues.

## R2.4 — CRASH: MCP net handler continues after failed malloc

**File:** `tools/mcp/controllers/net_controller.c:124-139` (`h_zcl_onion_health`)
```c
char *body = zcl_malloc(512, "onion_health_body");
if (!body) {
    res->error = MCP_ERR_INTERNAL;
    snprintf(res->error_message, sizeof(res->error_message), …);
    LOG_ERR("mcp.net", "malloc failed for onion_health body (512 bytes)");
}   // ← missing return
if (!addr) {
    snprintf(body, 512, …);   // ← body is NULL, crash
```
Verified: `LOG_ERR` expands to `return -1` in the macro set, but **the handler does not invoke the macro**; it manually calls `LOG_ERR` as a function-style log (line 130 is just `LOG_ERR(...)` without `do{…; return -1;}while(0)`). So control falls through to the NULL-deref.
**Priority:** P1. Add `return -1;` after the log line. Then grep the rest of `tools/mcp/controllers/` for the same anti-pattern — this is likely not unique.

## R2.5 — DATA_LOSS: coins_view_sqlite flush atomicity

**File:** `lib/storage/src/coins_view_sqlite.c:283-501` (batch write path)

The flush wraps UTXO writes in a `SAVEPOINT`, but the best-block pointer update happens *after* the savepoint release. SIGKILL in that window leaves UTXOs for block N+1 in the DB while `coins_best_block` still points to block N. On restart the node sees inconsistent state; there is no post-boot audit that compares max UTXO height to `coins_best_block`.

This is the same bug class as the 2026-04-10 UTXO wipe, and the same class as the snapsync atomicity gap (R1/P2.6 above).

**Priority:** P1. Move the `coins_best_block` write inside the savepoint. Add a boot-time integrity check: `SELECT MAX(height) FROM utxos` must equal `coins_best_block`. If off, halt with STATE_D reason `UTXO_TIP_MISMATCH`, do not self-heal.

## R2.6 — HIGH: MCP handlers build RPC params with unbounded snprintf

**Files:**
- `tools/mcp/controllers/app_controller.c:44`
- `tools/mcp/controllers/wallet_controller.c:76, 244`
- `tools/mcp/controllers/ops_controller.c:75-95`

Pattern: `snprintf(params, 256, "[\"%s\"]", user_string)` where `user_string` came from untrusted JSON. 256 bytes is tight for anything with an address or txid; bigger wallet params can truncate. Truncation isn't detected (`snprintf` return is ignored), so the RPC call silently fires with a malformed JSON array.
**Priority:** P2. Introduce a helper `mcp_build_params(buf, buflen, "[%s]", …)` that: (a) returns error if truncated, (b) JSON-escapes the string properly. Migrate all DEFINE_PT handlers to it.

## R2.7 — HIGH: block-file write lacks length-prefix + CRC wrapping

**File:** `lib/storage/src/disk_block_io.c:181-213`

Three separate `fwrite` calls (magic / size / data) then `fdatasync`. SIGKILL between #1 and #2 leaves a file that parses as "block with random size". Read path trusts the size field without CRC; deserialization may succeed into garbage, caught only by hash comparison one layer up.
**Priority:** P2. Wrap the three writes in a single atomic-ish record: `[magic][size][crc32][data]`. Verify CRC on read; return `false` (and log) on mismatch.

## R2.8 — MEDIUM: assertion-based input validation in Sapling Merkle tree

**File:** `lib/sapling/src/incremental_merkle_tree.c:37`
```c
assert(depth <= MAX_TREE_DEPTH);
```
If any public deserializer reaches this with `depth` from the wire, a release build with `-DNDEBUG` silently continues; debug build crashes. Either way the validation is wrong. Replace with `if (depth > MAX_TREE_DEPTH) return false;`.

## R2.9 — MEDIUM: Sapling verification functions return bare bool

**File:** `lib/sapling/src/sapling.c:484-577` (multiple sites)

`sapling_check_spend` / `sapling_check_output` return `false` for: malformed curve point, malformed proof, Groth16 mismatch — the caller cannot distinguish "hostile input" from "internal error." Log-only coverage is one `fprintf` in the whole file.
**Priority:** P2. Migrate to `struct zcl_result` once agent 2 lands `lib/util/result.h`. Every `return false` gets a distinct `SAPLING_ERR_*` code.

## R2.10 — MEDIUM: LevelDB reads skip checksum verification

**File:** `lib/storage/src/dbwrapper.c:73-78`
```c
leveldb_readoptions_set_verify_checksums(g_read_options, 0);
```
Comment on the line attributes past missing-UTXO incidents to this setting. Performance tradeoff is real, but the default should be **verify on**; turn off only behind a boot-time flag while recovery is suspected.
**Priority:** P2. Flip default. Hide the `off` behind a flag.

## R2.11 — MEDIUM: `make lint` raw-sqlite warning has 70+ hits in app code

Ran `make lint` right now. `check-raw-sqlite` target emits 70+ lines and then still exits 0 because it's WARNING-only. Most prolific offenders:
- `app/controllers/src/explorer_factoids.c` (16 hits, line 169 onward)
- `app/controllers/src/explorer_stats.c` (13 hits)
- `app/controllers/src/blockchain_controller.c` (11 hits)
- `app/models/src/database.c` (6 hits)
- `app/models/src/utxo.c` (1 hit)

These are mostly **reads**, which is safer than writes, but they still bypass the defensive layer. Incidentally, `make lint` *does* fail today on a different check: `app/services/src/node_health_service.c:71,82` — two `return -1` without logging in `get_rss_kb()` (reading `/proc/self/status`). Trivial 2-line fix but illustrates that lint-failing code is already on master.

**Priority (for the sqlite3_step cleanup):** P2 — wire P1.3 first so new violations can't be added, then batch-migrate the existing ones to `AR_STEP_ROW` / `AR_STEP_DONE`. Reads can use a simpler `AR_STEP_ROW_READONLY` wrapper that skips the write-validation hooks.

---

## Additional ownership proposal

Adding to the agent 4-7 plan from round 1:

| Agent | Worktree | Scope (round 2) |
|---|---|---|
| Agent 5 | `~/zclassic23-5` | R2.1 (silent full-wipe) is now the **highest priority in this agent's bucket** — do it first. Also R2.5 (coins_view atomicity), R2.10 (LevelDB checksums). |
| Agent 6 | `~/zclassic23-6` | Add R2.7 (block-file CRC). |
| Agent 7 | `~/zclassic23-7` | Add R2.4, R2.6 (MCP). |
| **Agent 8 (new)** | `~/zclassic23-8` | **Sapling hardening**: R2.2, R2.3, R2.8, R2.9. Adds proof-verification fuzz corpus to `lib/test/fuzz_seeds/sapling/`. Migrates verification functions to `zcl_result` once agent 2 lands `result.h`. |

---

## Extended acceptance gates (in addition to the 8 from round 1)

9. [ ] No self-healing DB-quarantine path that deletes user data without explicit operator action.
10. [ ] Sapling witness-parsing bounds checked; fuzz corpus lives in `lib/test/fuzz_seeds/sapling/`.
11. [ ] All ECDH secret material scrubbed with `memory_cleanse`, verified by a grep-level lint rule.
12. [ ] Every MCP handler returns immediately after `LOG_ERR` or `res->error = ...`.
13. [ ] Block-file writes include a CRC that is verified on read.
14. [ ] `make lint` exits 0 — no WARNING-only checks, no FAIL.

---

## Round 2 false positives (not bugs, recorded to avoid re-chasing)

- `disk_block_io.c` **does** call `fdatasync` (line 206) before recording the block position, so `pread` at the recorded offset is safe from SIGKILL. Contrary to the R2 audit's initial framing.
- `app/services/src/utxo_recovery_service.c:47-66` has a policy-gated wipe (`ZCL_MAX_UTXO_WIPE_ROWS` + operator-prompt callback) — *not* a silent wipe. Good example of the pattern R2.1 should adopt.
- `note_encryption.c:109-111, 132-134` also zero scratch buffers; those locations are the exact set to convert (sprout_note_encrypt, sprout_note_decrypt, symmetric halves) — not just the sapling ones.

