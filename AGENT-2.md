# AGENT-2 - Wallet / Storage / App-Layer / Net / Validation

**Working directory:** `~/zclassic23-2`
**Coordinator:** Rhett in `~/zclassic23`
**Sibling:** Agent-3 in `~/zclassic23-3`

This is the current executable packet. `AGENT.md` is the full plan and
row source of truth. Vibepoint kickoff is authoritative for the active
claim; use this file as lane and row context after kickoff.

## Session Start

1. `cd ~/zclassic23-2`
2. `git pull --rebase origin main`
3. `vibepoint:kickoff { "agent": 2 }`
4. Follow the required next call from kickoff, then read the matching row
   in `AGENT.md`.
5. Ship one row at a time: RED commit first, GREEN commit second.
6. Run `make test` before every commit. Run `make ci` before every push.

## Lane

**Full edit access:**
- `lib/wallet/`, `lib/storage/`, `lib/coins/`
- `lib/net/`, except Agent-3's P7.4 watchdog-only scope
- `lib/validation/`, except `sigops.c` and `check_block.c`
- `lib/script/`, `lib/rpc/`, `lib/event/`
- `lib/util/`, excluding crypto helpers
- `app/controllers/`, `app/services/`, `app/models/`, `app/views/`
- `tools/mcp/`, `tools/mcp/controllers/`
- `lib/sync/`
- `lib/test/` for tests covering your changes
- `deploy/zclassic23.service`
- Repo-root hygiene such as `.gitignore`, `Makefile`, tracked binaries

**Read-only / off-limits:**
- `lib/crypto/`, `lib/sapling/`, `lib/keys/`
- `lib/validation/src/sigops.c`, `lib/validation/src/check_block.c`
- `lib/core/src/random.c`
- `vendor/`, except where Rhett explicitly assigns a pin bump

**STOP and ping Rhett before changing:**
- Serialized block, tx, UTXO, or wallet-keystore formats
- Consensus constants
- P2P wire formats
- Row scope or acceptance criteria

## Current Queue

**NOW:** `P24.31 -> P24.32 -> P24.33 -> P24.34 -> P24.19 -> P24.20 -> P24.21 -> P24.22 -> P24.23 -> P24.24 -> P24.25 -> P24.26`

Already landed and not yours anymore:
- `P24.28`: `a940dd5ba` RED + `0af03d99e` GREEN
- `P24.29`: `ec1867c95` RED + `069f9b8bd` GREEN, with coordinator pre-land `1a56e79e7`
- `P24.30`: `10a8a8fec` RED + `b3c98d6d0` GREEN

## NOW: P24.31

**Goal:** populate `tx_index` during LDB fast-sync. P24.29 added a bounded
self-heal scan when the tx index is missing; P24.31 is the structural fix
that makes deep-history spends not depend on that fallback.

Start from the `P24.31` row in `AGENT.md`. Expected shape:
- RED: fast-sync/import fixture where a deep-history spend misses
  `tx_index` and would require scan fallback.
- GREEN: background/index-builder path after `utxo_recovery_import_ldb`
  walks block files, writes tx offsets, and records a
  `node_state["tx_index_built_through"]` watermark.
- Acceptance: after LDB import, deep-history self-heal hits `tx_index`
  directly and scan-fallback counters/events do not fire.

Keep the implementation narrow. Do not fold P24.32/P24.33/P24.34 into this
row.

## Safe Live-Node Tools

Use these for diagnostics:
- `zcl_status`
- `zcl_kpi`
- `zcl_getblockcount`
- `zcl_peers`
- `zcl_peer_report`
- `zcl_events`
- `zcl_logtail`
- `zcl_health`
- `zcl_validationstatus`

Do not use raw `zcl_rpc` against unconfirmed methods during incident work.
If a tool is not listed here, treat it as unsafe until Rhett confirms it.

## Commit Discipline

- RED commit message: `test/P24.31: RED for tx_index population during LDB fast-sync`
- GREEN commit message: `P24.31: populate tx_index during LDB fast-sync`
- One row per commit pair.
- No shell scripts, Python, or Docker.
- No `--no-verify`, no force push, no amending pushed commits.
