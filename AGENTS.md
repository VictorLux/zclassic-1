# zclassic23 — AGENTS.md (portable agent onboarding)

This file is the **portable** orientation index. Both Claude Code and
Gemini CLI read it; any AI coding agent with an `AGENTS.md` convention
should read it first.

> If you need tool-specific details: Claude Code reads [`CLAUDE.md`](CLAUDE.md),
> Gemini CLI reads [`GEMINI.md`](GEMINI.md). The rules in this file apply
> to both.

## First 60 seconds — figure out your role

1. Are you **the coordinator** or **a worker**?
   - Working in `~/zclassic23` → **coordinator** (Rhett's lane; read [`AGENT.md`](AGENT.md))
   - Working in `~/zclassic23-2` → **Agent-2** worker (read [`AGENT-2.md`](AGENT-2.md))
   - Working in `~/zclassic23-3` → **Agent-3** worker (read [`AGENT-3.md`](AGENT-3.md))

2. `git pull --rebase` before doing anything else.

3. Open your per-role file → find the **Current status — NOW = P<X.Y>** line.
   That row is your assignment.

## The rules (non-negotiable)

1. **RED-first test discipline.** Every code-touching row lands as two
   commits: a RED failing test, then the GREEN fix. No hotfixes.
2. **One row per commit.** Row ID (e.g. `P24.13`) in the commit message.
3. **`make test` before every commit. `make ci` before every push.**
4. **Respect lane boundaries** — listed in your AGENT-2.md / AGENT-3.md.
   Editing outside your lane causes merge conflicts with the sibling.
5. **No shell scripts. No Python. No Docker.** Everything in C23,
   compiled into the `zclassic23` binary, exposed via MCP.
6. **Never amend pushed commits. Never `--no-verify`. Never
   `git push --force`.**
7. **STOP + ping Rhett** on any change to: serialized block/tx format,
   consensus constants, P2P wire format, or a row's scope.

## Key files

| File | What |
|---|---|
| [`AGENT.md`](AGENT.md) | Master plan — every row, tier rollup, coordinator state |
| [`AGENT-2.md`](AGENT-2.md) | Agent-2 executable checklist (wallet/storage/net/validation) |
| [`AGENT-3.md`](AGENT-3.md) | Agent-3 executable checklist (crypto/sapling/keys) |
| [`CLAUDE.md`](CLAUDE.md) | Project overview, MCP tool surface, architecture |
| [`GEMINI.md`](GEMINI.md) | Gemini CLI onboarding (session-start routine) |
| [`DEFENSIVE_CODING.md`](DEFENSIVE_CODING.md) | Mandatory standards enforced by `make lint` |
| [`ATTRIBUTIONS.md`](ATTRIBUTIONS.md) | Concept-borrow attributions (cite in commits) |
| [`LICENSE`](LICENSE) + [`NOTICE`](NOTICE) | Apache-2.0 + upstream copyright preservation |

## MCP tools (via the `zcl23` MCP server)

The running node exposes 60+ tools — see [`CLAUDE.md`](CLAUDE.md) for
the full inventory. Most useful subset for diagnostic work:

`zcl_status`, `zcl_kpi`, `zcl_getblockcount`, `zcl_peers`, `zcl_events`,
`zcl_logtail`, `zcl_health`, `zcl_validationstatus`, `zcl_peer_report`,
`zcl_rpc` (escape hatch for any of 100+ RPC methods).

**⚠ `zcl_syncdiag` crashes the live node (P24.11 CRITICAL).** Use
`zcl_events` for sync diagnostics until that row lands.

## Build

```bash
make -j$(nproc)   # build
make test         # tests — required before every commit
make ci           # lint + tests — required before every push
make deploy       # coordinator only — restarts the live mainnet node
```

## License

Apache-2.0. See [`LICENSE`](LICENSE). Upstream copyright notices
preserved in [`NOTICE`](NOTICE). Per-file SPDX headers are row P24.12.
