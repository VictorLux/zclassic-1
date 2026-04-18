# zclassic23 — Master Agent Checklist

Derived from the full code review of 2026-04-17. Supersedes earlier wave plans.

Owner: Rhett (primary). Delegates: Agent-2 (see `AGENT-2.md`), Agent-3 (see `AGENT-3.md`).

---

## Progress — last update 2026-04-19 (night, P8 wave opened by Agent-3 — 8 new rows)

**Overall: 59 / 72 rows closed (82%) | SWRC ~88%**

| Tier | Closed / Total | % | Open rows |
|---|---|---|---|
| **CRITICAL** | 10 / 11 | **91%** | P8.1 |
| **HIGH** | 25 / 29 | **86%** | P4.1, P4.2, P7.9, P8.2, P8.3 |
| **MED** | 20 / 26 | **77%** | P7.10, P8.4, P8.5, P8.6, P8.7, P8.8 |
| **LOW** | 2 / 2 | **100%** | — |
| (P0 baseline) | 4 / 4 | **100%** | — |

**Open by owner (2026-04-19 night, after P8 wave opened):**
- **Agent-2 (3 logical tasks + 7 P8 rows):** P4.1+P4.2 paired (NOW), P7.9+P7.10 paired (NEXT), then P8.1 (CRIT — wire-format heap overflow on `lib/net/src/zmsg.c`) takes priority over the rest of the P8 queue. P2.1 closed (d8c5442d1).
- **Agent-3 (1 row, then queue empty):** P8.3 (HIGH — MMB unbounded mountain height on deserialize, `lib/chain/src/mmb.c`) — adjacent to P5.5/FlyClient lane. P1.6 (f6aa0b080), P1.7 (5ce252bb6), P1.16b (c841defd2), P5.5 (75576d7a0), P7.4 (f6474c77b), and the P8 audit pass itself all closed.
- **Rhett:** 0 (coordinator only). Action items pending:
  1. `make deploy` from `~/zclassic23` to push Agent-3's consensus fixes (P1.6 P2SH sigops, P1.7 strict difficulty, P5.5 tor pin) + Agent-2's P2.1 mempool validation onto the production node + verify `zcl_onion_status` bootstraps within 60s for the P5.5 smoke test.
  2. Decide whether MAX_P2SH_SIGOPS=15 per-input should land as a mempool-policy rule (Agent-2 lane) — see AGENT-3.md 2026-04-19 P1.6 note for context.

**ACTION ITEM (Rhett):** the P7.1 fix landed as `a6bedccad` but production is still at h=3,081,411 because nothing triggered a rebuild + redeploy. Run `make deploy` on the production box to push the fix live. Expected result: chain advances past 3,081,411 within 60s, catches up to legacy zclassicd (currently ~3,082,462) within 2 minutes.

**Top remaining risks:** all P-tier CRIT rows are closed. The remaining queue is Agent-2 hardening work — script-interpreter stack refactor (P4.1+P4.2), tip-stuck backpressure watchdog (P7.4), thread-registry + shutdown audit (P7.9+P7.10). Agent-3 queue is empty.

