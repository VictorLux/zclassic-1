# AGENT3 — Wave 19: Service Hardening + Operational Depth

**Worktree:** `~/zclassic23-3`
**Workflow:** push directly to `master` after `./test_zcl` passes
**Coordinator:** AGENT1 (main `~/zclassic23`)
**Peer:** AGENT2 (`~/zclassic23-2`) — sync pipeline fix, different files

---

## Context

Agent3's infrastructure layers are mature: wallet encryption, RPC timeout, peer bandwidth, tracing, WebSocket events, MCP surface (76 tools). Wave 19 shifts to hardening what exists and closing remaining lint/safety gaps across the codebase.

The node is stuck at 2,016,354 (AGENT2 is fixing the sync pipeline). Agent3's work is orthogonal — improve code quality, close defensive coding gaps, and add operational tools.

---

## Task 1: Eliminate Raw `sqlite3_step()` in Service Layer

`make lint` reports 18 raw `sqlite3_step()` calls in service code. These should use `AR_STEP_ROW`/`AR_STEP_DONE` wrappers or be marked `// raw-sql-ok` with justification.

### Files to fix:
- `app/services/src/block_index_loader.c` — 4 violations
- `app/services/src/utxo_recovery_service.c` — 7 violations
- `app/services/src/chain_state_repository.c` — 2 violations
- `app/services/src/node_health_service.c` — 2 violations
- `app/services/src/snapshot_sync_service.c` — 3 violations

### Rules:
- Read `DEFENSIVE_CODING.md` for the AR_STEP pattern
- If the `sqlite3_step` genuinely cannot use the wrapper (e.g., it's inside a low-level helper that IS the wrapper), mark it `// raw-sql-ok` with a one-line reason
- If it can use the wrapper, switch it
- After fixing, `make lint` should show ZERO service-layer violations

### Test:
- `make test` must still pass
- `make lint` must be cleaner

---

## Task 2: RPC Cookie Rotation Service

The RPC cookie file is generated once at boot and never changes. A long-running node uses the same cookie for weeks. Add automatic rotation.

### Files to create/modify:
- `lib/rpc/src/cookie_rotation.c` (new) — rotation logic
- `lib/rpc/include/rpc/cookie_rotation.h` (new) — public API
- `lib/rpc/src/httpserver.c` — wire rotation into the auth path

### What to implement:

```c
// cookie_rotation_start(const char *datadir, int interval_s)
//   - Default interval: 3600 (1 hour), env: ZCL_COOKIE_ROTATE_S
//   - Background thread regenerates .cookie file atomically (write tmp, rename)
//   - Old cookie valid for grace_period_s (default 60) after rotation
//   - Event: EV_RPC_COOKIE_ROTATED
//
// cookie_rotation_stop()
// bool cookie_rotation_check(const char *cookie)
//   - Returns true if cookie matches current OR grace-period-old cookie
```

### Test:
- Test rotation creates new cookie
- Test old cookie still valid during grace period
- Test old cookie rejected after grace period
- Test atomic file replacement

---

## Task 3: Sapling Key Material Scrubbing

After wallet encryption wraps keys, the plaintext copies in memory should be scrubbed.

### Files to modify:
- `lib/wallet/src/wallet_sqlite.c` — after `wallet_encrypt_blob()`, scrub the plaintext buffer
- `lib/wallet/src/keystore.c` — scrub key material after use in signing operations
- `lib/support/src/cleanse.c` — verify `memory_cleanse()` uses volatile writes

### What to implement:

```c
// After every wallet_encrypt_blob() call:
//   memory_cleanse(plaintext_buf, plaintext_len);
//
// After every signing operation that materializes a private key:
//   memory_cleanse(&privkey, sizeof(privkey));
//
// Audit: grep for stack-allocated uint8_t[32] near private key operations
// and add scrubbing at function exit.
```

### Test:
- Test that memory_cleanse actually zeroes memory (read back after scrub)
- Verify no compiler optimization eliminates the scrub (volatile)

---

## Task 4: MCP Tool Gap Audit + Backfill

CLAUDE.md documents 85+ RPC methods. Current MCP surface is 76 tools. Audit the gap and backfill the most useful missing tools.

### What to do:
1. List all RPC methods (check `lib/rpc/src/server.c` registration table)
2. Compare against MCP tool registrations in `tools/mcp/controllers/*.c`
3. For each gap, either:
   - Add an MCP wrapper (if the method is useful for AI agents)
   - Document why it's skipped (e.g., deprecated, dangerous, or covered by `zcl_rpc` passthrough)

### Priority backfill targets:
- `getmempoolentry` — useful for transaction debugging
- `getblocktemplate` — useful for mining analysis
- `z_listunspent` — useful for shielded balance details
- `z_viewtransaction` — useful for transaction analysis
- Any other methods that agents would commonly need

### Test:
- MCP tool count should increase
- `test_mcp_controllers.c` EXPECTED_TOTAL/EXPECTED_OPS must be updated
- `make docs-mcp` should regenerate cleanly

---

## Task 5: Harden Controller Error Paths

Several controllers have bare `return -1` without setting an error body for MCP. Audit and fix.

### What to do:
1. `grep -rn 'return -1' app/controllers/src/*.c tools/mcp/controllers/*.c` 
2. For each hit in an MCP-facing handler: ensure an error body is set before returning
3. For each hit in an RPC handler: ensure the error is logged with context

### Rules from DEFENSIVE_CODING.md:
- Every MCP handler must set an error body: never `return -1` without explaining why
- Use `LOG_FAIL()`, `LOG_ERR()`, `LOG_NULL()` from `util/log_macros.h`

### Test:
- `make lint` should pass
- `make test` should pass

---

## Build & Test

After EACH task:
```bash
git pull origin master
make -j$(nproc) 2>&1 | tail -20
make test 2>&1 | tail -10
make lint 2>&1 | tail -10
git add <specific files> && git commit -m "wave 19 task N: description"
git push origin master
```

## Boundary: Files You MUST NOT Touch
- `lib/net/src/msg_headers.c` (Agent2)
- `lib/validation/src/process_block.c` (Agent2)
- `app/services/src/sync_watchdog_service.c` (Agent2)
- `app/services/src/block_index_integrity.c` (Agent2)
- `app/services/src/sync_service.c` (Agent2)
- `app/services/src/block_sync_service.c` (Agent2)
- `app/controllers/src/sync_controller.c` (Agent2)
- `config/src/boot.c` (Agent2)
- `config/src/boot_index.c` (Agent2)

---

## Current Status

*(Update each session. Keep it short.)*

- **2026-04-14 wave 19 assigned** — 5 tasks: sqlite3_step cleanup, cookie rotation, key scrubbing, MCP backfill, controller error paths.
