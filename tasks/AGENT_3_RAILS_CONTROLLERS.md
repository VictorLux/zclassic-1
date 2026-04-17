# Agent 3 — Rails-grade Controller + Service Layer (Track C)

**Pick up when:** your `a3/build-ci-deploy-hardening` PR has merged and you've pulled. Don't stand down.

**Read first:** [`RAILS_PARITY_PLAN.md`](../RAILS_PARITY_PLAN.md), then `lib/rpc/src/server.c` (strong params live there), `app/controllers/src/transaction_controller.c` (one of the 16 that already use `sp_require` — your template).

**Worktree:** `~/zclassic23-3`
**Branch:** `a3/rails-strong-params` (first of three PRs on this track)
**Base:** `origin/master`
**Directive:** keep pushing to master.

---

## Mission, one sentence

Make every RPC / REST / MCP entrypoint a Rails controller: strong params on input, uniform `{errors: {field: [messages]}}` envelope on output, `before_action` filters for wallet/chain/tor preconditions, `after_commit` for notifications, AR everywhere a raw `sqlite3_step` lives today.

---

## Scope (Track C from the plan)

### PR 1 — `a3/rails-strong-params`

**Files:**
- 26 of 42 files under `app/controllers/src/` that don't yet call `sp_require` / `sp_permit` (see [Inventory](#controller-inventory) below)
- `lib/rpc/src/server.c` — lint gate: any public handler entry without an `sp_require` call on first 20 lines fails `make lint`
- `Makefile` — add `check-strong-params` target; wire into `lint`
- `lib/test/src/test_strong_params.c` — extend with a handler that forgets `sp_require`, assert lint catches it

**Deliverables:**

1. **C1 — strong params everywhere.** For each of the 26 uncovered controllers, add at function entry:
   ```c
   if (!sp_require(params, "foo"))    return rpc_error(result, SP_MISSING_FIELD, "foo required");
   if (!sp_permit(params, (const char *[]){"foo","bar",NULL})) return rpc_error(result, SP_UNKNOWN_FIELD, "unknown field");
   ```

2. **C2 — uniform error envelope.** Ship `rpc_errors_from_ar_errors(struct ar_errors *, json_writer *)` in `lib/rpc/src/server.c` that emits:
   ```json
   {"error":{"code":"validation_failed","fields":{"address":["can't be blank","is not a t-addr"]}}}
   ```
   Every controller that returns a validation error converts via this helper. Ad-hoc error strings in controllers retire.

3. **Lint gate.** `make check-strong-params`: grep every controller function whose name matches `^rpc_|^handle_` and verify first 20 lines contain `sp_require`. If not → `FAIL`. Ship with D2-style `exit 1`, not warning.

**Done when:** all 42 controllers pass the lint gate; `test_strong_params` catches a deliberately-missing gate; `make ci` green.

#### Controller inventory

Already covered (16 — skip): transaction_controller, wallet_shielded_controller, game_controller, network_controller, event_controller, misc_controller, hodl_controller, chain_inspect_controller, file_controller, wallet_diagnostic_controller, swap_controller, name_controller, messaging_controller, file_market_controller, mining_controller, store_controller.

Still to do (26): api_controller, blockchain_controller, blog_controller, explorer_controller, explorer_factoids, explorer_stats, health_controller, legacy_import, repair_controller, snapshot_controller, sync_controller, wallet_controller, wallet_helpers (internal — may not need), wallet_rescan_controller, wallet_scan, wallet_view_controller, wallet_view_coins, wallet_view_dashboard, wallet_view_helpers, wallet_view_history, wallet_view_node, wallet_view_projection, wallet_view_receive, wallet_view_send, wallet_view_shield, zslp_controller.

Note: wallet_view_* are mostly read-only dashboards — strong-param surface is the params coming in from HTML form state, not mutating data. Still require the gate for consistency; `sp_permit` on the empty-allowed list is the explicit "no params expected" signal.

---

### PR 2 — `a3/rails-before-actions`

**Files:**
- `app/controllers/src/*.c` — register `before_action` filters via the existing router
- `lib/rpc/src/server.c` — wire `ar_router_add_filter` calls at boot
- `lib/net/src/ws_events.c` — wire the `after_commit → WebSocket` path
- `lib/test/src/test_before_actions.c` (new)

**Deliverables:**

1. **C4 — `before_action` filters.** Register via `ar_route_add_filter`:
   - `require_wallet_unlocked` — on `sendtoaddress`, `sendmany`, `signrawtransaction`, `dumpprivkey`, `importprivkey`, `z_sendmany`, `z_shieldcoinbase`, `walletpassphrasechange`.
   - `require_chain_synced` — on `sendtoaddress`, `sendmany`, `getblocktemplate`, every mining handler.
   - `require_tor_bootstrapped` — on onion-service handlers (`onion_status`, `/directory.json`, any ZCL Name resolve that depends on .onion).
   - `rate_limit_rpc` — global filter via `ar_router_add_filter`, 30 requests/minute per API key by default.

