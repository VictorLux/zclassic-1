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
