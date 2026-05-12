# AGENT-3 - Cryptography / Sapling / Consensus-Crypto / Net-Parallel

**Working directory:** `~/zclassic23-3`
**Coordinator:** Rhett in `~/zclassic23`
**Sibling:** Agent-2 in `~/zclassic23-2`

This is the current executable packet. `AGENT.md` is the full plan and
row source of truth. Vibepoint kickoff is authoritative for the active
claim; use this file as lane and row context after kickoff.

## Session Start

1. `cd ~/zclassic23-3`
2. `git pull --rebase origin main`
3. `vibepoint:kickoff { "agent": 3 }`
4. Follow the required next call from kickoff, then read the matching row
   in `AGENT.md`.
5. Ship one row at a time: RED commit first, GREEN commit second.
6. Run `make test` before every commit. Run `make ci` before every push.

## Lane

**Full edit access:**
- `lib/crypto/`, `lib/sapling/`, `lib/keys/`
- `lib/core/src/random.c`
- `lib/validation/src/sigops.c`
- `lib/validation/src/check_block.c`
- `vendor/tor` submodule pin bumps only
- `lib/net/src/msgprocessor.c` and `lib/net/src/download.c` only for
  the P7.4 backpressure-watchdog scope
- `lib/test/` for tests covering your changes

**Read-only / off-limits:**
- `lib/wallet/`, `lib/storage/`, `lib/coins/`, `app/*`, `tools/mcp/`
- `lib/rpc/`, `lib/script/`
- Other `lib/validation/` files
- `lib/core/` beyond `random.c`
- Other `vendor/` directories

**STOP and ping Rhett before changing:**
- Serialized block or tx formats
- Consensus constants
- P2P wire formats
- Row scope or acceptance criteria

## Current Queue

**NOW:** `P24.27 -> P20.10 -> P15.4 -> P15.5 -> P17 -> P18.4 -> P21.7/P21.8 -> P22.4 -> P23.7 -> P24.2/P24.4`

Already landed and not yours anymore:
- `P11.8` parity-diff CI gate
- `P20.11` `zcl_kickoff`
- `P20.12` `zcl_kickoff` enrichment

Ignore old instructions that say to restore P11.8 WIP, work P20.12, or
follow unlisted external tasks. Those rows are stale for this repo unless
Rhett adds matching rows back to `AGENT.md`.

## NOW: P24.27

**Goal:** add an observability lint gate so failure-path
`fprintf(stderr, ...)` calls in `lib/` and `app/` cannot stay silent.
The 2026-04-22 stall had a stderr-only warning that did not emit an event;
this row prevents that class from recurring.

Start from the `P24.27` row in `AGENT.md`, with this coordinator override:
**do not add a new shell script.** The repo rule is no shell scripts, no
Python, no Docker. Implement the gate in C23 or existing compiled tooling
and wire it through the current lint/test path.

Expected shape:
- RED: a self-test fixture with a bad `fprintf(stderr, ...)` failure path
  that lacks `event_emit`, a terminal return/exit/abort, or an
  `obs-ok:<reason>` marker.
- GREEN: compiled lint/check code that scans `lib/` and `app/`, reports
  file/line/context, and passes only when each stderr site is observable
  or explicitly justified.
- Acceptance: `make lint` and `make test` pass; new stderr failure paths
  without observability fail the gate.

Keep the implementation narrow. Do not refactor unrelated crypto, Sapling,
or validation logic while annotating existing stderr sites.

## Safe Live-Node Tools

Use these for diagnostics:
- `zcl_status`
- `zcl_kpi`
- `zcl_getblockcount`
- `zcl_events`
- `zcl_logtail`
- `zcl_health`
- `zcl_validationstatus`

Do not use raw `zcl_rpc` against unconfirmed methods during incident work.
If a tool is not listed here, treat it as unsafe until Rhett confirms it.

## Commit Discipline

- RED commit message: `test/P24.27: RED for observability lint gate`
- GREEN commit message: `P24.27: require observability for stderr failure paths`
- One row per commit pair.
- No shell scripts, Python, or Docker.
- No `--no-verify`, no force push, no amending pushed commits.
