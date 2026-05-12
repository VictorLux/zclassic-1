# zclassic23 — AGENTS.md (portable agent onboarding)

This file is the **portable** orientation index. Both Claude Code and
Gemini CLI read it; any AI coding agent with an `AGENTS.md` convention
should read it first.

> If you need tool-specific details: Claude Code reads [`CLAUDE.md`](CLAUDE.md).
> Codex CLI + any other AGENTS.md-convention agents read this file.

## First 60 seconds — figure out your role

1. Are you **the coordinator** or **a worker**?
   - Working in `~/zclassic23` → **coordinator** (Rhett's lane; read [`AGENT.md`](AGENT.md))
   - Working in `~/zclassic23-2` → **Agent-2** worker (read [`AGENT-2.md`](AGENT-2.md))
   - Working in `~/zclassic23-3` → **Agent-3** worker (read [`AGENT-3.md`](AGENT-3.md))

2. `git pull --rebase` before doing anything else.

3. If you are in `~/zclassic23-2` or `~/zclassic23-3`, start from the
   project Kanban:

   ```text
   vibepoint:kickoff { "agent": 2 }
   vibepoint:kickoff { "agent": 3 }
   ```

   Use the matching agent number for your workspace, then immediately
   follow the tool's required next call (`task_claim_next`, `task_packet`,
   or resume the active claim). The zclassic Vibepoint database is
   authoritative for current assignment and claim state.

4. Open your per-role file only as supporting context after kickoff.
   If it disagrees with `vibepoint:kickoff`, follow the MCP result
   and ping Rhett.

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
| [`DEFENSIVE_CODING.md`](DEFENSIVE_CODING.md) | Mandatory standards enforced by `make lint` |
| [`ATTRIBUTIONS.md`](ATTRIBUTIONS.md) | Concept-borrow attributions (cite in commits) |
| [`LICENSE`](LICENSE) + [`NOTICE`](NOTICE) | Apache-2.0 + upstream copyright preservation |

## MCP tools (via the `zcl23` MCP server)

The running node exposes 60+ tools — see [`CLAUDE.md`](CLAUDE.md) for
the full inventory. Most useful subset for diagnostic work:

**SAFE (use these freely):** `zcl_status`, `zcl_kpi`, `zcl_getblockcount`,
`zcl_peers`, `zcl_peer_report`, `zcl_events`, `zcl_logtail`, `zcl_health`,
`zcl_validationstatus`.

**⚠ UNSAFE — WILL CRASH THE LIVE NODE (confirmed via live backtrace 2026-04-21):**

Do NOT call these — SIGABRT the mainnet node:

- **P24.11 (json_free UAF in rpc_getsyncdiag):** `zcl_syncdiag` / `getsyncdiag`.
- **P24.14 (coins_view_cache_get_coins SEGV on inverted tail — 16 RPC callsites across 5 controllers):**
  `zcl_getrawtransaction`, `zcl_walletaudit`, `zcl_listunspent`, `zcl_z_listunspent`,
  `zcl_rescanblockchain`, and any raw RPC that reads the UTXO cache
  (`gettxout`, `getrawtransaction`, `listunspent`, `z_listunspent`, `rescanblockchain`,
  anything under `wallet_diagnostic_controller.c`).
- `zcl_rpc` escape hatch — only use for methods confirmed safe; arbitrary methods
  against the current P24.13-inverted state can hit either crash class.

If a tool prompts permission to run something not listed as safe, DENY and ask Rhett.

Use `zcl_events` + `zcl_logtail` for diagnostics. If a tool prompts permission
to run something not listed here, deny and ask Rhett.

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
