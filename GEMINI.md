# zclassic23 — Gemini CLI onboarding

You are a **worker agent** in the zclassic23 multi-agent coordination
workflow. Rhett (human) is the coordinator and runs Claude Code from
`~/zclassic23`. You are invoked from one of two sibling worktrees;
which one tells you which lane you own.

## First step, every session — figure out which agent you are

```bash
pwd                            # → /home/rhett/zclassic23-2 OR -3
git pull --rebase              # always sync to origin/main before starting
```

| Your `pwd` | You are | Read this file next |
|---|---|---|
| `~/zclassic23-2` | **Agent-2** — wallet/storage/net/validation lane | [`AGENT-2.md`](AGENT-2.md) |
| `~/zclassic23-3` | **Agent-3** — crypto/sapling/keys lane | [`AGENT-3.md`](AGENT-3.md) |
| `~/zclassic23`  | (Coordinator's clone — Rhett only; you should never run here) | — |

Your executable checklist — the row you're working on NOW, what comes
next, and your lane boundaries — lives in your per-agent file. Read it
every session before editing.

## Shared orientation (both agents read these)

| File | Purpose |
|---|---|
| [`AGENTS.md`](AGENTS.md) | 60-line portable orientation index |
| [`CLAUDE.md`](CLAUDE.md) | Project overview, MCP tools, architecture |
| [`AGENT.md`](AGENT.md) | Master plan — all rows, tier table, coordinator state |
| [`DEFENSIVE_CODING.md`](DEFENSIVE_CODING.md) | Mandatory coding standards enforced by `make lint` |
| [`ATTRIBUTIONS.md`](ATTRIBUTIONS.md) | Concept-borrow attributions (cite in commits) |
| [`LICENSE`](LICENSE) + [`NOTICE`](NOTICE) | Apache-2.0 + upstream copyright preservation |

## Non-negotiable rules (all agents)

1. **zclassic23 is the next-gen product. zclassicd is a legacy bootstrap peer only.** Never conflate them.
2. **No shell scripts (`.sh`). No Python.** Build every tool into the `zclassic23` binary (C23) and expose via MCP. This matches your `~/.gemini/GEMINI.md` global memory.
3. **No Docker.** Nodes run as systemd user linger-services.
4. **RED-first test discipline.** Code-touching rows land as two commits:
   - RED commit: a failing test that reproduces the bug or proves the missing behavior. Push this first.
   - GREEN commit: the real fix. Test must now pass.
   No hotfixes. No combined commits. No `--no-verify`.
5. **One row per commit.** Include the row ID (e.g. `P24.13`) in the commit message.
6. **`make test` MUST pass before every commit. `make ci` before every push.**
7. **Update `AGENT.md` + your per-agent file when a row lands.** Mark `done <SHA> [test:X.X]`, then advance your own NOW line.
8. **Never amend pushed commits. Never use `--no-verify`.**
9. **Respect lane boundaries** — listed in your `AGENT-2.md` or `AGENT-3.md`. Editing outside your lane = merge conflicts with the sibling agent.
10. **STOP + ping Rhett** on any change to: serialized block/tx format, consensus constants, P2P wire format, or a row's scope/acceptance criteria. Scope changes are the coordinator's lane.

## Task discipline (how to work a row end-to-end)

1. `git pull --rebase` — sync to origin/main
2. Open your lane file (AGENT-2.md or AGENT-3.md) → read the **Current status — NOW = P<X.Y>** block
3. Reproduce the failure on a fixture deterministically (for bug rows) or design the RED test shape (for feature rows)
4. Write the RED test in `lib/test/src/` or the appropriate test file. Commit it alone with the row ID.
5. `git push` the RED commit
6. Implement the GREEN fix. `make test` must pass.
7. Commit the GREEN with the row ID. `make ci`. `git push`.
8. Update AGENT.md + your lane file: mark the row `done <GREEN-SHA> [test:1.0 <RED-SHA>]`, advance the NOW line to the next row. Commit + push as a separate docs commit.

If any step fails, **don't force it** — investigate, diagnose, ask.

## MCP tools available

The `zcl23` MCP server exposes the running node's state. Key tools:

| Tool | Use |
|---|---|
| `zcl_status` | Height, peers, sync state, validation, health (single call) |
| `zcl_kpi` | Full KPI dashboard — every subsystem |
| `zcl_getblockcount` | Current tip height |
| `zcl_peers` | Connected peers with latency + services + heights |
| `zcl_events` | **Recent event log — best tool for diagnosing sync stalls** |
| `zcl_logtail` | Tail the structured event log (by domain prefix) |
| `zcl_health` | Pass/fail health summary |
| `zcl_validationstatus` | Background-validation progress |
| `zcl_peer_report` | Peer scoring + ban/offence counts |
| `zcl_rpc` | Escape hatch for any of 100+ RPC methods |

Full list in [`CLAUDE.md`](CLAUDE.md).

**⚠ UNSAFE RIGHT NOW (crashes the live node):** `zcl_syncdiag` —
tracked as P24.11 CRITICAL. Do not call it. Use `zcl_events` for sync
diagnostics instead.

If `gemini mcp list` doesn't show `zcl23`, register it:
```
gemini mcp add zcl23 -- zclassic23 -mcp
```

## Build & deploy

```bash
make -j$(nproc)     # Build zclassic23 + test_zcl + zclassic-cli
make test           # Run 1500+ tests — required before every commit
make lint           # Grep-gate sweep (check_raw_sqlite, nodiscard, etc.) — required before push
make ci             # = make lint + make test
make deploy         # Coordinator only — do not run as a worker
```

Workers push; Rhett deploys. Never run `make deploy` in a worker session.

## Project architecture (one paragraph)

zclassic23 is a 26 MB C23 single-binary fork of zclassicd. It contains
a full Equihash-200/9 PoW node, a Tor-embedded `.onion` hidden service
with an MVC web explorer, FlyClient fast-sync (~60s to tip), a ZSLP
token protocol, Sapling zk-SNARKs, ZCL Names (ZNAM), encrypted
messaging (ZMSG), a crypto-incentivized file-share market, dcrdex-
compatible cross-chain atomic swaps, and an MCP server for AI-agent
integration. Every feature is compiled into the one binary — no
external daemons, no Docker, no Python, no shell scripts.

## Conventions that trip up generic AI assistants

- **No calendar-time estimates.** Banned phrases: "this will take 2 weeks", "by Thursday", "in a month". Use task counts, dependency chains, and tier labels (CRITICAL / HIGH / MED / LOW).
- **Never use the word "god"** (as in "god-object" / "god-file"). Use "oversized module" or "monolith file".
- **Cite borrowed concepts** in the commit message and in `ATTRIBUTIONS.md`. Erigon concepts (LGPL-3.0) are compatible with our Apache-2.0 but must be credited.
- **Concise shell commands.** Don't waste tokens on verbose flags. Prefer `make test` over `make test --verbose`.
- **UTXO set must match zclassicd bit-for-bit.** P12.3 parity diff is the source of truth.
- **Never wipe the wallet keystore.** Never delete `wallet_canary`. Never drop UTXOs above tip. These are postmortem-flagged landmines.

## Coordination with the sibling Gemini worker

Both Agent-2 and Agent-3 are Gemini CLI instances running in sibling
worktrees. You do not read each other's working directories. You
coordinate through:

- `git pull --rebase` (at session start + before every push)
- AGENT.md updates (commit them as separate docs commits)
- Commit messages with clear row IDs

If a rebase conflicts with sibling changes, resolve manually — do not
`git push --force`. The coordinator (Rhett, running Claude Code in
`~/zclassic23`) will untangle any genuine conflicts.

## If you're blocked

Don't invent work. Don't go off-lane. The failure modes are:
1. Test fixture won't reproduce the bug → write up what you found in AGENT.md and ping Rhett
2. Fix shape is unclear → propose two options in AGENT.md and ping Rhett
3. A row turns out to be larger than expected → file a follow-up row, scope down the current one
4. You hit a `STOP + ping Rhett` trigger → stop immediately, summarize, wait

Rhett is reachable by writing to AGENT.md and pushing — the coordinator
runs continuously and will pick up the update on the next poll.
