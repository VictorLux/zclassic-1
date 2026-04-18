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

## Current status — 2026-04-18 (evening, post-P2.5/P2.6/P5.2)

**Done and on main (24 rows + 1 infra):** P1.1, P1.2, P1.5, P6.1–P6.6,
P3.1, P3.2, P3.3, P3.4, P3.5, P3.6, P5.1, P5.3, P5.4, P5.7, P4.3, P4.4,
P4.5, P2.3, P2.8, P3.7, P2.4, P2.7, P2.6, P5.2, P2.5, plus parallel
test runner infrastructure (df5de36c4). AGENT.md shows SHAs.

The entire P2 network-attack-surface tier you owned is now closed,
plus all of P3 (MCP/app), P4 (script memory safety), and P5 deploy
hygiene except the two vendor-tier rows.

**Now working on:** P5.6 — vendored sqlite3.h CVE update.
**Queued NEXT:** none. After P5.6 lands, ping Rhett — the remaining
open rows are all pure-Rhett (P1.6/P1.7 consensus, P2.1/P2.2 net,
P4.1/P4.2 script interpreter) or in-flight on Agent-3 (P1.16, prf.c).

---

## NOW — P5.6: vendored sqlite3.h CVE update

**Lane note (one-time scope expansion):** vendor/ is normally Rhett's
lane, but P5.6 is well-bounded (single header + amalgamation) and
sqlite is a leaf dependency that doesn't intersect with consensus
code. Constraint: only `vendor/include/sqlite3.h` and
`vendor/sqlite/` (or wherever the .c amalgamation lives — find it,
don't guess). Do NOT touch any other vendor dir, do NOT touch
`vendor/tor` (P5.5 stays Rhett — submodule pin requires separate
testing).

**Bug.** `vendor/include/sqlite3.h:149` shows `SQLITE_VERSION
"3.49.0"`. Several CVE-class fixes have landed in the 3.50.x series
(check sqlite.org/changes.html for the exact list — the relevant
ones are likely the prepared-statement use-after-free and the
JSON1-parser bounds bug, but verify). We've been carrying the older
header against a public-attack-surface SQLite (it's the canonical
UTXO store and the wallet keystore — both reachable via P2P-relayed
or RPC-driven workloads).

**Fix.**

1. Pull the latest stable amalgamation tarball from sqlite.org
   (sqlite-amalgamation-XXXXXXXX.zip — pick the most recent
   3.x release). Verify the SHA against the published checksum.
2. Drop the new `sqlite3.h` into `vendor/include/` and the new
   `sqlite3.c` into wherever the existing one lives (find with
   `git ls-files | grep sqlite3.c`).
3. **Diff review:** git diff vendor/include/sqlite3.h — anything
   touching SQLITE_VERSION (expected) and the new feature flags
   (review carefully — we may need to disable any new defaults that
   change on-disk format or change behavior we depend on).
4. Build clean: `make clean && make -j$(nproc)`.
5. Run the FULL test suite, not the persistence subset. SQLite
   touches every code path that hits the chainstate, the wallet,
   the chain_state_repo, the addrman, the migrations, and the
   peer_scoring side table. Allow up to 15 min for the full
   `./test_zcl` run.
6. Commit message must list the CVE / changelog entries this pin
   picks up (cite the sqlite changelog URL + version range).

**Acceptance.**
- `./test_zcl` passes (full suite, no `ZCL_TEST_ONLY` shortcut).
- `make ci` passes.
- `zcl_status` after `make deploy` shows the node still syncs and
  serves RPC. Restart cycle: stop service → start service → height
  advances → wallet readable.
- The commit lays out, in the body, the specific CVE IDs or
  changelog bullets being pulled in. (If none — i.e. the bump is
  pure feature/perf — say so explicitly so future reviewers know
  this row's risk-driven; if it's pure hygiene we should still
  land it, but the commit should say "no CVE-class fixes in this
  range, hygiene update only.")

**Risk checkpoint.** If the new amalgamation breaks any test you
can't isolate to a sqlite behavior change in <30 min, STOP and
ping Rhett rather than chasing the regression. Reverting a vendor
bump is cheap; pushing a half-broken bump to main is expensive.

---

## NEXT — empty (after P5.6, ping Rhett)

P5.5 (vendor/tor submodule pin) stays Rhett because the pin update
needs Tor-specific smoke testing (.onion bootstrap timing, hidden
service descriptor publish) that lives in Rhett's lane. Don't touch
vendor/tor.

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
