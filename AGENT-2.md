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

## Current status — 2026-04-18 (late evening, P7 wave opened)

**Done and on main (25 rows + 1 infra):** P1.1, P1.2, P1.5, P6.1–P6.6,
P3.1, P3.2, P3.3, P3.4, P3.5, P3.6, P5.1, P5.3, P5.4, P5.7, P4.3, P4.4,
P4.5, P2.3, P2.8, P3.7, P2.4, P2.7, P2.6, P5.2, P2.5, P5.6, plus
parallel test runner infrastructure (df5de36c4). AGENT.md shows SHAs.

**New context:** Rhett ran a live-node inspection after the original
checklist drained. Found that the production zclassic23 chain tip is
**stuck at h=3,081,601** (val.block_connected loops on the same height
indefinitely; SIGABRT once memory hits the 6 GB cgroup ceiling). That
gap-analysis added a P7 tier of 10 rows to AGENT.md — five of them
are in your lane (chain-state-repository + lib/event/ crash handler +
deploy unit hygiene + SQLite pragma tuning).

**Now working on:** P7.2 boot tip-mismatch halt + P7.3 crash handler
stderr flush + P7.5/P7.6/P7.7 deploy-unit hygiene batch.
**Queued NEXT (pre-authorized):** P7.8 SQLite cache_size + mmap_size
tuning.

---

## NOW — P7.2 + P7.3 + P7.5/P7.6/P7.7 (deploy-unit batch)

These are independent and can land as separate commits.

### P7.2 — Boot tip-mismatch halt is advisory, must be fatal/auto-rewind

File: `app/services/src/chain_state_repository.c` (audit). Boot log
shows:

```
[coins] DB_ERR_TIP_MISMATCH: utxos max_height=3081408 > tip_height=3081407 (UTXOs ahead of tip) — halt and investigate; do not auto-heal.
Warning: Could not open SQLite coins view
```

Despite the "halt and investigate" wording, the node continues
booting and serving RPC. That defeats the safety belt: the
mismatched chain state is what's almost certainly causing P7.1
(tip stuck at h=3081601 on the live node). Either:

(a) **Strict halt** — make `DB_ERR_TIP_MISMATCH` fatal at boot.
    `LOG_FAIL` + structured event + `_exit(EXIT_FAILURE)`. The
    operator (or systemd) must explicitly clear the marker before
    the node will start again.
(b) **Auto-rewind** (preferred if safe) — when `utxos.max_height ==
    tip_height + 1`, treat as a single-block crash recovery:
    delete UTXOs at `tip_height + 1` only (BEGIN/COMMIT-bracketed),
    re-verify the resulting tip pointer, log loudly, continue.
    Require count check FIRST: if more than 32 UTXOs above tip,
    fall through to the strict-halt path (this is the scenario the
    `feedback_utxo_wipe_safety` memory in Rhett's CLAUDE rules
    forbids auto-healing because something else is very wrong).

I (Rhett) lean (b) but only with the count guard. Pick one and write
a regression test that:
- creates a UTXO row at height N+1 with chain tip at N
- boots the node
- with strict-halt: asserts `_exit(EXIT_FAILURE)` (fork+wait+WTERMSIG)
- with auto-rewind: asserts boot continues AND the high UTXO is gone

**Acceptance:** `make test` covers both the strict-halt fatal exit
and the auto-rewind safe path. Live node restart on Rhett's box
either rewinds to a consistent tip OR refuses to start with a
clear operator message.

### P7.3 — Crash handler header + stack trace never reach node.log

File: `lib/event/src/event.c:610-630` (the `crash_signal_handler`).

When the live node SIGABRT'd today, only the `sys.crash signal 6`
event survived to `node.log`. The `*** FATAL SIGNAL ***` header and
`backtrace_symbols_fd` output are missing. Likely cause: the
fprintf-buffered stderr never flushed before `_exit(128 + sig)`,
and `backtrace_symbols_fd` writes to STDERR_FILENO directly but the
fprintf buffer ahead of it owned the file position and got
discarded.

**Fix.**
1. Replace the fprintf header with a `write(STDERR_FILENO, ...)`
   on a small static buffer — async-signal-safe.
2. Add `fflush(stderr); fsync(STDERR_FILENO);` between the
   `event_dump_recent` call and `_exit`.
