# AGENT-2 — Wallet / Storage / App-Layer Hardening

**Derived from:** the 2026-04-17 full code review. See `AGENT.md` for the
cross-agent priority table. Last brief rewrite: 2026-04-17.

**Working directory:** `~/zclassic23-2` (separate git clone; pushes to `origin/main`).
**Coordinator:** Rhett (`~/zclassic23`).
**Sibling:** Agent-3 (`~/zclassic23-3`), in the crypto/sapling lane.

---

## Lane — what you may edit

**Full edit access:**
- `lib/wallet/`, `lib/storage/`, `lib/coins/`
- `app/controllers/`, `app/services/`, `app/models/`, `app/views/`
- `tools/mcp/controllers/` — including JSON-payload construction
  (P3.1 / P3.2 live here)
- `tools/mcp/` general (`rpc_client.c`, `middleware.c`, `router.c`, etc.)
- `lib/test/` — only to add/modify tests covering your changes
- Top-level repo hygiene for the P5 wave: `.gitignore`, tracked
  binaries, stale docs at repo root

**Read-only / off-limits:**
- `lib/crypto/`, `lib/sapling/`, `lib/keys/` — Agent-3's lane
- `lib/rpc/`, `lib/validation/`, `lib/consensus/`, `lib/net/`,
  `lib/script/` — Rhett's lane
- `deploy/zclassic23.service` (externalip + addnode block) — Rhett (P5.2)
- `vendor/` — Rhett (submodule pins, CVE cherry-picks)

---

## Current status — 2026-04-17

**Done and on main:**

| Row | What | SHA |
|---|---|---|
| P1.1 | Wallet wrapper silent-error | 8608820e7 |
| P1.2 | Flush commits partial state | 8608820e7 |
| P1.5 | Raw sqlite3_step in UTXO batch writer | 152603fdc |
| P6.1 | write_sapling_key UPDATE failure propagates | 8608820e7 |
| P6.2 | Flusher reset scoped to own stmts | 152603fdc |
| P6.3 | read_keys LOG_ERR on malformed rows | 8608820e7 |
| P6.4 | Migration bookkeeping rc-checked | 767d9d3e7 |
| P6.5 | Hot-path prepare hoisted to init | 8608820e7 |
| P6.6 | coins_alloc OOM logged | dc60b7e7b |
| P3.3 | ~115 raw sqlite3_step sites across 17 files | 2a59ac938 + 8 siblings |
| **P3.1** | **MCP `zcl_send` JSON injection closed** | **b0134339b** |
| **P3.2** | **MCP `zcl_sendtoaddress` JSON injection closed** | **b0134339b** |
| **P3.4** | **store address checksum validation** | **64a4afffc** |
| **P3.5** | **rpc_client realloc leak closed** | **f0e8d31d3** |
| **P3.6** | **URL-decode + HMAC form token** | **efa211811** |

Every CRITICAL/HIGH row in your original lane is now closed. The full
P3 group is done except P3.7 (stays with Rhett — `lib/rpc/` is
off-limits for you).

**Now working on:** P5 operator hygiene — see NOW below.
**Queued:** narrow expansion into `lib/script/` / `lib/validation/` for
three small P4 rows, plus an infrastructure contribution (parallel test
runner). See NEXT.

---

## NOW — P5 operator hygiene (pre-authorized)

Do in order, smallest first. These were previously "queued"; now
they're active.

### P5.1 — remove tracked export_snapshot ELF (HIGH)

The 1.1 MB `export_snapshot` binary is in git despite `.gitignore`. Fix:

```bash
git rm --cached export_snapshot
```

Verify `.gitignore` actually matches. One commit: `build: untrack
export_snapshot ELF`.

### P5.7 — repo-root clutter (LOW)

Audit tracked `.md` at repo root against what's still relevant. Move
superseded hardening docs to `docs/archive/`; delete anything confirmed
unused. Add runtime artifacts (`node.db`, `*.log`) to `.gitignore`. One
commit per logical group.

### P5.3 — hardcoded `/home/rhett` paths (HIGH)

Files: `tools/export_snapshot.c:15`, `tools/zcl-nodectl.c:628-637`,
plus anywhere else `grep -rn "/home/rhett" tools/ app/ lib/ config/`
finds. Replace with `getenv("HOME")` for runtime paths, `zcl_datadir()`
for data paths, install-prefix default otherwise. Don't introduce new
config; use what's already there.

Add a regression test that exercises a non-`/home/rhett` `$HOME`.

### P5.4 — purge shell scripts in `tools/*.sh` that MCP replaces (MED)

For each `tools/*.sh`:

