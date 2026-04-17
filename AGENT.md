# zclassic23 — Master Agent Checklist

Derived from the full code review of 2026-04-17. Supersedes earlier wave plans.

Owner: Rhett (primary). Delegates: Agent-2 (see `AGENT-2.md`), Agent-3 (see `AGENT-3.md`).

**Ground rules for every agent**
- zclassic23 is the next-gen product. zclassicd is a legacy bootstrap peer only.
- Read `CLAUDE.md` and `DEFENSIVE_CODING.md` before touching code.
- Everything is in the single binary — no standalone shell scripts, no Docker.
- `make test` MUST pass before any commit. `make ci` MUST pass before push.
- Small commits. Push frequently. No amending pushed commits.
- Do NOT touch files outside your assigned scope — conflicts cost time.

---

## Priority 0 — Build enforcement (BLOCKS everything else)

The single highest-leverage fix: everything downstream depends on lint actually
failing the build. Owner: Rhett.

| # | Task | Files | Status |
|---|---|---|---|
| P0.1 | Flip `check-raw-sqlite` from warn/exit-0 to fail/exit-1 | `Makefile:506-514` | open |
| P0.2 | Add `-DZCL_AR_ENFORCE` to `CFLAGS` | `Makefile:~37` | open |
| P0.3 | Wire `tools/scripts/check_no_secret_printf.sh` into `make lint` | `Makefile:~543` | open |
| P0.4 | Make `deploy` target depend on `ci` | `Makefile:298-302` | open |

Doing P0 will surface a flood of existing violations — that's the point. Agent 2
and Agent 3 fix violations in their lanes; Rhett fixes the rest.

---

## Priority 1 — Money-loss / consensus-split

| # | Task | File:line | Severity | Owner |
|---|---|---|---|---|
| P1.1 | Wallet wrapper silent-error (`return true` after `LOG_FAIL`) | `lib/wallet/src/wallet_sqlite.c:259,439,571,600,661,703,759,835,938,984,1103` | CRITICAL | Agent 2 — done 8608820e7 |
| P1.2 | Flush commits partial state (rc ignored) | `lib/wallet/src/wallet_sqlite.c:1054-1072` | CRITICAL | Agent 2 — done 8608820e7 |
| P1.3 | Sapling verify fail-open on NULL VK | `lib/sapling/src/sapling.c:505, 559` | CRITICAL | Agent 3 — done 3b4b08ba9 (on a3/build-ci-deploy-hardening; not yet in main) |
| P1.4 | Sapling params loaded without integrity check | `lib/sapling/src/params_init.c:47-167` | CRITICAL | Agent 3 — done 785db18b1 (branch, not main) |
| P1.5 | Raw `sqlite3_step` in UTXO batch writer | `lib/storage/src/coins_view_sqlite.c:461,474,509,557` | CRITICAL | Agent 2 — done 152603fdc |
| P1.6 | No P2SH sigop accounting — consensus split risk | `lib/validation/src/sigops.c:10-18` | HIGH | Rhett |
| P1.7 | `skip_diffbits` silently skips difficulty check | `lib/validation/src/check_block.c:222,233-250` | HIGH | Rhett |
| P1.8 | Ed25519 missing `S<L` canonicality | `lib/crypto/src/ed25519.c:300-355` | HIGH | Agent 3 — done c510c7335 (branch, not main) |
| P1.9 | RedJubjub missing `S<r` canonicality | `lib/sapling/src/sapling.c:386` | HIGH | Agent 3 — done 8440cd864 (branch, not main) |
| P1.10 | `find_group_hash` returns ignored → silent zero generators | `lib/sapling/src/sapling.c:81-110` | HIGH | Agent 3 — done e221e0212 (branch, not main) |
| P1.11 | Zero `LOG_FAIL` usage across crypto/sapling | `lib/crypto/*`, `lib/sapling/*` | HIGH | Agent 3 — in-progress (started 6b98134a0 chacha20poly1305) |

---

## Priority 2 — P2P attack surface (Owner: Rhett)