2. **C6 — after_commit hooks (depends on Track M4 landing first — PR3 of Agent 2 merges before this PR ships).**
   - wallet_tx `after_commit` → emit `EV_WALLET_TX_COMMITTED` → refresh wallet balance cache + push WebSocket.
   - block `after_commit` → move `EV_BLOCK_CONNECTED` emission here (currently fires on `AR_FINISH_SAVE`).
   - peer save `after_commit` → debounced addrman persist (already have the service — wire it to the hook instead of the 60s timer).

3. **Tests (`test_before_actions.c`):**
   - call `sendtoaddress` with wallet locked → expect filter rejection, handler never runs.
   - call mining handler pre-sync → rejected.
   - overflow rate limit → rejected, subsequent calls succeed after window.

**Ordering note:** if Track M4 isn't merged when you start this PR, use `after_save` hooks as a temporary stand-in and flag with `// TODO(agent2): move to after_commit when M4 lands`. Do not block.

**Done when:** every wallet-mutating handler passes the locked-wallet test; `after_commit` hooks fire only on real commit when Track M4 is in.

---

### PR 3 — `a3/rails-controllers-ar`

**Files:**
- `app/controllers/src/blockchain_controller.c` (90 raw sqlite → migrate or annotate)
- `app/controllers/src/explorer_stats.c` (23)
- `app/controllers/src/explorer_factoids.c` (17)
- `app/controllers/src/explorer_controller.c` (13)
- `app/controllers/src/sync_controller.c` (9), `wallet_view_history.c` (5), `repair_controller.c` (5), `api_controller.c` (5), others (<5 each)
- `tools/mcp/**/*.c` — every handler ending in a bare `return -1;` with no body set (P1.5)
- `tools/mcp/openapi_gen.c` (new) — generate OpenAPI schema from registered validators + strong-param lists

**Deliverables:**

1. **C3 — controller AR migration.** Migrate every raw `sqlite3_step` / `sqlite3_exec` in `app/controllers/` to:
   - `db_<model>_find_*` / `db_<model>_each` from the model layer (preferred), OR
   - `AR_QUERY_*` macros (for controller-local SQL), OR
   - `AR_STEP_ROW_READONLY` wrapper from Agent 3's build/CI/deploy task (for read-only explorer dashboards that legitimately need a one-shot query), OR
   - `// raw-sql-ok: <reason>, agent-3` annotation for the remainder, with a LINEAR rationale comment.

   Target: `blockchain_controller.c` under 10 raw step calls, with every remaining one annotated.

2. **C5 — MCP handler parity.** Audit every handler in `tools/mcp/controllers/*.c`:
   - Every error path sets an error body via `mcp_set_error(result, code, message)` — no silent `return -1;`.
   - Every handler participates in the strong-params lint gate from PR 1.
   - Errors flow through `rpc_errors_from_ar_errors` (PR 1's C2 helper).

3. **C7 — OpenAPI / schema docs.** `tools/mcp/openapi_gen.c` walks the router at boot and emits an OpenAPI 3.1 document describing:
   - every registered method + its category
   - the strong-params whitelist
   - the validator names (from Track M's named-validator registry if available; otherwise table name)
   - standard error envelope schema

   `zcl_openapi` MCP tool returns the generated doc. Closes the drift between code and docs.

**Done when:** `blockchain_controller.c` has ≤10 raw sqlite calls, each annotated; no MCP handler returns `-1` without a body; `zcl_openapi` output includes every currently-registered RPC method.

---

## Files you MUST NOT touch

- `app/models/src/*.c` — Agent 2's Track M.
- `app/models/include/models/*.h` — except adding `#include` of new M1 matchers when needed.
- `lib/util/src/result.c` — Agent 2's.
- `lib/wallet/src/wallet_sqlite.c` — Agent 2 owns it for the zcl_result rollout.

If you need a model change to complete a controller migration, leave a `// TODO(agent2): expose db_<model>_find_by_<field>_r` breadcrumb and keep the raw-sqlite annotation until they ship it.

---

## Done when all three PRs merge

- [ ] `make lint` green with strong-params gate FAILing on bypass.
- [ ] `make ci` green.
- [ ] Every RPC/REST/MCP handler has `sp_require` + `sp_permit` on first 20 lines.
- [ ] Every validation error returns the uniform JSON envelope.
- [ ] `before_action` filters reject locked-wallet / pre-sync / pre-tor calls.
- [ ] `after_commit` hooks fire only on real commit (once M4 lands).
- [ ] `app/controllers/src/blockchain_controller.c` has ≤10 raw sqlite calls, each annotated.
- [ ] `zcl_openapi` describes every registered method.

---

## Hand-off (each PR)

```
cd ~/zclassic23-3
git push origin <branch>
gh pr create --title "<title>" --body "$(cat <<'EOF'
## Summary
See RAILS_PARITY_PLAN.md Track C, PR <N>.

## Plan
<3 bullets>

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

When all three merge, pull and request the next assignment. Do not stand down.