1. Identify the MCP tool that replaces it (usually the naming matches:
   `tools/zcl-balance.sh` → `zcl_balance`).
2. If MCP covers the functionality: delete the script.  Commit message:
   `tools: purge zcl-FOO.sh (superseded by MCP tool zcl_FOO)`.
3. If functionality is missing from MCP: TODO comment, flag Rhett,
   leave the script in place.

Project rule: no standalone shell scripts — everything in the binary.
See `feedback_no_external_tools.md` in Rhett's memory.

---

## NEXT — narrow P4 expansion + parallel test runner (pre-authorized)

Rhett is formally expanding your lane to cover three small, well-bounded
P4 rows. All three have sharp specs, are ≤100 LoC each, and don't
touch the interpreter core (which P4.1/P4.2 own — those stay Rhett).

**Expanded read/write scope for this wave:**
- `lib/script/src/script.c` and `lib/script/include/script/script.h`
  (P4.3 only)
- `lib/script/src/sigencoding.c` (P4.5 only)
- `lib/validation/src/connect_block.c` (P4.4 only)

**Still off-limits:** `lib/script/src/interpreter.c`, all other
`lib/validation/` files, `lib/consensus/`, `lib/net/`, `lib/rpc/`.

### P4.3 — script_num_serialize outsize bounds check (MED)

File: `lib/script/include/script/script.h:239-258`

**Bug.** `script_num_serialize(out, outsize, v)` writes up to 9 bytes
(max `CScriptNum`) but doesn't check `outsize`. If a caller passes a
short buffer, it's a heap overflow.

**Fix.** Return 0 / false if `outsize < required`. Update every caller
to handle the rejection (compile error tells you where they are). Add a
test that passes `outsize = 1, v = INT64_MAX` and asserts rejection
without writing past the buffer.

### P4.4 — disconnect_block unbounded realloc on vin.prevout.n (MED)

File: `lib/validation/src/connect_block.c:586-607`

**Bug.** The realloc size is derived from an attacker-controlled
`vin.prevout.n` without an upper bound. A block with a malformed
transaction (n = 2³²−1) makes us attempt a ~128 GiB alloc during
disconnect.

**Fix.** Clamp `n` to the actual vout count of the prevout's funding
transaction (known at this point). If `n >= funding_tx.vout_count`,
reject the block as `bad-txns-inputs-invalid` with a LOG_FAIL. Add a
test that constructs a block with out-of-range prevout.n and asserts
rejection.

### P4.5 — sigencoding strict-DER bound inconsistency vs Bitcoin (MED)

File: `lib/script/src/sigencoding.c:11-56`

**Bug.** Our strict-DER check has an off-by-one vs. the Bitcoin /
upstream Zcash implementation — we reject signatures with length ==
`r_len + s_len + 6` that they accept, or vice versa. Consensus risk.

**Fix.** Compare our code against
`vendor/sources/zcashd/src/script/interpreter.cpp` (if vendored) or
the current upstream Bitcoin Core `IsValidSignatureEncoding`. Align
the bounds exactly. Add a regression test vector from the Bitcoin
test suite.

### Infrastructure — parallel test runner (no severity, high leverage)

Rhett asked about this earlier. Current `test_zcl` is single-binary,
single-threaded, and runs ~140 test groups in sequence. On a 32-core
box we use ~3%. Tests take 8–15 minutes.

Build a fork-parallel driver under `lib/test/src/test_parallel.c` that:

1. Enumerates the test-group symbols (same list as `test.c:38-194`).
2. `fork()`s one child per group, capped at `nproc` workers.
3. Each child redirects stdout / stderr to a temp file, runs its group,
   exits with 0 / 1.
4. Parent waits on all children, collects pass/fail, prints the union
   output in group order, returns 1 if any failed.
5. Skip forking for groups that call `ecc_start()` / `ecc_verify_init()`
   — those either need to be moved into each group's setup, or the
   driver forks a fresh process per group (cleaner but slightly slower
   — acceptable).

Ship as a new `make test-parallel` target. Keep the sequential
`./test_zcl` working as-is — the parallel runner is additive.

**Acceptance.** On a 32-core box, `make test-parallel` should be ≥10×
faster than `./test_zcl` with the same pass/fail outcome.

This is pure infrastructure — touches only `lib/test/`, the Makefile,
and a new `main()` in `test_parallel.c`. No consensus, no wallet, no
crypto.

---

## Preflight — run verbatim when re-bootstrapping a stale clone

