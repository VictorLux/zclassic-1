# zclassic23 — Master Agent Checklist

Derived from the full code review of 2026-04-17. Supersedes earlier wave plans.

Owner: Rhett (primary). Delegates: Agent-2 (see `AGENT-2.md`), Agent-3 (see `AGENT-3.md`).

---

## Progress — last update 2026-04-18 (late-evening, P5.6 landed + P7 wave opened)

**Overall: 46 / 63 rows closed (73%) | SWRC ~74%**

(Denominator grew 53 → 63 when the P7 fresh-review wave opened ten new
rows from a live-node inspection that surfaced a tip-stuck outage and
several latent operability gaps. See P7 section below.)

| Tier | Closed / Total | % | Open rows |
|---|---|---|---|
| **CRITICAL** | 7 / 10 | **70%** | P2.1, P2.2, **P7.1 (live outage)** |
| **HIGH** | 18 / 26 | **69%** | P1.6, P1.7, P1.16, P4.1, P4.2, **P7.2, P7.3, P7.4, P7.9** |
| **MED** | 15 / 21 | **71%** | P5.5, **P7.5, P7.6, P7.7, P7.8, P7.10** |
| **LOW** | 2 / 2 | **100%** | — |
| (P0 baseline) | 4 / 4 | **100%** | — |

**Open by owner (late-evening 2026-04-18 after P7 wave opened):** Rhett 11 (P1.6, P1.7, P2.1, P2.2, P4.1, P4.2, P5.5, **P7.1, P7.4, P7.9, P7.10**) · Agent-2 5 (**P7.2 boot tip-mismatch halt** + **P7.3 crash handler flush** + **P7.5/P7.6/P7.7 deploy-unit hygiene batch**, with **P7.8 SQLite tuning** queued NEXT) · Agent-3 1 (P1.16 in flight; prf.c nullifier still queued NEXT)

**Top remaining risks:** P7.1 (tip stuck at h=3,081,601 on the live node) is the new headline — the chain is dead in the water until it's fixed. P2.1 + P2.2 net CRITs remain. The original 53-row review is 87% closed by SWRC; the new P7 wave drops aggregate SWRC because it adds 1 CRIT + 4 HIGH + 5 MED of fresh weight.

**SWRC formula:** CRIT=4, HIGH=2, MED=1, LOW=0.5. P0 rows weighted as HIGH. Total weighted capacity now 105 + 4+8+5 = 122. Update this block every time a row closes.

**SWRC formula:** CRIT=4, HIGH=2, MED=1, LOW=0.5. P0 rows weighted as HIGH. Total weighted capacity = 105. Update this block every time a row closes.

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
| P1.6 | No P2SH sigop accounting — consensus split risk | `lib/validation/src/sigops.c:10-18` | HIGH | Rhett |
| P1.7 | `skip_diffbits` silently skips difficulty check | `lib/validation/src/check_block.c:222,233-250` | HIGH | Rhett |
| P1.8 | Ed25519 missing `S<L` canonicality | `lib/crypto/src/ed25519.c:300-355` | HIGH | Agent 3 — done c510c7335 (merged bcab984fd) |
| P1.9 | RedJubjub missing `S<r` canonicality | `lib/sapling/src/sapling.c:386` | HIGH | Agent 3 — done 8440cd864 (merged bcab984fd) |
| P1.10 | `find_group_hash` returns ignored → silent zero generators | `lib/sapling/src/sapling.c:81-110` | HIGH | Agent 3 — done e221e0212 (merged bcab984fd) |
| P1.11 | Zero `LOG_FAIL` usage across crypto/sapling | `lib/crypto/*`, `lib/sapling/*` | HIGH | Agent 3 — done ca139a5ad (full audit across lib/crypto/ + lib/sapling/; algorithmic/retry-signal returns in fr.c documented rather than logged — see fr.c header) |
| P1.11b | Note-encryption esk nonce-reuse sanity guard (Step 8 of A3 brief) | `lib/sapling/src/note_encryption.c` | MED | Agent 3 — done 909636215 |
| P1.12 | `jub_scalar_mul` constant-time rewrite (side-channel on secret keys) | `lib/sapling/src/fr.c:307-333` | HIGH | Agent 3 — done 15218ba2f (masked linear-scan table select + unconditional-add-with-mask; diff test + timing test in test_sapling_crypto.c) |
| P1.13 | curve25519 constant-time audit (X25519 DH, esk in note encryption) | `lib/crypto/src/curve25519.c` | HIGH | Agent 3 — done da3dbbccb (audit comment in source documenting CT properties of the TweetNaCl ladder; Hamming-weight regression timing test in test_sapling.c — Wave 2 / Step H) |
| P1.14 | ed25519 constant-time pass (verify-only path, JoinSplit consensus) | `lib/crypto/src/ed25519.c` | MED | Agent 3 — done b63b149c9 (audit comment confirms verify-only file uses cswap + XOR-OR diff check; no `ed25519_sign` exists, sign-side guidance documented for future work — Wave 2 / Step I) |
| P1.15 | RNG hygiene wrapper + secret-generation call-site sweep | `lib/crypto/`, `lib/sapling/` | HIGH | Agent 3 — done 7abe359c5 (new `zcl_random_secret_bytes` wrapper rejects `GetRandBytes` all-zero fail-open; migrated esk/groth16-blind/redjubjub-T/sapling-r/sha3-nonce; lib/core/random.c root-cause flagged) |
| P1.16 | `lib/core/random.c` GetRandBytes root-cause fail-open (flagged during P1.15) | `lib/core/src/random.c` | HIGH | Agent 3 — next (narrow scope: lib/core/src/random.c only) |

