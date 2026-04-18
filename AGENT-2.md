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

## Current status — 2026-04-18

**Done and on main (15 rows):** P1.1, P1.2, P1.5, P6.1–P6.6, P3.1, P3.2,
P3.3, P3.4, P3.5, P3.6, P5.1, P5.3, P5.4, P5.7, P4.3, P4.4, P4.5.

Every row in your original brief plus the entire P3 group plus the
narrow-P4 expansion plus full P5 hygiene wave is now closed. AGENT.md
shows the SHAs.

**Now working on:** narrow-scope expansion into the network and RPC
lanes — see NOW below. Three small AGENT.md rows.
**Queued:** parallel test runner (infrastructure). See NEXT.

---

## NOW — narrow expansion into lib/net/ + lib/rpc/

Three more bounded fixes. Each is in a single file, each has a sharp
spec, each is the kind of work you've already proven you do well.

**Expanded read/write scope for this wave:**
- `lib/net/src/fast_sync.c` (P2.3 only)
- `lib/net/src/net.c` (P2.8 only)
- `lib/rpc/src/httpserver.c` (P3.7 only)

**Still off-limits:** every other file in `lib/net/`, `lib/rpc/`,
`lib/validation/`, `lib/consensus/`, `lib/script/` (except the two
P4 files you already touched).

### P2.3 — fast_sync bypasses AR_BEGIN_SAVE (HIGH)

File: `lib/net/src/fast_sync.c:480-526`

**Bug.** The fast-sync chunk writer takes the same shortcut every other
raw-step site did pre-P3.3 — calls `sqlite3_step()` directly, skipping
the activerecord transaction wrapper. A partial chunk write under power
loss / OOM kill leaves the chainstate in a half-imported state.

**Fix.** Same migration pattern as P3.3 / P1.5: hoist the prepare into
the init path, route stepping through `AR_BEGIN_SAVE` for writes /
`AR_STEP_ROW_READONLY` for reads. If a step needs to remain raw for
performance, document it with a `// raw-sql-ok: <reason>` annotation.

**Acceptance.** Add a test that simulates a mid-chunk SQLITE_IOERR and
asserts the surrounding chunk transaction rolls back atomically.

### P2.8 — no global byte budget on recv queue (MED)

File: `lib/net/src/net.c:104-115`

**Bug.** The recv queue grows unbounded — a peer can fill our memory
by sending data we haven't processed yet. Per-connection limits exist;
a process-wide cap doesn't.

**Fix.** Track total recv-queue bytes across all peers in an atomic
counter incremented on enqueue / decremented on dequeue. Reject the
TCP read (or stop reading from the socket — backpressure) when the
total exceeds a configurable cap (`-maxrecvbuffertotal=N`, default
e.g. 256 MiB). LOG_ERR with peer count + total bytes when the cap
trips so operators can tune.

**Acceptance.** Test that fills the budget from one synthetic peer
and asserts the next read is refused / paused without crashing.

### P3.7 — `/metrics` open on TLS listener with no auth (MED)

File: `lib/rpc/src/httpserver.c:355-381`

**Bug.** The Prometheus-style `/metrics` endpoint is registered on the
public TLS listener with no auth gate. It exposes peer counts, tx
volume, mempool size — all useful to a stalker / network adversary for
fingerprinting.

**Fix.** Gate `/metrics` behind the same RPC cookie auth (or an explicit
`-metrics-allowed-cidr=` option) the wallet RPCs already use. Look at
how `getblockchaininfo` is registered and apply the same auth shim.

**Acceptance.** Test that posts to `/metrics` without a cookie and
asserts 401; with a valid cookie and asserts 200 + Prometheus body.

---

## NEXT — parallel test runner (infra, pull up if NOW closes quickly)

`test_zcl` is single-binary, single-threaded, runs ~140 test groups in
sequence. On a 32-core box we use ~3%. Tests take 8–15 minutes per
iteration — biggest productivity drag in the project right now.

Build a fork-parallel driver under `lib/test/src/test_parallel.c`:

1. Enumerate the test-group symbols (same list as `test.c:38-194`).
2. `fork()` one child per group, capped at `nproc` workers.
3. Child redirects stdout / stderr to a temp file, runs its group,
   exits with 0 / 1.