```bash
cd ~/zclassic23-2

# 1. Preserve any local-only work
git add -A
git diff --staged --quiet || git commit -m "a2: wip checkpoint before AGENT-2.md workstream"

# 2. If on a feature branch, back it up, then move to main
CURRENT=$(git branch --show-current)
if [ "$CURRENT" != "main" ]; then
    git push origin "$CURRENT" 2>/dev/null || true
    git checkout main
fi

# 3. Sync
git pull origin main --rebase=false

# 4. Confirm clean baseline
git status
git branch --show-current   # must print "main"

# 5. Read the rules and the checklist
cat CLAUDE.md
cat DEFENSIVE_CODING.md
cat AGENT.md

# 6. Confirm green baseline (STOP + report if either fails)
make -j"$(nproc)"
./test_zcl
```

---

## Commit protocol

- One logical fix per commit. Good: `wallet: propagate flush failure
  through wrapper return values`. Bad: `wallet + storage + migrations`.
- Every commit: `make test` must pass. Every push: `make ci` must pass.
- Commit message format:

  ```
  <subsystem>: <one-line summary>

  <why — cite file:line from AGENT.md>

  Fixes P3.4 (AGENT.md checklist).
  ```

- After each push, update the row in `AGENT.md`:
  `open` → `in-progress` → `done <SHA>`.
- Push frequently so Rhett can review incrementally.
- Never `--amend` a pushed commit. Never `--force-push`.

---

## Coordination rules

- Agent-3 is in `lib/crypto/` + `lib/sapling/` + `lib/keys/`. You should
  not see their files in your diff. If you do, stop and check.
- If you need something Rhett owns (a validation fix, a vendor update),
  note it in your commit message and wait — don't front-run.
- Surprising out-of-scope discoveries → append to the "Notes from
  Agent-2" section at the end of this file. Do not expand scope silently.
- When your NOW + NEXT piles are both empty, ping Rhett. Don't start
  P5.2 / P5.5 / P5.6 on your own (service file / vendor / CVE work needs
  coordination).

---

## Notes from Agent-2

**2026-04-17 — P1.1/P1.2/P1.5/P6.1–P6.6 landed (first wave).**
Four commits: dc60b7e7b, 767d9d3e7, 152603fdc, 8608820e7; status update
af247faf0. `ZCL_TEST_ONLY=persistence ./test_zcl` passes cleanly.
`test_wallet_flush_rollback` covers the exact silent-partial-state path
that lost 0.4 ZCL on 2026-04-12 (SQLite trigger that aborts
`wallet_transactions` INSERTs; assert the whole transaction rolls back
and the function returns false).

**Out-of-scope flag — `test_block_pruning` hangs on current main.**
After "prune: fixture init (basic)... OK" the process sits in
`futex_wait_queue` / `hrtimer_nanosleep` at 6–17% CPU indefinitely.
Reproduces against a clean merge with and without Agent-2 changes —
not a regression from this workstream. Owner: Rhett
(`app/services/src/block_pruning_service.c` is out of Agent-2's lane).
Worth checking whether the new `test_make_lint_gates` (which shells out
to `make`, may linger in `system()`) interacts with an earlier test's
background thread — haven't bisected.

**2026-04-17 — P3.3 landed (second wave).** ~115 raw `sqlite3_step`
sites migrated across 17 files (commits 50d25b2d8, d8246b85e, f6ee6a4c6,
36b5eafaf, 9dd2faf83, fa55e8dd7, 794f758d5, 2544d044d, 2a59ac938). Five
state-kv / rollback opt-outs retained with `// raw-sql-ok:` annotations
describing why they're intentionally unchecked — all correct.

**2026-04-17 — P3 group closed (third wave).** Five commits:
b0134339b (P3.1/P3.2 MCP JSON injection + class sweep across all MCP
controllers), f0e8d31d3 (P3.5 rpc_client realloc leak), 64a4afffc
(P3.4 store Base58Check/Bech32 checksum), efa211811 (P3.6 URL-decode
+ HMAC-bound form token). P3 table in AGENT.md fully marked done for
Agent-2's rows.

**P3.6 scope flag for Rhett — classical per-session CSRF still
pending.** The form-token is HMAC(per-process-random, product_id), so
it blocks the common threat (malicious third-party page crafting a
`<form action>` to our .onion — same-origin policy keeps JS from
reading the token). It does NOT protect against a server-side
attacker who can `curl` the product page to scrape the token before
triggering the victim browser. Closing that gap requires binding the
token to a session cookie, which needs plumbing cookies through
`lib/net/src/onion_service.c → store_handle_request` (today the
handler receives only `method/path/body`, not headers). Flagging
rather than expanding scope — this touches Rhett's lane
(`lib/net/`).
