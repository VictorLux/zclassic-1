# zclassic23 — Master Agent Checklist

Derived from the full code review of 2026-04-17. Supersedes earlier wave plans.

Owner: Rhett (primary). Delegates: Agent-2 (see `AGENT-2.md`), Agent-3 (see `AGENT-3.md`).

---

## Progress — last update 2026-04-18

**Overall: 37 / 53 rows closed (70%) | SWRC 70%**

(Denominator grew from 50 to 53 when Agent-3's Wave 2 opened three new
rows — P1.13 curve25519 CT, P1.14 ed25519 CT, P1.15 RNG hygiene — and
closed all three in the same push.)

| Tier | Closed / Total | % | Open rows |
|---|---|---|---|
| **CRITICAL** | 7 / 9 | **78%** | P2.1, P2.2 |
| **HIGH** | 13 / 22 | **59%** | P1.6, P1.7, P2.3–P2.6, P4.1, P4.2, P5.2 |
| **MED** | 11 / 16 | **69%** | P2.7, P2.8, P3.7, P5.5, P5.6 |
| **LOW** | 2 / 2 | **100%** | — |
| (P0 baseline) | 4 / 4 | **100%** | — |

**Open by owner (after 2026-04-18 reassignment):** Rhett 12 · Agent-2 3 (P2.3 + P2.8 + P3.7; parallel-test-runner infra shipped df5de36c4) · Agent-3 2 (root-cause `lib/core/random.c` fail-open they flagged + prf.c nullifier-path timing audit)

**Top remaining risks:** the two open CRITs are both in the network lane (P2.1 mempool tx accept, P2.2 stack overflow in msg handler). Chain-split risk on deploy is essentially eliminated; DoS-on-deploy is now the headline. P1.6 + P1.7 are the last two HIGHs in the consensus tier and block Rhett.

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
| P2.3 | fast_sync bypasses AR_BEGIN_SAVE | `lib/net/src/fast_sync.c:480-526` | HIGH | Agent 2 — queued (narrow scope: same migration pattern as P3.3) |
| P2.4 | Swarm per-chunk hash verification effectively absent | `lib/net/src/fast_sync.c:892-895`, `msgprocessor.c:1968` | HIGH | Rhett |
| P2.5 | connman deadlock risk: `cs_nodes` held across callback | `lib/net/src/connman.c:802-836` | HIGH | Rhett (mutex discipline — careful) |
| P2.6 | `g_swarm_active` TOCTOU → state leak | `lib/net/src/msgprocessor.c:1961-1981, 2040` | HIGH | Rhett |
| P2.7 | FlyClient challenge amplification — no rate limit | `lib/net/src/msgprocessor.c:1864-1900` | MED | Rhett |
| P2.8 | No global byte budget on recv queue | `lib/net/src/net.c:104-115` | MED | Agent 2 — queued (narrow scope: lib/net/src/net.c only) |

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
| P3.7 | `/metrics` open on TLS listener with no auth | `lib/rpc/src/httpserver.c:355-381` | MED | Agent 2 — queued (narrow scope: lib/rpc/src/httpserver.c only) |

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
| P5.2 | `deploy/zclassic23.service:21` hardcodes Rhett's externalip + 9 addnodes | HIGH | Rhett |
| P5.3 | Hardcoded `/home/rhett` in `tools/export_snapshot.c:15`, `tools/zcl-nodectl.c:628-637` | HIGH | Agent 2 — done 09e4fb15a (shared $HOME helper in lib/util/include/util/rpc_paths.h; also swept test_phgr13_fix.c sprout-VK path + two README absolute-path links; new test_no_hardcoded_home regression test scans every deployed binary for the literal and exercises the helper with alt/NULL HOME) |
| P5.4 | 10 shell scripts in `tools/` duplicating MCP — purge | MED | Agent 2 — done 0f33d3fc1 (audit found 1/8 actual MCP-duplicates: verify_restart_follow.sh ⇒ zcl-nodectl verify-follow; the other seven are build-time or multi-node orchestration with no MCP equivalent — per-script rationale in "Notes from Agent-2" in AGENT-2.md) |
| P5.5 | `vendor/tor` submodule ahead of pinned commit | MED | Rhett |
| P5.6 | Vendored `sqlite3.h` is 3.49.0 — newer CVE-class fixes unpicked | MED | Rhett |
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

## Status tracking

Edit the tables inline as work lands. Replace `open` with `in-progress` / `done`
and include the commit SHA. When Agent-2 or Agent-3 ships a chunk, the owning
agent updates its own row. Rhett reviews before anything depending on it lands.