4. Parent waits on all children, collects pass/fail, prints union
   output in group order, returns 1 if any failed.
5. Groups that call `ecc_start()` / `ecc_verify_init()` either need
   their setup hoisted, or the driver forks a fresh process per group
   (cleaner but slightly slower — acceptable).

Ship as a new `make test-parallel` target. Keep sequential `./test_zcl`
unchanged — the parallel runner is additive.

**Acceptance.** On a 32-core box, `make test-parallel` should be ≥10×
faster than `./test_zcl` with the same pass/fail outcome.

Pure infrastructure — touches only `lib/test/` and the Makefile.
No consensus, no wallet, no crypto.

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

**2026-04-17 — P5 operator-hygiene sweep closed (fourth wave).**
Five commits: `a9ac382b7` (P5.1 export_snapshot ELF untracked),
`611ae4281 / e7528c4f0 / 8902f9ae7 / d106192a4` (P5.7 repo-root
archive — 41 → 18 root `.md`, two stale binaries untracked),
`09e4fb15a` (P5.3 hardcoded `/home/rhett` → `$HOME` via shared
`lib/util/include/util/rpc_paths.h` helper + regression test
`test_no_hardcoded_home.c`), `0f33d3fc1` (P5.4 purge
`verify_restart_follow.sh`).

**2026-04-17 — P4 narrow-scope-expansion wave closed (fifth wave).**
Three commits: `61104d06d` (P4.3 `script_num_serialize` bounds
check — precompute required length and return 0 for short buffers
instead of silent truncate; 6 assertions covering the 1-byte
rejection path, INT64 extremes, value == 0), `f69956cab` (P4.4
`disconnect_block` clamp — reject `prevout.n ≥ MAX_BLOCK_SIZE` to
close the ~128 GB realloc DoS; regression test constructs a minimal
block with `prevout.n = UINT32_MAX`), `28fe53112` (P4.5 sigencoding
parity — byte-for-byte audit found NO divergence from upstream
Zcash; 16-vector BIP66 parity table locks the canonical behavior in
place). The "off-by-one vs Bitcoin" described in the brief was a
false positive — see commit message for the full audit trail.

**NOW + NEXT are both empty for Agent-2.** Only the parallel test
runner infrastructure item remains. All P5 operator-hygiene and P4
narrow-scope-expansion rows are shipped. Pinging Rhett before
starting anything outside the committed lane.

**P5.4 audit — flag for Rhett, not a full purge.** Only one of the
eight `tools/*.sh` scripts had a clean 1:1 replacement
(`verify_restart_follow.sh` ⇒ `zcl-nodectl verify-follow`). The other
seven don't match the "MCP tool with the same suffix" pattern the
brief assumed:

- `consensus_parity_audit.sh` — compares ZClassic23 RPC vs the C++
  reference; MCP is single-node and can't observe both.
- `dep_audit.sh` — build-time CVE scan over `vendor/`; no runtime
  analogue possible.
- `deploy_verify.sh` — post-`make deploy` poll-until-live probe.
  Thin enough to move into `zcl-nodectl` as `deploy-verify` (new
  subcommand would duplicate ~30 lines of `cmd_status`'s RPC
  plumbing). Low priority — deploy_verify only runs from the
  Makefile, not interactively.
- `release.sh` — tarball + sha3 + GPG signing; build-time, not a
  runtime surface.
- `soak_test.sh` — 72-hour monitoring daemon. Could become a
  `zcl-nodectl soak` subcommand that logs `zcl_status` every 5 min;
  minor win over a shell loop.
- `test_dual_node.sh` — integration test that *starts* the node and
  asserts RPC comes up; binding this into a runtime tool would
  require that tool to fork the node-under-test, which violates
  single-responsibility.
- `test_txn_checklist.sh` — parity check across two nodes, same
  shape as `consensus_parity_audit`.

Net result: one delete, seven left in place with the above
rationale. If you want a stricter no-shell policy, the natural next
step is a `zcl-nodectl deploy-verify` / `soak` refactor — estimate
~80 LoC total, no scope boundary crossing.