3. The `event_dump_recent` itself uses fprintf internally — audit
   it and either swap to write(2) or call `fflush(stderr)` at the
   end. (Safe in practice on Linux glibc inside a SIGABRT handler;
   the existing comment acknowledges this trade-off.)

**Acceptance:** add a regression test that fork()s a child,
installs the crash handler, raises SIGABRT, redirects child stderr
to a temp file, then asserts the temp file contains "FATAL SIGNAL
6" AND at least 3 frame addresses. Test is hostile to noisy stderr
in the parent — dup2(/dev/null) before fork like the P1.16 test
pattern Agent-3 used in test_core.c.

### P7.5/P7.6/P7.7 — Deploy unit hygiene batch (one commit)

File: `deploy/zclassic23.service` (you wrote this for P5.2; one
follow-up commit covers all three).

- **P7.5:** `TimeoutStopSec=300` → `TimeoutStopSec=90`. The 300s was
  a "worst case with headroom" but live evidence shows hangs do
  happen, and 5min is too long an outage. Comment update needed.
- **P7.6:** `StartLimitBurst=3 StartLimitIntervalSec=300` → bump
  burst window to allow more recovery attempts during operator
  triage (e.g. burst=10, interval=600). Worse than restart-loop is
  permanently-down-after-3-crashes. Document the trade-off.
- **P7.7:** Add `LimitCORE=infinity` and create a directory hint:
  `Environment="ZCL_CORE_DIR=%h/.zclassic-c23/cores"`. Do NOT add
  `ExecStartPre=mkdir`; the binary should create it lazily on first
  abort if the env var is set. (Or, simpler: set `kernel.core_pattern`
  expectations in deploy README rather than in the unit.)

**Acceptance:** `systemctl --user daemon-reload && systemctl --user
restart zclassic23` succeeds. Verify `LimitCORE=` appears in
`systemctl --user show zclassic23 | grep LimitCORE`.

---

## NEXT (pre-authorized) — P7.8 SQLite tuning

After P7.2/P7.3/P7.5/P7.6/P7.7 land. File:
`lib/storage/src/coins_view_sqlite.c:187` (and any other open-time
PRAGMA site).

Currently only `journal_size_limit = 104857600` is set. SQLite
defaults give us ~2 MB page cache and `mmap_size=0`. With a 1.3M-row
chainstate that's a lot of disk traffic. But: `boot_index.c:307`
comment says `mmap_size=64MB previously caused SIGSEGV after ~64K
addresses` — sleeping landmine, do not naively re-enable.

**Plan.**
1. Audit which sqlite handles in the binary need cache tuning
   (chainstate, wallet, addrman, peer_scoring, etc).
2. Pick safe `cache_size = -65536` (64 MB negative = bytes) for
   the chainstate; leave wallet at default.
3. **Skip mmap_size** unless you can identify the boot_index.c root
   cause — flag for Rhett with the audit findings if you find it.
4. Add a regression test that opens a 100k-row UTXO set, runs
   100 random reads, and asserts no SIGSEGV / no reader-rewind bug.

**Acceptance:** `./test_zcl` passes; ASAN build (`make asan`) passes
with the new pragma values; ad-hoc benchmark via `tools/zcl-rpc
getblockchaininfo` shows no regression.

**Risk checkpoint.** If any test breaks under the new cache_size
that isn't trivially explained, STOP and ping Rhett. Storage-tier
breakage is expensive to debug.

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

**2026-04-18 — P2.4 + P2.7 lane-expansion wave closed (eighth wave).**
Two commits:

- `9e8cfbb27` P2.4 swarm per-chunk SHA3 verification. Root cause was
  wire-format, not code — `zmanifest` only carried the Merkle root, so
  `swarm_sync_init` calloc'd a zero-filled `chunk_hashes` buffer on
  the receive side and every legitimate chunk then failed
  `fast_sync_verify_chunk` against all-zero hashes. Extended the
  manifest to carry `num_chunks * 32` bytes of per-chunk SHA3-256
  after the fixed header (capped at `MANIFEST_MAX_CHUNKS = 65_000` to
  fit within `MAX_PROTOCOL_MESSAGE_LENGTH` and bound the allocation a
  peer can force), added merkle-root reconstruction on the receiver
  side before trusting any chunk hash, and tightened
  `swarm_sync_init` to require non-NULL chunk_hashes + bounded
  num_chunks. Receiver side now bans on truncated-manifest (20),
  bounds (20), or merkle-mismatch (100). Three regression tests:
  single-bit flip → chunk rejected with 0 rows in utxos + state reset
  to NEEDED; re-request from another peer lands 3 rows; init refuses
  NULL and oversized cases.

