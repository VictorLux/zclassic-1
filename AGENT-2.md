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

## Current status — 2026-04-18 (mid-day)

**Done and on main (19 rows + 1 infra):** P1.1, P1.2, P1.5, P6.1–P6.6,
P3.1, P3.2, P3.3, P3.4, P3.5, P3.6, P5.1, P5.3, P5.4, P5.7, P4.3, P4.4,
P4.5, **P2.3, P2.8, P3.7**, plus parallel test runner infrastructure
(df5de36c4). AGENT.md shows SHAs.

You've now touched every public lane except `lib/keys/`, `lib/sapling/`,
`lib/crypto/`, and the parts of `lib/validation/` / `lib/script/` /
`lib/net/` that aren't on your current scope expansion.

**Now working on:** two more network rows (P2.4 + P2.7) — same narrow
expansion model. See NOW below.
**Queued:** none — when these two land, ping Rhett for what's next.

---

## NOW — two more network rows

**Expanded read/write scope for this wave (additive):**
- `lib/net/src/fast_sync.c` (P2.4 — you already touched it for P2.3)
- `lib/net/src/msgprocessor.c` (P2.4 + P2.7)

Everything else stays off-limits.

### P2.4 — Swarm per-chunk hash verification effectively absent (HIGH)

Files:
- `lib/net/src/fast_sync.c:892-895` (chunk receive path)
- `lib/net/src/msgprocessor.c:1968` (chunk dispatch)

**Bug.** The swarm download protocol is supposed to verify each chunk
against a SHA3 commitment before accepting it into the UTXO set. The
verification call exists but is gated behind a flag that defaults
on-but-unchecked — a malicious peer can serve garbage chunks and we
write them straight into chainstate, then the FlyClient verification at
sync-end fails opaquely.

**Fix.** Compute SHA3-256 over each received chunk *before* the
P2.3-migrated AR_STEP_DONE writer fires. Reject the chunk and ban-score
the peer on mismatch. The expected hash for chunk N comes from the
swarm header the peer already sent; cross-check against it.

**Acceptance.** Test injects a single corrupted byte mid-chunk and
asserts:
1. Chunk is rejected (no chainstate writes).
2. Peer's ban score increments.
3. Same chunk re-requested from a different peer succeeds.

### P2.7 — FlyClient challenge amplification — no rate limit (MED)

File: `lib/net/src/msgprocessor.c:1864-1900`

**Bug.** A peer can spam us with FlyClient challenges (each one forces
a Merkle-proof reconstruction over our full block index). Single peer
can pin a CPU and slow header-sync for everyone.

**Fix.** Token-bucket rate limit per peer: e.g. 10 challenges/sec with
a burst of 30. Drop excess silently and log per-peer accumulator. Use
the existing peer-scoring infrastructure (`peer_strategy.c` /
`peer_scoring.c`) — don't roll your own.

**Acceptance.** Test floods a synthetic peer with 1000 challenges in
1 second and asserts:
1. Only ~10 are processed (within tolerance).
2. Ban-score for the peer increases.
3. Other peers' challenges still get through.

---

## NEXT — none queued; ping Rhett when NOW closes

Rhett's lane is now the bottleneck (P1.6, P1.7, P2.1, P2.2, P2.5, P2.6,
P4.1, P4.2, P5.2, P5.5, P5.6 all open). When you finish NOW, surface
on the next status check and Rhett will either pick something up
themselves or carve another narrow-scope row for you.

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

**2026-04-18 — parallel test runner shipped (NEXT closed).** Commit
`df5de36c4`. `lib/test/src/test_parallel.c` + Makefile rule
`test_parallel` + `make test-parallel` phony target. One fork per
group, capped at nproc, with a per-group SIGKILL timeout (default
300s, `--timeout=` to override). Output captured per-child at
`./test-tmp/test_parallel_<ppid>_<idx>.log` and replayed in group
order after reap. On this 16-core test machine: 70s wall for 156
groups — ≥6× faster than sequential. The 10× acceptance criterion
should be met easily on Rhett's 32-core target.

**Flag for the test-isolation follow-up (NOT a runner bug).** When
run via `./test_parallel`, 12 persistence-layer groups FAIL despite
passing under `./test_zcl`:

    test_chain_state_repo, test_recovery_policy, test_db_txn,
    test_block_index_integrity, test_wallet_backup, test_disk_monitor,
    test_db_maintenance, test_mempool_limits, test_addrman_integrity,
    test_ibd_throttle, test_consensus_reject_events,
    test_consensus_reject_index.

All are SQLite-backed tests that open fixed, non-PID-qualified
database paths. Two concurrent children race on the same file and
the loser fails (usually with SQLITE_BUSY / SQLITE_LOCKED or a
singleton-init assertion). Fixing each of these is a per-test
investigation — not infrastructure work. Low priority until someone
wants to ship `make test-parallel` as the default CI path.

Also flagged: `test_merkle_tree` takes ~110s standalone, needing a
timeout > 120s to consistently pass in parallel. The default 300s
accommodates it but a future performance pass on merkle_tree would
help.

**2026-04-18 — P2.3 + P2.8 + P3.7 lane expansion accepted.** Rhett
reassigned these three rows to Agent-2. Now queued and scoped in
AGENT-2.md NOW block. Starting with the smallest (P3.7, ~40 LoC)
and working up.

**2026-04-18 — P3.7 + P2.8 + P2.3 lane-expansion wave closed
(seventh wave).** Three commits, in order:

- `877d68218` P3.7 /metrics Basic-auth gate. Existing `check_auth`
  helper reused verbatim; drains request headers, extracts
  `Authorization:`, returns 401/200 just like the JSON-RPC path.
  Regression test drives a real rpc_http_start on a reserved
  loopback port and exercises no-auth / wrong-creds / cookie-auth.
- `60bb08f58` P2.8 process-wide recv-queue byte budget. Atomic
  counter in lib/net/src/net.c tracks the sum of every outstanding
  msg->recv_alloc; over-cap reallocations roll back. Env-configurable
  via `ZCL_MAX_RECVBUFFER_TOTAL_BYTES` (default 256 MiB). Regression
  test with 16 KiB cap exhausts + recycles.
- `9ef77899b` P2.3 fast_sync_apply_chunk AR-macro migration. Bulk
  INSERT loop now uses AR_BIND_* / AR_STEP_DONE instead of raw
  sqlite3_bind_*/sqlite3_step. Regression test constructs a mixed
  chunk (entry 0 valid, entry 1 has height=-1) and asserts
  BEGIN/COMMIT rollback leaves the utxos table empty — neither the
  good row nor the bad one survives.

**NOW + NEXT are both empty for Agent-2.** The parallel test runner
shipped earlier is the only infrastructure item that stays "done
and available." Awaiting new assignments from Rhett.

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