---

## Priority 2 — P2P attack surface

| # | Task | File:line | Severity | Owner |
|---|---|---|---|---|
| P2.1 | Mempool accepts any peer tx — no sig/UTXO/fee check | `lib/net/src/msg_tx.c:34-69` | CRITICAL | Rhett |
| P2.2 | 1.6 MB stack alloc in message handler | `lib/net/src/msg_tx.c:288` | CRITICAL | Rhett |
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
| P4.1 | 520 KB `script_stack` passed by value, on-stack | `lib/script/include/script/interpreter.h:22-30`, `interpreter.c:619-652` | HIGH | Rhett (needs careful interpreter refactor) |
| P4.2 | Silent `stack_push` failures corrupt later stack assumptions | `lib/script/src/interpreter.c:619-620` | HIGH | Rhett (tied to P4.1) |
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
| P5.5 | `vendor/tor` submodule ahead of pinned commit | MED | Rhett |
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
| P7.1 | Tip stuck at 3,081,601 — `val.block_connected` repeats for same height; `chain_height` never advances | `lib/validation/src/process_block.c`, `connect_block.c` | CRITICAL | Rhett — NOW (live outage) |
| P7.2 | Boot logs `DB_ERR_TIP_MISMATCH ... halt and investigate` then keeps running — must be fatal or auto-rewind | `app/services/src/chain_state_repository.c` (audit + tighten the halt path) | HIGH | Agent 2 — NOW (narrow scope: chain_state_repository.c + new test) |
| P7.3 | Crash handler runs but FATAL header + `backtrace_symbols_fd` output never reaches `node.log` (only `sys.crash` event survives) | `lib/event/src/event.c:610-630` | HIGH | Agent 2 — NOW (narrow scope: lib/event/src/event.c only — small isolated fix) |
| P7.4 | Backpressure missing under tip-stuck loop — RSS climbs to 6.0G (cgroup MemoryHigh) accumulating block buffers when tip doesn't advance | `lib/net/src/msgprocessor.c`, `download.c` | HIGH | Rhett |
| P7.5 | `deploy/zclassic23.service:34` `TimeoutStopSec=300` amplifies hangs into 5-min outages; trim to 60-90s + watchdog | `deploy/zclassic23.service` | MED | Agent 2 — NOW (small batch with N6+N7) |
| P7.6 | `StartLimitBurst=3 / StartLimitIntervalSec=300` can permanently disable service after 3 crashes in 5min | `deploy/zclassic23.service` | MED | Agent 2 — NOW (with P7.5) |
| P7.7 | `LimitCORE=` not set — first SIGABRT today produced no usable post-mortem | `deploy/zclassic23.service` | MED | Agent 2 — NOW (with P7.5) |
| P7.8 | SQLite default `cache_size` (~2 MB) and `mmap_size` (0) on a 1.3M-row chainstate; `boot_index.c:307` warns mmap_size=64MB previously caused SIGSEGV — pick safe values + lock with a test | `lib/storage/src/coins_view_sqlite.c:187`, schema_migration.c | MED | Agent 2 — NEXT |
| P7.9 | No central thread registry — 12+ `pthread_create` sites, each shutdown signals its own flag; no single function joins them all | `lib/util/src/sync.c` (or new `lib/util/src/thread_registry.c`), all spawn sites | HIGH | Rhett |
| P7.10 | `g_shutdown_requested` checked in only 6 files — `bg_validation`, `header_sync`, `peer_strategy`, `scheduler`, `workpool` either don't check it or use a different flag | cross-cutting (audit) | MED | Rhett (companion to P7.9) |

---

## Status tracking

Edit the tables inline as work lands. Replace `open` with `in-progress` / `done`
and include the commit SHA. When Agent-2 or Agent-3 ships a chunk, the owning
agent updates its own row. Rhett reviews before anything depending on it lands.