- `a46410c50` P2.7 FlyClient challenge token-bucket rate limit. Each
  `zfcchallenge` forces `snapsync_build_fc_response` to rebuild 50
  MMB proofs over the full block index — tens of ms per challenge.
  Deliberately kept the state *off* `struct p2p_node` (which lives in
  Rhett's lane) by using a 64-slot LRU side table in
  `msgprocessor.c`. Burst 30, refill 10/sec (constants exposed as
  `FC_CHALLENGE_BURST` / `FC_CHALLENGE_RATE_PER_SEC` in
  `msgprocessor.h`). On empty bucket we silently drop and fire
  `PEER_OFFENCE_FLOOD` through the existing `peer_scoring_record`
  infra — exactly once per flood episode, resets when the peer next
  consumes a token, so sustained offenders cross the ban threshold
  while legit bursts never accrue. Three regression tests drive the
  limiter with an explicit clock: 1000 challenges in 1 ms ticks →
  ≤45 accepted / 961 dropped; flood → recover → flood re-arms the
  scoring gate; attacker draining its own bucket leaves a victim
  peer with its full burst.

**Scope-expansion note for Rhett.** P2.7 was the first Agent-2 row
touching the FlyClient message handlers directly. No header files
outside `lib/net/include/net/{fast_sync.h,msgprocessor.h}` were
modified, no changes to `struct p2p_node`, no crypto or consensus
code touched. The rate limiter is a drop-in side module — if you
want it relocated (`lib/net/src/flyclient_rate.c`?) or folded into
`peer_scoring.c`, happy to rework it.

**2026-04-18 (evening) — P2.6 + P5.2 lane closed (ninth wave).**
Two commits:

- `658b6fe5d` P2.6 `g_swarm_active` TOCTOU. The TOCTOU window was
  `if (!g_swarm_active) { lock; init; g_swarm_active = true; unlock; }`
  — two peers racing near-simultaneous zmanifest messages could both
  observe false, both take g_swarm_mutex in sequence, both call
  `swarm_sync_init`, second overwriting the first. Replaced the
  check+claim with `atomic_compare_exchange_strong` on the already-
  `_Atomic bool` global. CAS winner runs swarm_sync_init under mutex;
  if init fails, the winner releases the claim so the next peer can
  retry. CAS loser logs + drops the message. Reset site at 2376 uses
  explicit `atomic_store` for symmetry. Added test hooks
  `msgprocessor_test_swarm_{try_claim,release,is_active}` mirroring
  the P2.7 pattern; three regression tests: no-race, pthread-barrier-
  synchronized concurrent racers (exactly one wins), reset cycle
  re-arms.

- `ba450ea5c` P5.2 deploy service env-file hygiene. Moved the
  hardcoded `-externalip=205.209.104.118` and 9 `-addnode=...` flags
  out of `deploy/zclassic23.service` ExecStart into an optional
  `EnvironmentFile=-%h/.config/zclassic23/env`. Used bare `$VAR` form
  (not `${VAR}`) so systemd splits the `ZCL_ADDNODE_FLAGS` value on
  whitespace into separate argv entries. `deploy/zclassic23.env.example`
  ships the template with Rhett's current values for easy migration;
  a fresh clone without the env file starts clean against DNS seeds.
  README.md got a one-paragraph pointer. Smoke-tested on Rhett's box:
  height advanced 3081407 → 3081408 across restart, 4 peers connected,
  `getnetworkinfo.localaddresses` shows 205.209.104.118:8033 (proves
  `$ZCL_EXTERNALIP_FLAG` expanded from env).

**Out-of-scope flag for Rhett — sibling TOCTOU in the same file.**
`g_block_swarm_active` at `msgprocessor.c:2439/2451` has the identical
check-then-write pattern. Same atomic-CAS fix applies. Deliberately
left out of the P2.6 commit to keep "one logical fix per commit" —
please queue as a new P2.x row and I'll pick it up next rotation
(~20 LoC + 3 tests, direct copy of the P2.6 recipe).

**Test suite status.** test_zcl was killed in `test_block_pruning`
(the known hang already documented earlier in this file — not a
regression from this workstream). All 2773 prior tests including the
three new `swarm_cas: ...` tests passed before the hang site. `make
lint` is clean.

**2026-04-18 (evening) — P2.5 connman deadlock closed (tenth wave).**
One commit:

- `cd4b3c42f` P2.5 thread_message_handler → connman_run_message_cycle.
  Replaced the hold-cs_nodes-across-callback anti-pattern with the
  classic snapshot+iterate: take cs_nodes just long enough to copy
  the pointer list and add_ref each non-disconnected entry, drop
  the lock, run process_messages + send_messages with NO connman
  lock held, re-acquire cs_nodes to drop the refs. zcl_mutex_t is
  recursive so the three msgprocessor.c re-entry sites at 1328,
  2772, 3292 were already fine — but that recursion masked the
  latent hazard with any sibling lock taken outside the handler,
  and blocked every unrelated cs_nodes acquirer for the full
  iteration duration. Memory safety now relies on the existing
  ref_count: `connman_run_deferred_free_sweep` re-parks entries
  with `ref_count > 0` so an in-flight snapshot can't be UAF'd;
  the immediate-free fallback at the disconnect site grows a
  matching safety belt. `deferred_free[64]` bumped to the named
  `CONNMAN_DEFERRED_FREE_CAP = 256` — with max_connections=125
  that leaves enough headroom for ref'd entries to persist across
  socket cycles without hitting the fallback path.

  Stress test in test_net.c, opt-in via ZCL_STRESS_TESTS=1. Stands
  up a 50-peer connman with mock signals, spawns two workers: one
  drives connman_run_message_cycle in a tight loop, the other runs
  the disconnect + sweep loop mimicking thread_socket_handler. Mock
  process_messages flips peer->disconnect every 20 calls to force
  continuous ref churn. 1-second window. Default ./test_zcl skips
  the test. On 16-core test machine: 142M cycles, 1.9K callbacks,
  deferred_free drains clean, both workers join without blocking.

**Test-suite status.** `ZCL_STRESS_TESTS=1 ./test_zcl` reached
`p25_connman: ... OK (cycles=142833988 callbacks=1912)` along with
all earlier net + crypto + sapling tests before the known
`test_block_pruning` hang point that pre-dates Agent-2's workstream.
`make lint` clean.

**Scope note for Rhett.** This row was the first Agent-2 change to
touch `struct connman` directly (bumped `deferred_free[64]` →
`deferred_free[CONNMAN_DEFERRED_FREE_CAP]`). The new public entry
points `connman_run_message_cycle` + `connman_run_deferred_free_sweep`
are exposed in `net/connman.h` to let the stress test drive the
cycles without spinning up a full `connman_start()`. If you'd prefer
they be `*_test_*` prefixed to emphasize the opt-in surface, trivial
rename — no other caller exists today.

**NOW + NEXT are both empty for Agent-2.** The parallel test runner
infrastructure + the P2.5 stress test scaffolding remain as
"available opt-in infrastructure" but are not in-flight. Awaiting
Rhett — only open HIGHs are consensus-tier (P1.6, P1.7) or
script-tier (P4.1, P4.2), and the remaining MED rows are vendor
(P5.5, P5.6) — all in Rhett's lane.

**2026-04-18 (late evening) — P5.6 sqlite 3.49.0 → 3.53.0 landed
(eleventh wave, one-time vendor scope expansion).** One commit:

- `30e6fbc2e` P5.6 vendor: bump sqlite amalgamation 3.49.0 → 3.53.0.
  Downloaded `sqlite-amalgamation-3530000.zip` from sqlite.org
  (2026-04-09 release, latest stable), verified SHA3-256
  `c2325c53b3b41761469f91cfb078e96882ac5d85bac10c11b0bd8f253b031e5b`
  against the published checksum, then installed
  `vendor/include/sqlite3.h` (tracked) and rebuilt
  `vendor/lib/libsqlite3.a` (gitignored, per the `vendor/lib/` +
  `vendor/sqlite3.c` rules in `.gitignore`) from the amalgamation
  with `gcc -O2 -DSQLITE_THREADSAFE=1 -c vendor/sqlite3.c; ar rcs
  vendor/lib/libsqlite3.a sqlite3.o`. Matched the prior archive's
  compile-options strings table exactly: THREADSAFE=1 + SYSTEM_MALLOC
  + all DEFAULT_\* at stock values — no new on-disk-format flags,
  no ENABLE_JSON1/FTS/COLUMN_METADATA flipped. Header surface diff
  was 2583 lines but purely additive: new `SQLITE_ERROR_KEY` /
  `SQLITE_IOERR_CODEC` / `SQLITE_ERROR_RESERVESIZE` error codes,
  new `SCM_BRANCH` / `SCM_TAGS` / `SCM_DATETIME` introspection
  defines, de-experimentalized `snapshot_*` family, new
  `carray_bind_v2` / `db_status64` / `str_free` / `str_truncate`
  / `set_errmsg` / `setlk_timeout` / `changeset apply_v3` +
  `change_*` builders; no signature changes to bind/step/prepare/
  open. CVE-class fixes picked up across 3.50.x → 3.53.x are laid
  out verbatim in the commit body and mirrored into the AGENT.md
  row body (3.49.1 concat_ws buffer overrun, 3.49.2 NOT NULL
  optimization memory error, 3.50.3 CREATE TRIGGER parser
  memory-safety regression from 3.49.0, 3.50.4 two uninit-var
  reads, 3.51.0 POSIX-advisory-lock-abuse corruption detection,
  3.51.3 + 3.53.0 WAL-reset corruption bug).

**Test-suite status.** `./test_zcl` ran 2516/2516 assertions green
through every sqlite-backed group — test_sqlite,
test_wallet_sqlite_enc, test_wallet_sqlite_open_errors,
test_wallet_persistence_cycle, test_schema_migration,
test_db_migration_idempotent, test_chain_state_repo, test_db_txn,
test_wallet_backup, test_db_maintenance — plus every non-sqlite
group including all sapling/crypto/net/msg-handler groups, stopping
at the pre-existing `test_block_pruning` hang (futex_wait_queue main
thread, hrtimer_nanosleep worker — matches the signature documented
in earlier Notes entries). No new FAIL lines outside
deliberate-negative-test paths; no SQLITE_BUSY / SQLITE_LOCKED /
SQLITE_CORRUPT lines anywhere in the log. `make lint` clean,
`make clean && make -j$(nproc)` clean.

**Deploy-smoke note for Rhett.** Did NOT run `make deploy` on this
row. The systemd unit points at `%h/zclassic23/zclassic23` (Rhett's
primary clone), not `%h/zclassic23-2/zclassic23` — so `make deploy`
from Agent-2's clone would checkpoint the WAL + restart the service
but run Rhett's unchanged binary, which defeats the purpose of
smoke-testing the new SQLite. Confirmed instead that the Agent-2
`./zclassic23` binary links + launches (`./zclassic23 -version`
correctly refuses to start against the already-locked datadir) and
that the running live node remains healthy (height 3081601,
10 peers, RPC responsive) — nothing in this commit touches shared
runtime state. To put 3.53.0 on the live node Rhett needs to pull
main into `~/zclassic23`, `make deploy` from there, and confirm
`zcl_status` shows height advancing across the restart.

**Process note.** The `.gitignore` entries `vendor/lib/` and
`vendor/sqlite3.c` mean the on-disk amalgamation source and the
built archive are both gitignored — only the public header passes
through git. If future vendor bumps want to cache the amalgamation
source for reproducibility we should either drop the
`vendor/sqlite3.c` exclusion (and accept the +8 MB source in the
tree) or add a tiny `vendor/rebuild-sqlite.sh` that fetches +
verifies + compiles in a single step. Low-pri either way — the
reproducible recipe is in the commit body.

**NOW + NEXT are both empty for Agent-2.** Queue drained. Only
remaining MED is P5.5 (vendor/tor submodule pin) which the brief
explicitly reserves to Rhett for .onion bootstrap smoke-testing.
Awaiting assignment.
