# zclassic23 — Master Agent Checklist

Derived from the full code review of 2026-04-17. Supersedes earlier wave plans.

Owner: Rhett (primary). Delegates: Agent-2 (see `AGENT-2.md`), Agent-3 (see `AGENT-3.md`).

---

## Core focus (2026-04-19 reset)

Three things matter, in order:

1. **The chain works.** Live node syncs to tip and stays there. No
   manual surgery. No 3-hour OOM cycle. Everything else is decoration
   until this is true.
2. **Consensus parity with zclassicd is provable.** Continuous diff
   against the legacy peer; CRITICAL alarm on any divergence. Without
   this, every change is rolling dice on a chain split.
3. **Recovery is automatic.** Crashes, disk corruption, power loss —
   the node comes back up and rejoins the chain on its own, every
   time. No "operator must know to delete this file."

**The rule, all bug fixes:**
1. Reproduce the failure on a fixture deterministically.
2. Identify the EXACT root cause — not the symptom.
3. Write the regression test FIRST. It must fail pre-fix.
4. Implement the fix. Test passes.
5. Then deploy.

**No hotfixes.** P8.9 (deployed) → P8.10 (proposed) was firefighting,
not engineering. Both are superseded by **P10.1** below — investigation
and test come before any fix.

---

## Progress — last update 2026-04-19 (P10.1.1 `1243e1766` + P10.1.2 writeup in `docs/postmortems/` identify the root cause — disconnect_block's `coins_map_erase` at `connect_block.c:639` doesn't propagate to the DIRTY-driven SQLite flush; P10.1.3 RED regression is next, Agent-3 reviewing the writeup)

**Overall: 67 / 85 rows closed (79%) | SWRC ~84%**

| Tier | Closed / Total | % | Open rows |
|---|---|---|---|
| **CRITICAL** | 12 / 15 | **80%** | **P10.1 (NOW)**, P8.10 (SUPERSEDED — frozen), P9.2 (deferred) |
| **HIGH** | 29 / 33 | **88%** | P9.1, P9.3, P9.4, P9.5 (all deferred) |
| **MED** | 21 / 30 | **70%** | P7.10, P8.4, P8.6, P8.7, P8.8, P9.6, P9.7, P9.8, P9.9 (all deferred) |
| **LOW** | 2 / 3 | **67%** | P9.10 (deferred) |
| (P0 baseline) | 4 / 4 | **100%** | — |

**Open by owner (2026-04-19 late night, post-reset):**
- **Agent-2 (1 row, NOW):** **P10.1.3 — RED regression test** that models the three-layer scratch→parent→SQLite shape and asserts `coins_view_cache_have_coins` returns false on the parent after `disconnect_block + flush`. P10.1.1 (`1243e1766`) shipped the fixture; P10.1.2 identifies the root cause — `coins_map_erase` at `connect_block.c:639` does not propagate to the DIRTY-driven SQLite flush — see `docs/postmortems/2026-04-19-bip30-stall.md`. Agent-3 review of the writeup is pending; P10.1.3 test lands next regardless. Three remaining P10.1 sub-rows after this: P10.1.3 → P10.1.4 minimal fix → P10.1.5 live canary. **No hotfix shortcuts.** P8.4/P8.6/P8.7/P8.8 + P7.10 follow-up stay deferred until P10.1 closes.
- **Agent-3 (on-call, 1 task):** **review Agent-2's P10.1.2 root-cause writeup** at `docs/postmortems/2026-04-19-bip30-stall.md` (landed in the same push as this status update). The writeup names the bug at `connect_block.c:639` + the enforcement boundary + the test gap; Agent-3 flags any logic holes BEFORE Agent-2 writes the P10.1.3 RED test. Their P9 sapling-prover audit (`04247c19a`) remains **deferred** until P10.1 closes. No new code from them until then.
- **Rhett:** 0 (coordinator only).

**LIVE-NODE STATUS (2026-04-19):** chain at h=3,081,407 (zclassicd at 3,082,897 — gap of 1,490 blocks). The stall is **acceptable** until P10.1 produces a tested fix — operator restarts every ~3h are tolerable; deploying another guess is not.

**Top remaining risks:** until P10.1 closes, the entire dev plan blocks. After P10.1: triage which P9 audit findings actually need fixes vs. which are theoretical, drain the deferred P8 MEDs (4 rows), then file Phase 2 work (continuous fuzzing in CI, consensus-parity diff service, sapling-tree checkpoint, structured logs).

**SWRC formula:** CRIT=4, HIGH=2, MED=1, LOW=0.5. P0 rows weighted as HIGH. Total weighted capacity = 167.5 (+4 for new CRIT P10.1).

---

## MVP target + Hardening KPI

**MVP target:** "Someone we don't know can run zclassic23 + use it for a
week without intervention." Eight binary acceptance criteria — see
[`MVP.md`](MVP.md) for the full list and CI verification.

**MRS (MVP Readiness Score, today): ~3 / 8** — criteria 1, 2, 4 likely
pass manual test; 3 partial; **5, 6, 7, 8 fail or untested**.
P10.1 (chain stall fix) is the gate to criterion 6 (7-day soak).

**HI (Hardening Index, today): ~50%** — most closed rows have tests,
but few were RED-first. Every row from P10 onward is 1.0 by
construction. **MVP achieved at MRS = 8/8 AND HI ≥ 80%.**

**Update rule for new rows:** mark `done <SHA> [test:1.0]` for the
P10.1 RED-test pattern, `[test:0.5]` for retroactive tests, `[test:0.0]`
for hotfix-only. Default = 0.0 if missing.

---

**Ground rules for every agent**
- zclassic23 is the next-gen product. zclassicd is a legacy bootstrap peer only.
- Read `CLAUDE.md` and `DEFENSIVE_CODING.md` before touching code.
- Everything is in the single binary — no standalone shell scripts, no Docker.
- `make test` MUST pass before any commit. `make ci` MUST pass before push.
- Small commits. Push frequently. No amending pushed commits.
- Do NOT touch files outside your assigned scope — conflicts cost time.

---

## Priority 0 — Build enforcement (DONE — landed via Agent-3 earlier wave)

Build lint gates are live. This unblocked every downstream agent.

| # | Task | Files | Status |
|---|---|---|---|
| P0.1 | Flip `check-raw-sqlite` from warn/exit-0 to fail/exit-1 | `Makefile:506-514` | done a5511028d (merged via bcab984fd) |
| P0.2 | Add `-DZCL_AR_ENFORCE` to `CFLAGS` | `Makefile:~37` | done a5511028d |
| P0.3 | Wire `tools/scripts/check_no_secret_printf.sh` into `make lint` | `Makefile:~543` | done a5511028d |
| P0.4 | Make `deploy` target depend on `ci` | `Makefile:298-302` | done a5511028d |

`make lint` fails-exit-1 on raw `sqlite3_step` hits; `test_make_lint_gates`
regression test locks the gates in place.

---

## Priority 1 — Money-loss / consensus-split