| # | Task | File:line | Severity |
|---|---|---|---|
| P2.1 | Mempool accepts any peer tx — no sig/UTXO/fee check | `lib/net/src/msg_tx.c:34-69` | CRITICAL |
| P2.2 | 1.6 MB stack alloc in message handler | `lib/net/src/msg_tx.c:288` | CRITICAL |
| P2.3 | fast_sync bypasses AR_BEGIN_SAVE | `lib/net/src/fast_sync.c:480-526` | HIGH |
| P2.4 | Swarm per-chunk hash verification effectively absent | `lib/net/src/fast_sync.c:892-895`, `msgprocessor.c:1968` | HIGH |
| P2.5 | connman deadlock risk: `cs_nodes` held across callback | `lib/net/src/connman.c:802-836` | HIGH |
| P2.6 | `g_swarm_active` TOCTOU → state leak | `lib/net/src/msgprocessor.c:1961-1981, 2040` | HIGH |
| P2.7 | FlyClient challenge amplification — no rate limit | `lib/net/src/msgprocessor.c:1864-1900` | MED |
| P2.8 | No global byte budget on recv queue | `lib/net/src/net.c:104-115` | MED |

---

## Priority 3 — MCP / application layer

| # | Task | File:line | Severity | Owner |
|---|---|---|---|---|
| P3.1 | MCP `zcl_send` JSON injection via `from`/`to` | `tools/mcp/controllers/wallet_controller.c:53-55` | CRITICAL | Rhett |
| P3.2 | MCP `zcl_sendtoaddress` JSON injection via `address` | `tools/mcp/controllers/wallet_controller.c:76-77` | CRITICAL | Rhett |
| P3.3 | ~80 raw `sqlite3_step` in controllers and services | `app/controllers/*`, `app/services/*` | HIGH | Agent 2 — next (reassigned after P6 completion) |
| P3.4 | `store_controller` accepts addresses without checksum | `app/controllers/src/store_controller.c:663-685` | HIGH | Rhett |
| P3.5 | `rpc_client.c` realloc overwrite w/ no NULL check | `tools/mcp/rpc_client.c:126` | HIGH | Rhett |
| P3.6 | `parse_form_field` does not URL-decode; no CSRF token | `app/controllers/src/store_controller.c:803-823` | MED | Rhett |
| P3.7 | `/metrics` open on TLS listener with no auth | `lib/rpc/src/httpserver.c:355-381` | MED | Rhett |

---

## Priority 4 — Script / consensus memory safety (Owner: Rhett)

| # | Task | File:line | Severity |
|---|---|---|---|
| P4.1 | 520 KB `script_stack` passed by value, on-stack | `lib/script/include/script/interpreter.h:22-30`, `interpreter.c:619-652` | HIGH |
| P4.2 | Silent `stack_push` failures corrupt later stack assumptions | `lib/script/src/interpreter.c:619-620` | HIGH |
| P4.3 | `script_num_serialize` lacks outsize bounds check | `lib/script/include/script/script.h:239-258` | MED |
| P4.4 | `disconnect_block` unbounded realloc on `vin.prevout.n` | `lib/validation/src/connect_block.c:586-607` | MED |
| P4.5 | `sigencoding` strict-DER bound inconsistency vs Bitcoin | `lib/script/src/sigencoding.c:11-56` | MED |

---

## Priority 5 — Operator / deploy hygiene (Owner: Rhett)

| # | Task | Severity |
|---|---|---|
| P5.1 | `export_snapshot` (1.1 MB ELF) tracked in git despite `.gitignore` | HIGH |
| P5.2 | `deploy/zclassic23.service:21` hardcodes Rhett's externalip + 9 addnodes | HIGH |
| P5.3 | Hardcoded `/home/rhett` in `tools/export_snapshot.c:15`, `tools/zcl-nodectl.c:628-637` | HIGH |
| P5.4 | 10 shell scripts in `tools/` duplicating MCP — purge | MED |
| P5.5 | `vendor/tor` submodule ahead of pinned commit | MED |
| P5.6 | Vendored `sqlite3.h` is 3.49.0 — newer CVE-class fixes unpicked | MED |
| P5.7 | Repo-root clutter: 40+ .md, `node.db` untracked at repo root | LOW |

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