**SWRC formula:** CRIT=4, HIGH=2, MED=1, LOW=0.5. P0 rows weighted as HIGH. Total weighted capacity = 135 (was 122 pre-P8; +4 CRIT P8.1, +4 HIGH P8.2/P8.3, +5 MED P8.4–P8.8 = +13).

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
| P4.1 | 520 KB `script_stack` passed by value, on-stack | `lib/script/include/script/interpreter.h:22-30`, `interpreter.c:619-652` | HIGH | Agent 2 — NEXT+2 (narrow scope: lib/script/ only; pointer-conversion refactor + P4.2 together in one commit) |
| P4.2 | Silent `stack_push` failures corrupt later stack assumptions | `lib/script/src/interpreter.c:619-620` | HIGH | Agent 2 — NEXT+2 (paired with P4.1 — same commit) |
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
| P7.9 | No central thread registry — 12+ `pthread_create` sites, each shutdown signals its own flag; no single function joins them all | `lib/util/src/sync.c` (or new `lib/util/src/thread_registry.c`), all spawn sites | HIGH | Agent 2 — NEXT+4 (narrow scope: new lib/util/src/thread_registry.c + migrate all pthread_create sites; paired with P7.10 in one commit) |
| P7.10 | `g_shutdown_requested` checked in only 6 files — `bg_validation`, `header_sync`, `peer_strategy`, `scheduler`, `workpool` either don't check it or use a different flag | cross-cutting (audit) | MED | Agent 2 — NEXT+4 (paired with P7.9 — same commit; every spawn site must register with the new thread_registry and check its shutdown flag) |

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
| P8.1 | `zmsg_deserialize` reads peer-controlled `uint8_t slen`/`rlen` (range 0-255) and copies that many bytes into `msg->sender[ZMSG_MAX_ADDR=128]` / `msg->recipient[128]` with no bounds check, then writes `'\0'` at `sender[slen]`. A peer sending `slen=255` corrupts ~127 bytes past the field into the adjacent `recipient`/`body` fields of the heap/stack-resident `struct zmsg_message`. Serialize side caps at 127; deserialize does not. Reachable from any handshake-complete peer that sends a `zmsg` P2P message — no auth, no rate limit on the parse layer. | `lib/net/src/zmsg.c:50-60` (struct in `lib/net/include/net/zmsg.h:43-53`) | CRITICAL | Agent 2 |
| P8.2 | Dandelion stem-peer PRNG seeded with `(uint64_t)time(NULL) ^ 0xdeadbeefcafe1234ULL` — ~31 bits of effective entropy. The same seed drives the Fisher-Yates shuffle that selects this epoch's stem peers AND the per-tx fluff coin-flip. An attacker with rough boot-time + epoch-rotation timing can replay the xorshift64 state and predict (a) which 2 outbound peers the node uses for stem relay this 10-min epoch, and (b) the stem/fluff outcome of every transaction the node originates — defeating Dandelion's origin-privacy property. Note: callers all hold `ds->cs` so the data race the original review flagged is NOT real; the seed-quality issue is the actual bug. | `lib/net/src/dandelion.c:42-54` | HIGH | Agent 2 |
| P8.3 | `mmb_deserialize` reads each mountain's `height` as a raw little-endian `uint32_t` from the input buffer with no upper-bound check (only `nm` is capped against `MMB_MAX_MOUNTAINS`). For any practical chain, `height` must be `≤ ⌈log2(num_leaves)⌉ ≤ 64`, but the code happily accepts `UINT32_MAX`. Downstream `mmb_merge_after_insert` (lines 100-115) increments `height` during merges — a deserialized state with `height` near `UINT32_MAX` triggers signed/unsigned wraparound on the next `mmb_append`, silently corrupting the FlyClient/snapshot trust root. Snapshot input may transit fast-sync/swarm before P2.4's hash check binds — defense-in-depth gap. | `lib/chain/src/mmb.c:254-261` | HIGH | Agent 3 |
| P8.4 | Compact-block reconstruction does an O(slots × mempool) linear scan: for every unfilled slot, the reconstructor recomputes `compact_block_short_txid()` (siphash) over **every** mempool entry until it finds a 6-byte match. With a 5 000-tx block and a 25 000-tx mempool that is 125 M siphash + memcmp ops per inbound compact block. A peer can amplify by sending compact blocks whose short-txids are placed late in the mempool iteration order. Code already comments `Simple O(n*m) for now; sufficient for typical mempools.` — promote to a `khash` short-txid table built once before the slot loop. | `lib/net/src/compact_blocks.c:272-319` | MED | Agent 2 |
| P8.5 | `bloom_filter_init_internal` only applies the `MAX_BLOOM_HASH_FUNCS` cap when `constrained=true` (the public `bloom_filter_init` path). The internal `rolling_bloom_init` path passes `constrained=false` and lets `num_hash_funcs = (data_size * 8 / num_elements * LN2)` grow without ceiling. Every subsequent `rolling_bloom_insert` / `contains` runs that many siphash iterations per call. Pathological tuning (small `num_elements`, large `data_size` from a tight `fp_rate`) produces hot-path CPU blow-up. Extract the `MIN(ideal, MAX_BLOOM_HASH_FUNCS)` clamp into both branches. | `lib/bloom/src/bloom.c:47-52` | MED | Agent 2 |
| P8.6 | `zslp_service_validate_token_key` returns true for `is_alphanumeric(token_key, len)` with `len ∈ [1, ZSLP_MAX_TOKEN_KEY_LEN]` **OR** the 64-char hex case. Because the alphanumeric branch has no length floor that distinguishes ticker-like names from truncated txids, a 10-char alphanumeric "txid" prefix is accepted and later canonicalized via `snprintf` — different short/long inputs may collide on the same canonical form. RPC callers can then look up tokens by ambiguous short keys; SEND requests targeting an aliased key reach the wrong token's payment service. | `app/services/src/zslp_service.c:62-72` | MED | Agent 2 |
| P8.7 | `zmarket_offer` computes `offer.num_chunks = (uint32_t)((size_bytes + FILE_MARKET_CHUNK_SIZE - 1) / FILE_MARKET_CHUNK_SIZE)` with no upper bound on `size_bytes`. `st.st_size` from `stat(2)` is operator-supplied (RPC argument is a path on the local filesystem), so a sparse file or a deliberately crafted manifest produces `size_bytes` near `UINT64_MAX`. The `+ FILE_MARKET_CHUNK_SIZE - 1` then wraps to a tiny value, making `num_chunks` underestimate the true chunk count. Downstream chunk-challenge / payment math operates on a wrong chunk count without ever logging the mismatch. Lower priority because the input is operator-controlled, but worth a one-line `if (size_bytes > UINT64_MAX - CHUNK_SIZE) reject` guard. | `app/controllers/src/file_market_controller.c:140-142` | MED | Agent 2 |
| P8.8 | ZNAM builder gates diverge from the parser. `znam_build_register` and `znam_build_update` reject `target_type > 3` (literal — covers only ONION/ZADDR/TADDR), but `znam_parse` and `znam_build_set_record` accept up to `ZNAM_TYPE_CONTENT = 7` (BTC/LTC/DOGE/CONTENT). A user calling REGISTER with a BTC address gets a silent zero-bytes-written return and no logged reason. Either lift the literal-3 cap to `ZNAM_TYPE_CONTENT` (intentional design — REGISTER should support multi-coin per the ENS-inspired spec) or document the restriction with a `LOG_FAIL`. | `lib/znam/src/znam.c:189-205` (vs parser at `:125`) | MED | Agent 2 |

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

## Status tracking

Edit the tables inline as work lands. Replace `open` with `in-progress` / `done`
and include the commit SHA. When Agent-2 or Agent-3 ships a chunk, the owning
agent updates its own row. Rhett reviews before anything depending on it lands.