| # | Task | File:line | Severity | Owner |
|---|---|---|---|---|
| P1.1 | Wallet wrapper silent-error (`return true` after `LOG_FAIL`) | `lib/wallet/src/wallet_sqlite.c:259,439,571,600,661,703,759,835,938,984,1103` | CRITICAL | Agent 2 — done 8608820e7 |
| P1.2 | Flush commits partial state (rc ignored) | `lib/wallet/src/wallet_sqlite.c:1054-1072` | CRITICAL | Agent 2 — done 8608820e7 |
| P1.3 | Sapling verify fail-open on NULL VK | `lib/sapling/src/sapling.c:505, 559` | CRITICAL | Agent 3 — done 3b4b08ba9 (merged bcab984fd) |
| P1.4 | Sapling params loaded without integrity check | `lib/sapling/src/params_init.c:47-167` | CRITICAL | Agent 3 — done 785db18b1 (merged bcab984fd) |
| P1.5 | Raw `sqlite3_step` in UTXO batch writer | `lib/storage/src/coins_view_sqlite.c:461,474,509,557` | CRITICAL | Agent 2 — done 152603fdc |
| P1.6 | No P2SH sigop accounting — consensus split risk | `lib/validation/src/sigops.c:10-18` | HIGH | Agent 3 — done f6aa0b080 (mirror of zclassicd `src/main.cpp::GetP2SHSigOpCount` + `src/script/script.cpp:202-228`; new `script_get_sig_op_count_p2sh` + `get_p2sh_sig_op_count` wired into connect_block's post-`have_inputs` path; no per-input 15-cap (see AGENT-3.md notes — deferred to mempool-policy / Agent-2 lane because consensus-level cap would diverge from zclassicd); 6 unit vectors for the P2SH counter in test_validation.c covering non-P2SH fallback, simple redeem, 16×CHECKSIG raw count, CHECKMULTISIG accurate-N, non-push scriptSig, and P2SH-off fallback) |
| P1.7 | `skip_diffbits` silently skips difficulty check | `lib/validation/src/check_block.c:222,233-250` | HIGH | Agent 3 — done 5ce252bb6 (removed window_clean probe + skip_diffbits goto/label; contextual_check_block_header now always calls GetNextWorkRequired — which itself returns nProofOfWorkLimit on incomplete ancestor windows, so incomplete-window nodes compare against the weakest permitted compact instead of blindly trusting the header; fast-sync/MMB callers must bypass contextual_check_block_header entirely, as process_block.c:732-734's skip_contextual already does; regression test in test_chain.c: header with nBits=0x1d00ffff at height 101 must fail with bad-diffbits — verified 0x1d00ffff vs expected 0x1f07ffff) |
| P1.8 | Ed25519 missing `S<L` canonicality | `lib/crypto/src/ed25519.c:300-355` | HIGH | Agent 3 — done c510c7335 (merged bcab984fd) |
| P1.9 | RedJubjub missing `S<r` canonicality | `lib/sapling/src/sapling.c:386` | HIGH | Agent 3 — done 8440cd864 (merged bcab984fd) |
| P1.10 | `find_group_hash` returns ignored → silent zero generators | `lib/sapling/src/sapling.c:81-110` | HIGH | Agent 3 — done e221e0212 (merged bcab984fd) |
| P1.11 | Zero `LOG_FAIL` usage across crypto/sapling | `lib/crypto/*`, `lib/sapling/*` | HIGH | Agent 3 — done ca139a5ad (full audit across lib/crypto/ + lib/sapling/; algorithmic/retry-signal returns in fr.c documented rather than logged — see fr.c header) |
| P1.11b | Note-encryption esk nonce-reuse sanity guard (Step 8 of A3 brief) | `lib/sapling/src/note_encryption.c` | MED | Agent 3 — done 909636215 |
| P1.12 | `jub_scalar_mul` constant-time rewrite (side-channel on secret keys) | `lib/sapling/src/fr.c:307-333` | HIGH | Agent 3 — done 15218ba2f (masked linear-scan table select + unconditional-add-with-mask; diff test + timing test in test_sapling_crypto.c) |
| P1.13 | curve25519 constant-time audit (X25519 DH, esk in note encryption) | `lib/crypto/src/curve25519.c` | HIGH | Agent 3 — done da3dbbccb (audit comment in source documenting CT properties of the TweetNaCl ladder; Hamming-weight regression timing test in test_sapling.c — Wave 2 / Step H) |
| P1.14 | ed25519 constant-time pass (verify-only path, JoinSplit consensus) | `lib/crypto/src/ed25519.c` | MED | Agent 3 — done b63b149c9 (audit comment confirms verify-only file uses cswap + XOR-OR diff check; no `ed25519_sign` exists, sign-side guidance documented for future work — Wave 2 / Step I) |
| P1.15 | RNG hygiene wrapper + secret-generation call-site sweep | `lib/crypto/`, `lib/sapling/` | HIGH | Agent 3 — done 7abe359c5 (new `zcl_random_secret_bytes` wrapper rejects `GetRandBytes` all-zero fail-open; migrated esk/groth16-blind/redjubjub-T/sapling-r/sha3-nonce; lib/core/random.c root-cause flagged) |
| P1.16 | `lib/core/random.c` GetRandBytes root-cause fail-open (flagged during P1.15) | `lib/core/src/random.c` | HIGH | Agent 3 — done 94d607b85 (getrandom(2) preferred → /dev/urandom fallback → abort() on any total failure; void signature preserved per scope boundary; lib/keys/src/key.c migrated to zcl_random_secret_bytes for defense-in-depth; SIGABRT fault-injection test added to test_core.c) |
| P1.16b | `jubjub_to_scalar` constant-time reduction on Sapling nullifier path (prf_nsk + RedJubjub nonces) | `lib/sapling/src/jubjub.c:46-108` | HIGH | Agent 3 — done c841defd2 (replaced bi_cmp early-exit + bi_sub borrow-branch + per-bit branch with bi_cond_sub mask-select; acc<r selection is now a limb-wise mask derived from the subtraction's final borrow; 10k-vector diff test + 5 corners + Hamming-weight timing regression (ratio 1.002) in test_sapling_crypto.c) |

---

## Priority 2 — P2P attack surface

| # | Task | File:line | Severity | Owner |
|---|---|---|---|---|
| P2.1 | Mempool accepts any peer tx — no sig/UTXO/fee check | `lib/net/src/msg_tx.c:34-69` | CRITICAL | Agent 2 — done da318931d (msg_tx_accept classifies every incoming `tx` into a 7-way enum — OK / INVALID / DUPLICATE / CONFLICT / BELOW_FEE / MISSING_INPUTS / INTERNAL_ERROR — and records PEER_OFFENCE_INVALID_MESSAGE for INVALID / CONFLICT only; orphan / duplicate / below-fee drop silently as rate-limit. Fee = coins_view_cache_get_value_in - transaction_get_value_out checked against pool->min_relay_fee. tx_mempool_has_conflict is a new read-only probe in lib/validation/ so conflicts are attributed before tx_mempool_add_unchecked folds them into its generic bool. 3 regression tests in test_mempool.c: invalid-vout → INVALID + ban 10; double-spend → CONFLICT + ban on second peer only; below-fee → BELOW_FEE + no ban. Bonus: uncovered + fixed a self-deadlock in disk_block_io_close_cache from block_pruning_service — see commit 3979340c9.) |
| P2.2 | 1.6 MB stack alloc in message handler | `lib/net/src/msg_tx.c:288` | CRITICAL | Agent 2 — done 352a83167 (process_mempool scratch now heap-allocated via zcl_malloc; LOG_FAIL on OOM; file-scope test hook `msgprocessor_test_set_mempool_alloc_hook` drives the forced-OOM path; 2 regression tests in test_mempool.c — 100-tx happy path pushes all inv through p2p_node_push_inventory, forced-OOM returns false with inventory_to_send untouched) |
| P2.3 | fast_sync bypasses AR_BEGIN_SAVE | `lib/net/src/fast_sync.c:480-526` | HIGH | Agent 2 — done 9ef77899b (migrated bulk-insert loop to AR_BIND_* + AR_STEP_DONE; regression test builds a 2-entry chunk with CHECK-violating height and asserts BEGIN/COMMIT rollback atomicity) |
| P2.4 | Swarm per-chunk hash verification effectively absent | `lib/net/src/fast_sync.c:892-895`, `msgprocessor.c:1968` | HIGH | Agent 2 — done 9e8cfbb27 (zmanifest carries per-chunk SHA3 hashes + merkle-root reconstruction check; swarm_sync_init requires chunk_hashes + bounds num_chunks at MANIFEST_MAX_CHUNKS; 3 regression tests prove bad chunk → 0 rows + retry, good chunk → 3 rows, init refuses NULL/oversized) |
| P2.5 | connman deadlock risk: `cs_nodes` held across callback | `lib/net/src/connman.c:802-836` | HIGH | Agent 2 — done cd4b3c42f (thread_message_handler replaced with connman_run_message_cycle: snapshot+add_ref under cs_nodes, run callbacks with NO lock, re-acquire cs_nodes to drop refs; connman_run_deferred_free_sweep re-parks entries with ref_count>0 so in-flight snapshots can't be UAF'd; deferred_free cap bumped 64→256 via CONNMAN_DEFERRED_FREE_CAP; immediate-free path grows a ref-count safety belt; ZCL_STRESS_TESTS-guarded 50-peer × 1s test in test_net.c — 142M cycles, 1.9K callbacks, deferred_free drains clean) |
| P2.6 | `g_swarm_active` TOCTOU → state leak | `lib/net/src/msgprocessor.c:1961-1981, 2040` | HIGH | Agent 2 — done 658b6fe5d (atomic_compare_exchange_strong flips the check+claim into one op; CAS winner runs swarm_sync_init under g_swarm_mutex, releases the claim on init failure; CAS loser drops the message; reset site uses explicit atomic_store; 3 regression tests: no-race, pthread-barrier-synchronized concurrent racers, reset cycle — also flagged sibling g_block_swarm_active TOCTOU at 2439/2451 as a separate P2.x row candidate) |
| P2.7 | FlyClient challenge amplification — no rate limit | `lib/net/src/msgprocessor.c:1864-1900` | MED | Agent 2 — done a46410c50 (per-peer token bucket: burst 30, refill 10/sec, drop silently + PEER_OFFENCE_FLOOD once per episode; LRU side table so peer churn can't grow memory; 3 regression tests: flood caps near burst+rate, ban-score registers once, victim peer unimpeded) |
| P2.8 | No global byte budget on recv queue | `lib/net/src/net.c:104-115` | MED | Agent 2 — done 60bb08f58 (atomic process-wide counter + env-configurable cap, default 256 MiB; regression test exhausts a 16 KiB cap and verifies rollback on over-cap alloc) |

---

## Priority 3 — MCP / application layer

| # | Task | File:line | Severity | Owner |
|---|---|---|---|---|
| P3.1 | MCP `zcl_send` JSON injection via `from`/`to` | `tools/mcp/controllers/wallet_controller.c:53-55` | CRITICAL | Agent 2 — done b0134339b (JSON encoder builder + class sweep across wallet/chain/net/app controllers; 3 injection tests) |
| P3.2 | MCP `zcl_sendtoaddress` JSON injection via `address` | `tools/mcp/controllers/wallet_controller.c:76-77` | CRITICAL | Agent 2 — done b0134339b |
| P3.3 | ~80 raw `sqlite3_step` in controllers and services | `app/controllers/*`, `app/services/*` | HIGH | Agent 2 — done 2a59ac938 (~115 sites migrated across 17 files; 5 state-kv/rollback opt-outs retained with descriptive scope annotations) |
| P3.4 | `store_controller` accepts addresses without checksum | `app/controllers/src/store_controller.c:663-685` | HIGH | Agent 2 — done 64a4afffc (Base58Check + Bech32 verification; 400 on bad checksum; 2 regression tests) |
| P3.5 | `rpc_client.c` realloc overwrite w/ no NULL check | `tools/mcp/rpc_client.c:126` | HIGH | Agent 2 — done f0e8d31d3 (zcl_realloc via tmp; only call-site) |
| P3.6 | `parse_form_field` does not URL-decode; no CSRF token | `app/controllers/src/store_controller.c:803-823` | MED | Agent 2 — done efa211811 (URL-decode ported; HMAC-bound per-product-id form token; 3 regression tests) |
| P3.7 | `/metrics` open on TLS listener with no auth | `lib/rpc/src/httpserver.c:355-381` | MED | Agent 2 — done 877d68218 (Basic-auth via check_auth; 401 on no/wrong creds, 200 on valid cookie; regression test drives all three paths over a loopback socket against a real rpc_http_start) |

---

## Priority 4 — Script / consensus memory safety

| # | Task | File:line | Severity | Owner |
|---|---|---|---|---|
| P4.1 | 520 KB `script_stack` passed by value, on-stack | `lib/script/include/script/interpreter.h:22-30`, `interpreter.c:619-652` | HIGH | Agent 2 — done a9fcf6c66 (paired with P4.2; heap-owned items via stack_init/stack_free, cleanup attribute in eval_script + verify_script, stack_copy_active replaces 520 KB struct assignments) |
| P4.2 | Silent `stack_push` failures corrupt later stack assumptions | `lib/script/src/interpreter.c:619-620` | HIGH | Agent 2 — done a9fcf6c66 (paired with P4.1; PUSH_OR_FAIL / SN_PUSH_OR_FAIL / INSERT_OR_FAIL propagate SCRIPT_ERR_STACK_SIZE to eval_script caller) |
| P4.3 | `script_num_serialize` lacks outsize bounds check | `lib/script/include/script/script.h:239-258` | MED | Agent 2 — done 61104d06d (precompute required length + reject-if-short instead of silent-truncate; 6 boundary assertions in test_script.c) |
| P4.4 | `disconnect_block` unbounded realloc on `vin.prevout.n` | `lib/validation/src/connect_block.c:586-607` | MED | Agent 2 — done f69956cab (clamp prevout.n ≥ MAX_BLOCK_SIZE → LOG_FAIL + reject; regression test in test_validation.c uses UINT32_MAX to exercise the previously-~128 GB realloc path) |
| P4.5 | `sigencoding` strict-DER bound inconsistency vs Bitcoin | `lib/script/src/sigencoding.c:11-56` | MED | Agent 2 — done 28fe53112 (byte-for-byte parity audit vs zclassic-cpp sigencoding.cpp: no divergence — "off-by-one" was a false positive from the brief; 16-vector BIP66-style parity table added to test_script.c locks the canonical boundary behavior in place) |

---

## Priority 5 — Operator / deploy hygiene

| # | Task | Severity | Owner |
|---|---|---|---|
| P5.1 | `export_snapshot` (1.1 MB ELF) tracked in git despite `.gitignore` | HIGH | Agent 2 — done a9ac382b7 |
| P5.2 | `deploy/zclassic23.service:21` hardcodes Rhett's externalip + 9 addnodes | HIGH | Agent 2 — done ba450ea5c (operator flags moved to EnvironmentFile=-~/.config/zclassic23/env; $VAR form preserves whitespace-splitting for multi-flag ZCL_ADDNODE_FLAGS; fresh clone starts clean when env file absent; deploy/zclassic23.env.example ships the template with Rhett's current values; README gets a one-paragraph pointer; smoke-test on Rhett's box: height 3081407 → 3081408, 4 peers, 205.209.104.118:8033 advertised in getnetworkinfo.localaddresses) |
| P5.3 | Hardcoded `/home/rhett` in `tools/export_snapshot.c:15`, `tools/zcl-nodectl.c:628-637` | HIGH | Agent 2 — done 09e4fb15a (shared $HOME helper in lib/util/include/util/rpc_paths.h; also swept test_phgr13_fix.c sprout-VK path + two README absolute-path links; new test_no_hardcoded_home regression test scans every deployed binary for the literal and exercises the helper with alt/NULL HOME) |
| P5.4 | 10 shell scripts in `tools/` duplicating MCP — purge | MED | Agent 2 — done 0f33d3fc1 (audit found 1/8 actual MCP-duplicates: verify_restart_follow.sh ⇒ zcl-nodectl verify-follow; the other seven are build-time or multi-node orchestration with no MCP equivalent — per-script rationale in "Notes from Agent-2" in AGENT-2.md) |
| P5.5 | `vendor/tor` submodule ahead of pinned commit | MED | Agent 3 — done 75576d7a0 (pin bumped d14113e → 73bd405; ahead commits 39feeefa1 dynhost-outbound-API + 73bd405d1 symbol-conflict-fix are already live on upstream origin/main; only outer-repo tree entry changed via `git update-index --cacheinfo 160000,73bd405...,vendor/tor`; libtor.a untouched, local binary byte-identical; `git submodule status` in Rhett's main clone drops the `+` prefix once he pulls; live .onion smoke test is Rhett's to trigger from `~/zclassic23` because the systemd service ExecStart points at `/home/rhett/zclassic23/zclassic23`, not at this clone) |
| P5.6 | Vendored `sqlite3.h` is 3.49.0 — newer CVE-class fixes unpicked | MED | Agent 2 — done 30e6fbc2e (pinned to 3.53.0 — latest stable as of 2026-04-09; picks up 3.49.1 concat_ws buffer overrun, 3.49.2 NOT NULL memory error, 3.50.3 CREATE TRIGGER parser memory-safety regression from 3.49.0, 3.50.4 uninit-var reads, 3.51.0 POSIX-advisory-lock-abuse corruption detection, 3.51.3 + 3.53.0 WAL-reset corruption bug; header surface additive-only — new error codes, de-experimentalized snapshot_\* family, new carray_bind_v2 / db_status64 / str_free / str_truncate / set_errmsg / setlk_timeout / changeset apply_v3 entry points; on-disk format unchanged; archive rebuilt from sqlite.org amalgamation with SHA3-256 c2325c53 verified, stock defaults matching the prior archive's embedded compile-options table; full test_zcl passes 2516/2516 through every sqlite-backed group before the pre-existing test_block_pruning hang) |
| P5.7 | Repo-root clutter: 40+ .md, `node.db` untracked at repo root | LOW | Agent 2 — done 611ae4281 + e7528c4f0 + 8902f9ae7 + d106192a4 (root-level .md cut 41→18; WAVE_6-12, AGENT2/3-era task docs, BOOT/REVIEW/CHECKLIST/MEMORY moved to docs/archive/; speedrun + zclassic23-asan binaries untracked) |

---

## Priority 6 — Wallet/storage medium (Owner: Agent 2)

| # | Task | File:line | Severity |
|---|---|---|---|
| P6.1 | `write_sapling_key` silent UPDATE failure → address collision | `lib/wallet/src/wallet_sqlite.c:822-830` | HIGH — done 8608820e7 |
| P6.2 | Flusher resets all shared-conn statements → reader rewound | `lib/storage/src/coins_view_sqlite.c:419-426` | MED — done 152603fdc |
| P6.3 | `read_keys` silently skips malformed rows | `lib/wallet/src/wallet_sqlite.c:533-553` | MED — done 8608820e7 |
| P6.4 | Migration framework unchecked bookkeeping writes | `lib/storage/src/schema_migration.c:134,169,230` | MED — done 767d9d3e7 |
| P6.5 | `write_best_block`/`write_scan_height` re-prepare every call | `lib/wallet/src/wallet_sqlite.c:642-705` | MED — done 8608820e7 |
| P6.6 | `coins_alloc` OOM silent (treated as "no outputs") | `lib/coins/src/coins.c:54-55,106-110` | LOW — done dc60b7e7b |

---

## Priority 7 — Live-node + post-AGENT.md fresh review (2026-04-18)

Surfaced after the original 53-row checklist drained to ~85%. Live
inspection of the running node + node.log revealed a hard outage and
several latent operability gaps. P7.1 + P7.2 are blocking the live
chain right now; the rest are next-wave hardening.

| # | Task | File:line | Severity | Owner |
|---|---|---|---|---|
| P7.1 | Tip stuck at 3,081,601 — `val.block_connected` repeats for same height; `chain_height` never advances | `lib/validation/src/process_block.c:525`, connect_tip/disconnect_tip | CRITICAL | Agent 2 — done a6bedccad (root cause: `update_tip` was `static void` and discarded the bool from `process_block_commit_tip`; when csr refused the commit the tip never advanced but connect_tip still returned true, so every inbound block re-emitted EV_BLOCK_CONNECTED for the same height until RSS filled. Fix: update_tip now returns bool; connect_tip + disconnect_tip propagate via `validation_state_error(state, "csr-tip-commit-rejected"/"csr-tip-rollback-rejected")`; test wrapper `process_block_test_update_tip` drives a csr-initialized singleton with an orphan block_index through CSR_REJECTED_TIP_NOT_IN_INDEX and asserts caller sees false + chain tip unchanged + rejected counter ticks. Live verification deferred to Rhett's `make deploy` + `zcl_status` — remote node still points at `~/zclassic23/zclassic23`, not this clone's binary.) |
| P7.2 | Boot logs `DB_ERR_TIP_MISMATCH ... halt and investigate` then keeps running — must be fatal or auto-rewind | `app/services/src/chain_state_repository.c` (audit + tighten the halt path) | HIGH | Agent 2 — done 57e6ef391 (single-block overshoot auto-rewind with ≤32 row guard in lib/storage/src/coins_view_sqlite.c + hard `_exit(EXIT_FAILURE)` + EV_BOOT_VALIDATION_FAILED event at the config/src/boot.c caller; three regression tests in test_coins_view_atomicity.c cover auto-rewind, guard refusal at 33 rows, and two-block multi-overshoot; commit body flags the small config/src/boot.c scope touch for re-routing if needed) |
| P7.3 | Crash handler runs but FATAL header + `backtrace_symbols_fd` output never reaches `node.log` (only `sys.crash` event survives) | `lib/event/src/event.c:610-630` | HIGH | Agent 2 — done e9e79dda2 (fprintf header replaced with async-signal-safe write(STDERR_FILENO) on a 128-byte snprintf buffer; fflush(stderr) added at the tail of event_dump_recent; belt-and-suspenders fflush + fsync before _exit(128+sig); regression test forks a child, dup2's stderr to temp file, installs crash handler post-fork, raise(SIGABRT) and asserts FATAL SIGNAL 6 literal + ≥3 hex backtrace addresses) |
| P7.4 | Backpressure missing under tip-stuck loop — RSS climbs to 6.0G (cgroup MemoryHigh) accumulating block buffers when tip doesn't advance | `lib/net/src/msgprocessor.c`, `download.c` | HIGH | Agent 3 — done f6474c77b ( new `lib/net/src/tip_watchdog.c` + `lib/net/include/net/tip_watchdog.h`; INACTIVE→ACTIVE when `now - last_tip_advance > 60s` AND estimated download-queue bytes > 256 MiB, ACTIVE→INACTIVE on tip advance or 120s cooldown; tip-advance signal hooked via sync observers on EV_BLOCK_CONNECTED + EV_TIP_UPDATED registered in msg_processor_init; tick at the top of msg_process_messages; post-parse / pre-dispatch reject layer in msg_process_messages for "inv" and "block" emits EV_BACKPRESSURE_REJECT but does NOT bump ban-score; drain via new `dl_drain_for_backpressure` clears queued hashes + in-flight slots without touching the header chain; 3 default-mode unit tests + 1 ZCL_STRESS_TESTS-guarded 1000-orphan flood test in test_net.c — all green) |
| P7.5 | `deploy/zclassic23.service:34` `TimeoutStopSec=300` amplifies hangs into 5-min outages; trim to 60-90s + watchdog | `deploy/zclassic23.service` | MED | Agent 2 — done ec7948ee3 (TimeoutStopSec=90, bounds hung shutdown to 90s instead of 5-min outage; clears observed worst-case ~60s WAL+Tor teardown with headroom; landed as a one-commit batch with P7.6+P7.7) |
| P7.6 | `StartLimitBurst=3 / StartLimitIntervalSec=300` can permanently disable service after 3 crashes in 5min | `deploy/zclassic23.service` | MED | Agent 2 — done ec7948ee3 (StartLimitBurst=10, StartLimitIntervalSec=600 — 10 attempts over 10 minutes gives real triage time before the unit fails; P7.2's boot halt keeps a genuine chain-state bug draining the burst to the "unit stopped" clean signal) |
| P7.7 | `LimitCORE=` not set — first SIGABRT today produced no usable post-mortem | `deploy/zclassic23.service` | MED | Agent 2 — done ec7948ee3 (LimitCORE=infinity + ZCL_CORE_DIR env hint for lazy mkdir on first abort; inline systemd-coredump / plain-pattern core_pattern doc for the per-host sysctl the operator still has to set) |
| P7.8 | SQLite default `cache_size` (~2 MB) and `mmap_size` (0) on a 1.3M-row chainstate; `boot_index.c:307` warns mmap_size=64MB previously caused SIGSEGV — pick safe values + lock with a test | `lib/storage/src/coins_view_sqlite.c:187`, schema_migration.c | MED | Agent 2 — done dbca0be78 (audit found node.db already tuned at cache_size=-65536 / mmap_size=256MB via db_set_pragmas in app/models/src/database.c — refactored the values under named constants ZCL_NODE_DB_CACHE_SIZE_KIB / ZCL_NODE_DB_MMAP_BYTES for single-point future edit; regression tests in test_sqlite.c lock the PRAGMA cache_size reading == -65536 and cover a 100k-UTXO seed + 100 random-read smoke check against SIGSEGV / reader-rewind; boot_index.c:306 landmine root cause documented — standard SQLite mmap-vs-WAL-checkpoint aliasing, safe mitigation is the current "main handle mmap ON, all secondary mmap=0" split; AGENT-2.md flags fast_sync/onion_service/load_balancer RO-open sites in Rhett's lane as future tuning opportunities) |
| P7.9 | No central thread registry — 12+ `pthread_create` sites, each shutdown signals its own flag; no single function joins them all | `lib/util/src/sync.c` (or new `lib/util/src/thread_registry.c`), all spawn sites | HIGH | Agent 2 — done 19b2cac1d (new `lib/util/include/util/thread_registry.h` + impl with `thread_registry_spawn` / `_shutdown_requested` / `_request_shutdown` / `_join_all`; main.c signal handler bridges the legacy `g_shutdown_requested` sig_atomic_t to the new atomic flag; 50-thread stress test + straggler diagnostic in test_thread_registry.c satisfies AGENT-2.md acceptance; call-site migration is P7.10) |
| P7.10 | `g_shutdown_requested` checked in only 6 files — `bg_validation`, `header_sync`, `peer_strategy`, `scheduler`, `workpool` either don't check it or use a different flag | cross-cutting (audit) | MED | Agent 2 — infrastructure landed via P7.9 (`19b2cac1d`); open follow-up: migrate long-running loops (`bg_validation`, `header_sync`, `peer_strategy`, `scheduler`, `workpool`, plus the net/tor/https listeners) to poll `thread_registry_shutdown_requested()` alongside their local stop flags |

---

## Priority 8 — Fresh review wave (2026-04-19, post-P7 drain)

Surfaced by Agent-3's audit pass after the P1–P7 closes. Focus areas
were the subsystems the first two reviews skipped: P2P feature
protocols (zmsg, dandelion, file_market, p2p_game), token/name
protocols (zslp, znam), chain proofs (mmb, flyclient, compact_blocks,
bloom), application controllers (swap, game, messaging, file_market,
explorer), and mining. Subsystems re-audited and found clean: swap
controller, game controller, messaging controller, explorer
controller (XSS-escape and request-validation paths only), p2p_game
wire, dandelion epoch lifecycle, mining loop, flyclient sample/index
verification, znam parser bounds, store controller (post-P3.4/P3.6).

| # | Task | File:line | Severity | Owner |
|---|---|---|---|---|
| P8.1 | `zmsg_deserialize` reads peer-controlled `uint8_t slen`/`rlen` (range 0-255) and copies that many bytes into `msg->sender[ZMSG_MAX_ADDR=128]` / `msg->recipient[128]` with no bounds check, then writes `'\0'` at `sender[slen]`. A peer sending `slen=255` corrupts ~127 bytes past the field into the adjacent `recipient`/`body` fields of the heap/stack-resident `struct zmsg_message`. Serialize side caps at 127; deserialize does not. Reachable from any handshake-complete peer that sends a `zmsg` P2P message — no auth, no rate limit on the parse layer. | `lib/net/src/zmsg.c:50-60` (struct in `lib/net/include/net/zmsg.h:43-53`) | CRITICAL | Agent 2 — done b6726f83b (LOG_FAIL-return on slen/rlen >= ZMSG_MAX_ADDR and blen >= ZMSG_MAX_BODY; blen check tightened from `>` to `>=` since the trailing NUL at body[ZMSG_MAX_BODY] was already one past the end; 3-case regression test in test_protocols.c covers sender/recipient/body overflows) |
| P8.2 | Dandelion stem-peer PRNG seeded with `(uint64_t)time(NULL) ^ 0xdeadbeefcafe1234ULL` — ~31 bits of effective entropy. The same seed drives the Fisher-Yates shuffle that selects this epoch's stem peers AND the per-tx fluff coin-flip. An attacker with rough boot-time + epoch-rotation timing can replay the xorshift64 state and predict (a) which 2 outbound peers the node uses for stem relay this 10-min epoch, and (b) the stem/fluff outcome of every transaction the node originates — defeating Dandelion's origin-privacy property. Note: callers all hold `ds->cs` so the data race the original review flagged is NOT real; the seed-quality issue is the actual bug. | `lib/net/src/dandelion.c:42-54` | HIGH | Agent 3 — done 576b5cde2 (replaced static xorshift64 with `zcl_random_secret_bytes` for both the Fisher-Yates shuffle in `dandelion_maybe_rotate_epoch` and the per-tx coin-flip in `dandelion_should_stem`; safe-fail policy on RNG failure leaves `num_stem_peers=0` / returns `false` so txs fluff via normal relay rather than picking with a compromised RNG; audit comment in `dandelion.c` documents the cryptographic-RNG dependency; 2 new tests in `test_dandelion.c` via ZCL_TESTING-gated hooks — back-to-back shuffle non-determinism + 10k coin-flip ±3σ uniformity around 9000 expected) |
| P8.3 | `mmb_deserialize` reads each mountain's `height` as a raw little-endian `uint32_t` from the input buffer with no upper-bound check (only `nm` is capped against `MMB_MAX_MOUNTAINS`). For any practical chain, `height` must be `≤ ⌈log2(num_leaves)⌉ ≤ 64`, but the code happily accepts `UINT32_MAX`. Downstream `mmb_merge_after_insert` (lines 100-115) increments `height` during merges — a deserialized state with `height` near `UINT32_MAX` triggers signed/unsigned wraparound on the next `mmb_append`, silently corrupting the FlyClient/snapshot trust root. Snapshot input may transit fast-sync/swarm before P2.4's hash check binds — defense-in-depth gap. | `lib/chain/src/mmb.c:254-261` | HIGH | Agent 3 — done c06515cbd (new `MMB_MAX_HEIGHT=64` cap in `lib/chain/include/chain/mmb.h`; `mmb_deserialize` rejects any mountain whose height exceeds the cap and re-zeroes the struct so no poisoned peak leaks back; mirror guard in `mmb_merge_after_insert` returns -1 (propagated by `mmb_append`/`mmb_append_hash`) when pre-increment height is at the cap, catching in-memory corruption that bypasses the deserialize path; 3 new tests in `test_mmb.c` — malicious-blob reject + real-chain round-trip at 8192 leaves + wraparound guard at UINT32_MAX-1 and at MMB_MAX_HEIGHT) |
| P8.4 | Compact-block reconstruction does an O(slots × mempool) linear scan: for every unfilled slot, the reconstructor recomputes `compact_block_short_txid()` (siphash) over **every** mempool entry until it finds a 6-byte match. With a 5 000-tx block and a 25 000-tx mempool that is 125 M siphash + memcmp ops per inbound compact block. A peer can amplify by sending compact blocks whose short-txids are placed late in the mempool iteration order. Code already comments `Simple O(n*m) for now; sufficient for typical mempools.` — promote to a `khash` short-txid table built once before the slot loop. | `lib/net/src/compact_blocks.c:272-319` | MED | Agent 2 |
| P8.5 | `bloom_filter_init_internal` only applies the `MAX_BLOOM_HASH_FUNCS` cap when `constrained=true` (the public `bloom_filter_init` path). The internal `rolling_bloom_init` path passes `constrained=false` and lets `num_hash_funcs = (data_size * 8 / num_elements * LN2)` grow without ceiling. Every subsequent `rolling_bloom_insert` / `contains` runs that many siphash iterations per call. Pathological tuning (small `num_elements`, large `data_size` from a tight `fp_rate`) produces hot-path CPU blow-up. Extract the `MIN(ideal, MAX_BLOOM_HASH_FUNCS)` clamp into both branches. | `lib/bloom/src/bloom.c:47-52` | MED | Agent 3 — done 21da0531e (lifted the `MIN(ideal, MAX_BLOOM_HASH_FUNCS)` clamp out of the `constrained` branch in `bloom_filter_init_internal`; the `constrained` flag still gates the filter_bits sizing cap, but hash-func count is now always bounded on both paths — public `bloom_filter_init` and internal `rolling_bloom_init`; 3 new tests in `test_bloom.c` — rolling-path clamp with pathological tuning (num_elements=1, fp_rate=1e-30 → 97 hash funcs pre-fix, 50 post-fix on both b1/b2), sane-params regression (120k elements / 1e-6 fp → 19 hash funcs, well under cap), public-path clamp regression (same pathological params still clamp to 50)) |
| P8.6 | `zslp_service_validate_token_key` returns true for `is_alphanumeric(token_key, len)` with `len ∈ [1, ZSLP_MAX_TOKEN_KEY_LEN]` **OR** the 64-char hex case. Because the alphanumeric branch has no length floor that distinguishes ticker-like names from truncated txids, a 10-char alphanumeric "txid" prefix is accepted and later canonicalized via `snprintf` — different short/long inputs may collide on the same canonical form. RPC callers can then look up tokens by ambiguous short keys; SEND requests targeting an aliased key reach the wrong token's payment service. | `app/services/src/zslp_service.c:62-72` | MED | Agent 2 |
| P8.7 | `zmarket_offer` computes `offer.num_chunks = (uint32_t)((size_bytes + FILE_MARKET_CHUNK_SIZE - 1) / FILE_MARKET_CHUNK_SIZE)` with no upper bound on `size_bytes`. `st.st_size` from `stat(2)` is operator-supplied (RPC argument is a path on the local filesystem), so a sparse file or a deliberately crafted manifest produces `size_bytes` near `UINT64_MAX`. The `+ FILE_MARKET_CHUNK_SIZE - 1` then wraps to a tiny value, making `num_chunks` underestimate the true chunk count. Downstream chunk-challenge / payment math operates on a wrong chunk count without ever logging the mismatch. Lower priority because the input is operator-controlled, but worth a one-line `if (size_bytes > UINT64_MAX - CHUNK_SIZE) reject` guard. | `app/controllers/src/file_market_controller.c:140-142` | MED | Agent 2 |
| P8.8 | ZNAM builder gates diverge from the parser. `znam_build_register` and `znam_build_update` reject `target_type > 3` (literal — covers only ONION/ZADDR/TADDR), but `znam_parse` and `znam_build_set_record` accept up to `ZNAM_TYPE_CONTENT = 7` (BTC/LTC/DOGE/CONTENT). A user calling REGISTER with a BTC address gets a silent zero-bytes-written return and no logged reason. Either lift the literal-3 cap to `ZNAM_TYPE_CONTENT` (intentional design — REGISTER should support multi-coin per the ENS-inspired spec) or document the restriction with a `LOG_FAIL`. | `lib/znam/src/znam.c:189-205` (vs parser at `:125`) | MED | Agent 2 |
| P8.9 **HOTFIX** | P7.2 incomplete rewind: auto-rewind at boot removes UTXO rows where `height > tip_height` but leaves the corresponding `(txid, vout)` entries in the coins cache view, so the next `connect_block(tip+1)` trips BIP30 on the coinbase (txid already present, not pruned). Live-node evidence 2026-04-18 post-deploy: boot log says `[coins] auto-rewind: removed 2 UTXO row(s) above tip_height=3081407`, then `connect_tip: connect_block FAILED h=3081408: bad-txns-BIP30` repeats on every activate_best_chain cycle. Header tip advances to 3,082,600+, chain tip pinned at 3,081,407. Fix must rewind ALL rows the partial-block application touched — not just the ≤32 rows above tip, but any stale (txid, vout) entries for block h=tip+1 — OR treat an anchor-adjacent BIP30 hit as evidence of incomplete rewind and retry after a deeper purge. | `lib/coins/src/coins_view_sqlite.c` (P7.2 rewind) + `lib/validation/src/connect_block.c:212-233` (BIP30 check) | CRITICAL | Agent 2 — done b875152da (strengthened rewind sweeps utxos by txid when tx_index rows sit above tip; also purges the stale tx_index rows; 2 new cva tests repro the wrong-height orphan-coinbase shape and the original height>tip path) — **regression captured by P8.10** |
| P8.10 **HOTFIX-2** | **P8.9 was incomplete + persistent-stall causes BLOCK_FAILED_CHILD memory growth.** Live-node evidence (2026-04-19 23:22 deploy of `b875152da`): boot showed `[coins] tip check OK: max_utxo_height=3081408 tip_height=3081408`, RPC briefly returned h=3,081,408 (verified by background watcher), then within 3h chain regressed back to h=3,081,407 with `connect_tip: connect_block FAILED h=3081408: bad-txns-BIP30` resuming. Each retry emits `Propagated BLOCK_FAILED_CHILD to 973 descendants` (range 363–1467 across runs). Memory climbed from 2.3G post-boot to 5.9G/6.0G in 2h51m. Two distinct bugs: (1) the chain-disconnect from 3,081,408 back to 3,081,407 happens without a reorg log line — there's a path that revisits the block-validity decision and trips on the same BIP30 staleness P8.9 was supposed to fix, suggesting the strengthened sweep doesn't run on the disconnect→reconnect path; (2) the BLOCK_FAILED_CHILD propagation accumulates marks in the block index without GC, so persistent connect failure is itself an OOM amplifier independent of the download-queue path P7.4 watches. Fix must (a) make the strengthened sweep idempotent on the disconnect→reconnect cycle, AND (b) cap or GC the BLOCK_FAILED_CHILD propagation under persistent failure. | `lib/coins/src/coins_view_sqlite.c` (P8.9 sweep) + `lib/validation/src/process_block.c` (BLOCK_FAILED_CHILD propagation) + `lib/validation/src/chainstate.c` (disconnect→reconnect path) | CRITICAL | Agent 2 — HOTFIX NOW |

**Triage notes for Rhett:**
- P8.1 is a wire-format heap overflow reachable from any peer; promote
  ahead of Agent-2's other queue items.
- P8.3 is the only natural fit for Agent-3 — adjacent to the
  P5.5/MMB/FlyClient lane Agent-3 already owns. Everything else falls
  in Agent-2's net/app/zslp/znam/bloom lanes.
- Subsystems re-audited and clean: swap controller, game controller,
  messaging controller, explorer controller (request-parse + HTML-
  escape paths), p2p_game wire, dandelion epoch lifecycle, mining
  thread loop, flyclient sample/index verification, znam parser
  bounds, store controller (post-P3.4/P3.6).

---

## Priority 9 — Sapling-prover deep audit (2026-04-19, post-P8 drain) — **ALL DEFERRED until P10.1 closes**

> **DEFERRED 2026-04-19 (post-reset):** All P9.1–P9.10 rows are
> real bugs and stay open, but **no fix work starts until P10.1
> (chain-stall investigation) closes**. The core focus is "the
> chain works" — sapling-prover hardening is improvement on top.
> Agent-3 is on-call to review Agent-2's P10.1.2 root-cause
> writeup, NOT to start fixing P9.x rows.
>
> When P10.1 closes, Rhett re-triages P9.1–P9.10. Triage notes
> from the original audit are preserved at the bottom of this
> table.

Surfaced by Agent-3's sapling-only audit after P8.5. P1 wave touched
the API surface; this wave hit prover internals: `groth16_prover.c`,
`sapling_circuit.c`, `sapling_prover_c23.c`, `msm_parallel.c`,
`pedersen_hash.c`, `incremental_merkle_tree.c`, `sprout.c`, and the
`g1_scalar_mul` exit in `bls12_381.c`. Re-audited clean: `circuit_gadgets.c`,
`bn254.c`. `jubjub.c`, `fr.c`, `note_encryption.c`, `sapling.c` already
P1-audited and not re-scanned.

| # | Task | File:line | Severity | Owner |
|---|---|---|---|---|
| P9.1 | `g1_scalar_mul` is a naïve variable-time double-and-add: `if ((scalar[i] >> bit) & 1) g1_add(...)` branches on every scalar bit. Called from `groth16_prover.c:771,799,825,834,845` with the secret Groth16 blinding scalars `r_blind`, `s_blind`, `r·s`. Leaks blinding via branch-predictor / cache timing, breaking zero-knowledge (not soundness — a timing-observer recovers blinding and can check candidate witnesses). Same bug class as P1.12 / P1.16b fixed for `jub_scalar_mul`. | `lib/sapling/src/bls12_381.c:1708-1722` (prover sites in `groth16_prover.c:771,799,825,834,845`) | HIGH | Agent 3 |
| P9.2 | `sapling_circuit.c` ships two `/* placeholder */` paths with UB that sit in production `sapling_create_spend_proof` / `sapling_create_output_proof`. At :161-162, `struct jub_point nk_point; jub_scalar_mul(&nk_point, &nk_point, wit->nsk);` — `nk_point` is uninitialized AND self-aliased as input/output of scalar mul. At :65, `jub_scalar_mul(&rcm_point, &hash_point, rcm); /* placeholder */` then the function just returns `hash_point.x` — rcm is discarded, note commitment not randomized per spec. Both reachable from `zclassic_sapling_spend_proof` / `output_proof` (wallet path). Proofs produced here cannot verify. If paths are shadowed by another impl, delete with an explicit LOG_FAIL rather than leaving UB live. | `lib/sapling/src/sapling_circuit.c:65, 161-162` | CRITICAL | Agent 3 |
| P9.3 | `lc_add_term` / `cs_alloc_var` / `cs_enforce` return `void` and silently drop their input on `zcl_realloc` failure. On OOM mid-proof: (a) terms vanish, (b) new var alloc returns 0 = CS_ONE index, aliasing every subsequent var to ONE, (c) constraints vanish. Output CS is syntactically valid but semantically wrong; prover emits a Groth16 proof for a different circuit than the verifier expects. No log, no signal. Only surfaces under memory pressure — exactly when the node is struggling. | `lib/sapling/src/groth16_prover.c:38-44, 91-103, 117-145` | HIGH | Agent 3 |
| P9.4 | `fr_fft` / `fr_fft_parallel` silently no-op on non-power-of-2 input: `if ((size_t)1 << log_n != n) return;`. Caller continues with un-FFT'd data → invalid proof. Currently unreachable via `groth16_prove:648-650` which rounds domain to a power of 2, but the pattern sits in the prover waiting for the next caller. Promote to `bool`, LOG_FAIL on bad input, propagate. | `lib/sapling/src/groth16_prover.c:222`; `lib/sapling/src/msm_parallel.c:333,340` | HIGH | Agent 3 |
| P9.5 | Two lazy Sapling caches init without thread-safety: `pedersen_hash.c:14-42` (`ensure_generators` / `cached_generators[6]`) and `incremental_merkle_tree.c:92-107` (`ensure_sapling_empty_roots` / 33-entry cache), both guarded only by a plain `static bool`. Two concurrent first-callers (wallet RPC thread + bg_validation) race: one reads a partially-initialized cache entry (zero point → silently wrong Pedersen hash → wrong commitment). Manifests as transient "invalid commitment" errors in the first minute after boot under load, gone after caches stabilize. Fix with `pthread_once` or atomic+mutex double-check. | `lib/sapling/src/pedersen_hash.c:14-42`; `lib/sapling/src/incremental_merkle_tree.c:92-107` | HIGH | Agent 3 |
| P9.6 | `zclassic_sapling_spend_proof` takes `const unsigned char *witness` with no length; loop at :185-188 reads `1 + 32*33 = 1057` bytes gated only by the `depth != 32` first-byte check at :181. Not currently exploitable (live callers hand-build the witness), but the signature forbids compiler-side catching the next caller who passes a shorter buffer. Add `size_t witness_len` + LOG_FAIL on shortfall. | `lib/sapling/src/sapling_prover_c23.c:131-188` | MED | Agent 3 |
| P9.7 | `sprout_verify_groth16` computes `sprout_vk->ic_len - 1` (size_t). If `ic_len == 0` (fresh-calloc'd VK, partial load failure, defensive reset), underflows to SIZE_MAX — comparison still fails-closed, but LOG_FAIL prints `expected=18446744073709551615` which is operator-hostile. Also no locking: concurrent `sprout_set_vk(NULL)` between :49 and :80 crashes. Snapshot pointer to a local at function top + LOG_FAIL early on `ic_len < 1`. | `lib/sapling/src/sprout.c:11, 49, 80-83` | MED | Agent 3 |
| P9.8 | `pedersen_hash.c::ensure_generators` loops `for (int c = 0; c < 256; c++) { if (group_hash(...)) break; }` per Pedersen segment. If all 256 fail (cryptographically near-impossible, but a bug/bit-flip in `group_hash` would trigger it), loop exits silently and `cached_generators[i]` stays zero/stale. Pedersen hash then silently uses a zero generator, collapsing note commitments. Same class as P1.10 fixed in `sapling.c::find_group_hash` — mirror the pattern: track success, LOG_FAIL + abort on exhaustion. | `lib/sapling/src/pedersen_hash.c:33-38` | MED | Agent 3 |
| P9.9 | Prover emits `printf(...)\n` to stdout on every major step: `sapling_circuit.c:260,481` (per proof), `groth16_prover.c:604-605` (per PK load), `params_init.c:216,256,258,285` (per init). Pollutes operator log, leaks wallet-activity timing (spend-proof generation fires the printf, visible to anyone reading the log), conflicts with project-wide `LOG_INFO` / event-bus discipline. Replace with `LOG_INFO` or delete — data is already in events/metrics. | `sapling_circuit.c:260,481`; `groth16_prover.c:604-605`; `params_init.c:216,256,258,285` | MED | Agent 3 |
| P9.10 | `msm_parallel.c` workers use naïve bucket access: `g1_add(&buckets[val - 1], ...)` with `val` derived from `c` bits of a secret scalar. Every scalar-bit group hits a different cache line → cache-side-channel on the witness (notes, rcm, esk). Same concern as P9.1 but on the witness side. Switch prover-side MSM to CT bucket access (linear scan + masked select), OR document non-CT + confirm single-tenant threat model. Verifier-side MSM (public inputs + VK) is fine. | `lib/sapling/src/msm_parallel.c:60-69, 181-190`; serial at `groth16_prover.c:300-322, 362-381` | LOW | Agent 3 |

**Triage notes for Rhett (revisit AFTER P10.1 closes):**
- P9.2 is the only CRIT — land first. If the placeholder paths are
  shadowed by another prover impl, the fix is a 3-line LOG_FAIL; if
  live, it's a multi-day note-commitment rewrite.
- P9.1 + P9.10 are the two halves of the prover side-channel story.
  Consider a single commit — shared helpers and a shared Hamming-
  weight timing regression, mirror of P1.12 / P1.16b.
- P9.6, P9.7, P9.8 are small defensive-coding hygiene rows — batchable.
- P9.9 is a 10-minute commit. P9.5 pairs with any future `sapling/`
  perf work; the fix is `pthread_once`.

---

## Priority 10 — Root-cause discipline (2026-04-19 reset)

P8.9 (deployed) and P8.10 (proposed) were hotfixes against an
incompletely-understood failure. **Both are superseded by P10.1.**
P8.10 is frozen (not deleted — kept for reference) but **must not
be implemented as written**.

The work order for P10.1 is non-negotiable. Each step is a separate
commit; the next step does not start until the previous one is
reviewed.

| # | Task | Acceptance | Owner |
|---|---|---|---|
| **P10.1.1** | **Reproduce the chain stall on a fixture.** Build a deterministic test fixture: pre-seed `utxos` + `tx_index` with the post-P7.1 partial-application state observed on the live node (or a smaller equivalent that exercises the same code path). Boot a node from this fixture. Assert the BIP30 false-positive reproduces. Commit the fixture + reproduction script to `lib/test/`. | New `test_chain_stall_repro` lives in `lib/test/`; runs in `make test`; FAILS today (no fix yet). | Agent 2 — done 1243e1766 [test:1.0] (in-memory fixture at `lib/test/src/test_chain_stall_repro.c`: pre-seeds `coins_view_cache` with a stale unspent coinbase for the block being reconnected + pins `best_block` to the parent hash; calls `connect_block` and asserts `bad-txns-BIP30` at `h=tip+1` with `dos=100`. Control test confirms a clean view advances the same block. `<100ms`, default-on in make test, no SQLite or thread state) |
| **P10.1.2** | **Root-cause writeup.** Markdown doc in `docs/postmortems/2026-04-19-bip30-stall.md`. Must answer: (a) the EXACT path that took chain `3,081,408 → 3,081,407` without a reorg log line; (b) WHY BIP30 trips after P8.9's strengthened sweep ran; (c) the invariant that should have been enforced; (d) why the existing tests didn't catch it. No code in this row. | One commit: `docs/postmortems/...md` + AGENT.md row updated. Reviewed by Agent-3 before P10.1.3 starts. | Agent 2 — done 5279752d1 (`docs/postmortems/2026-04-19-bip30-stall.md`: root cause = `disconnect_block`'s `coins_map_erase` at `connect_block.c:639` doesn't propagate to the backing store because `cvc_batch_write`/`coins_view_sqlite_batch_write_ex` are DIRTY-driven. Invariant: after `disconnect_block(B) + flush`, every output in B must be absent from both the cache AND SQLite. Existing test gap: all three `disconnect_block` tests use a NULL backing view, so the missing DELETE signal is invisible. Agent-3 review pending before P10.1.3 starts) |
| **P10.1.3** | **Regression test that fails pre-fix.** Translate P10.1.2's invariant into a unit test in `lib/test/test_validation.c` (or wherever covers the affected path). Test must FAIL on the current main without the fix and PASS after the fix lands. The test name should describe the invariant ("connect_block leaves coins view consistent on retry"). | One commit: test added, `make test` shows the new test failing in `RED`. | Agent 2 |
| **P10.1.4** | **Minimal fix.** Smallest diff that makes P10.1.3's test pass without regressing any other test. No drive-by refactors. Add an assertion that panics if the invariant is ever violated again (debug builds only — release logs + bumps a metric). | One commit: code fix + assertion. `make test` green. P10.1.1 reproduction also passes (chain advances). | Agent 2 |
| **P10.1.5** | **Live-node verification.** Coordinator (Rhett, this clone) runs `make deploy` on the production node. Watches for `EV_BLOCK_CONNECTED` on h=3,081,408 within 120s. Logs the post-deploy memory plateau over the next 24h. Updates this row with `done` + the canary observations. | Live node stays advanced for 24h, RSS plateaus instead of climbing. | Rhett (coordinator) |

**Cancelled / deferred by this reset:**
- All P9.1–P9.10 (Agent-3's sapling-prover audit, `04247c19a`) are
  **deferred** — see the DEFERRED banner on Priority 9. The findings
  are real but the chain stall blocks the dev plan; sapling-prover
  hardening waits until P10.1 closes.
- All P8 MEDs (P8.4, P8.6, P8.7, P8.8) and the P7.10 follow-up are
  **deferred** until P10.1 closes. Not cancelled — parked.

**Triage notes:**
- P8.10 stays in the table for traceability. **Do not implement**
  the bandaid options listed in its row — they were guessed before
  P10.1.1 reproduction. The real fix may look completely different.
- If P10.1.2 surfaces a NEW class of bug (e.g., disconnect_tip's
  cleanup is broken in general, not just at h=3,081,408), file each
  finding as P10.2.x — one logical fix per row, same investigate-
  test-fix discipline.

---

## Status tracking

Edit the tables inline as work lands. Replace `open` with `in-progress` / `done`
and include the commit SHA. When Agent-2 or Agent-3 ships a chunk, the owning
agent updates its own row. Rhett reviews before anything depending on it lands.
