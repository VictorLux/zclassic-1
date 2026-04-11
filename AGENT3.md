# AGENT3 — MCP Security, Model Validation & Operational Observability

**Worktree:** `~/zclassic23-3`
**Workflow:** push directly to `master` after `./test_zcl` passes
**Coordinator:** AGENT1 (main `~/zclassic23`)
**Peer:** AGENT2 (`~/zclassic23-2`) — working on chain-state / recovery, different files

---

## Mission

The MCP surface is now the primary control plane for zclassic23 — AI agents, operators, and automation all reach the node through it. That makes it the attack surface **and** the observability surface. This wave hardens both sides: model validation hooks (data integrity), MCP middleware (security + rate limiting + timeouts), end-to-end integration tests (real-binary coverage), and structured metrics (operational visibility).

## Already done (don't redo)

- Phase 1: router + 41 tools behind schemas, EV_MCP_REQUEST logging, 27 unit tests
- Phase 2a: 22 backfill tools — total now **66 tools**
- Phase 2b: handlers split into `tools/mcp/controllers/*.c`, `mcp_server.c` down to 184 lines
- Phase 2c: `zcl_tools_list`, `zcl_self_test`, `zcl_logtail` operator tooling
- Defensive `json_get(obj, key)` null check in `lib/json/src/json.c`
- Live smoke test result: **66 total / 42 pass / 0 fail / 24 skip**

## Next wave — ordered by impact

### 1. Phase 3 — Model validator hooks

Every model has a `_validate()` function that's never called automatically. This is the "no validation on save" problem that lets bad data into SQLite without anyone noticing.

Build the validator registry in `app/models/src/database.c`:

```c
typedef bool (*db_validator_fn)(const void *row, char *err_out, size_t err_cap);

struct db_validator_entry {
    const char     *table;
    db_validator_fn fn;
};

void db_register_validator(const char *table, db_validator_fn fn);

/* Called by every db_<model>_save() before the SQL INSERT/UPDATE runs.
 * Returns false and populates err_out on failure; the save aborts. */
bool db_run_validators_for(const char *table, const void *row,
                            char *err_out, size_t err_cap);
```

Then wire validators for all 17 models:

`block`, `block_data`, `chain_snapshot`, `contact`, `file_service`, `leveldb_store`, `mempool_entry`, `mmb_leaf_store`, `onion_announcement`, `peer`, `store`, `tx_index`, `utxo`, `wallet_key`, `wallet_tx`, `zslp`, + `database` itself (the meta-row).

Each validator should enforce the **structural invariants** that the model's existing `_validate()` function already knows but never runs. Audit each one and write the validator to call it.

When validation fails, emit `EV_DB_VALIDATION_FAILED` with the table name and error message and **fail the save**. Do not silently pass through.

Tests: `lib/test/src/test_db_validators.c` — at least one positive and one negative test per table (so 34+ cases). Use in-memory sqlite.

**Coordination with AGENT2**: AGENT2 may touch `app/models/src/database.c` for their chain-state migration. Check their last commit before you start, and if they're mid-flight, do your work in a separate file (`database_validators.c`) that `database.c` `#include`s at the bottom. Small commits, frequent pulls.

### 2. MCP middleware — security + rate limiting + timeouts

Right now any MCP caller can:
- spam the node with unbounded request volume
- make a handler hang forever (no timeout)
- call destructive tools without auth

Build middleware that runs between `mcp_router_dispatch()` and the handler:

```c
struct mcp_middleware {
    /* Auth: if non-empty, require Bearer token in request metadata */
    const char *required_bearer_token;    /* env ZCL_MCP_BEARER_TOKEN */

    /* Rate limiting: token bucket per-tool */
    int64_t global_rps;                   /* env ZCL_MCP_GLOBAL_RPS, default 100 */
    int64_t destructive_rps;              /* env ZCL_MCP_DESTRUCTIVE_RPS, default 1 */

    /* Per-tool execution timeout */
    int64_t default_timeout_ms;           /* env ZCL_MCP_TIMEOUT_MS, default 5000 */

    /* Destructive tool marker (already partially modeled in zcl_self_test) */
    const char **destructive_tools;       /* {"zcl_sendtoaddress", "zcl_importprivkey", ...} */
    size_t       num_destructive_tools;
};

/* Middleware wraps the dispatch. Returns the same envelope shapes the
 * router already produces. New error codes: AUTH_REQUIRED, RATE_LIMITED,
 * TOOL_TIMEOUT. */
int mcp_middleware_dispatch(struct mcp_middleware *mw,
                             const struct mcp_request *req,
                             struct mcp_response *res);
```

Implementation:
- **Timeout**: run handler on a worker thread with a `pthread_cond_timedwait` on a completion semaphore. If timeout fires, mark the request abandoned (do not cancel the thread — C doesn't do safe thread cancellation for general code; let it finish and discard). Log `EV_MCP_TIMEOUT` with tool + elapsed_ms.
- **Rate limit**: two token buckets — global and destructive — refilled by `clock_gettime`, no `sleep`.
- **Auth**: compare `req->metadata["authorization"]` against the env token. Constant-time compare.

Mark these tools as destructive (use the same list `zcl_self_test` already skips):
`zcl_sendtoaddress`, `zcl_send`, `zcl_importprivkey`, `zcl_rescanblockchain`, `zcl_replaywalletfromchain`, `zcl_addnode`, `zcl_swap_initiate`, `zcl_swap_participate`, `zcl_market_buy`, `zcl_market_offer`, `zcl_msg_send`, `zcl_msg_send_named`, `zcl_name_register`.

Tests in `lib/test/src/test_mcp_middleware.c`: at least 12 cases covering every path (auth pass/fail, global limit, destructive limit, timeout fire, destructive marker matching, env-var loading, concurrent bucket refills).

### 3. End-to-end MCP integration test — `tools/mcp/test_mcp_e2e.c`

Today `test_mcp_router` tests the router in isolation. There's no test that **forks the real `zclassic23 -mcp` binary, writes JSON-RPC over stdio, and asserts on the response**. Build one:

```c
/* Start zclassic23 -mcp -datadir=<tempdir>, speak MCP over its stdin/stdout,
 * run every non-destructive tool, assert envelope shape and required fields.
 * Cleanup on exit. */
```

This is the test that would catch a regression like "Phase 2b renamed a field and nobody noticed because the router test doesn't touch JSON shapes". Include:

- `initialize`, `tools/list` → exact 66 tool names
- `zcl_status`, `zcl_health`, `zcl_kpi` → required fields present
- error envelope: send malformed JSON-RPC, assert `-32700` parse error
- validation envelope: call `zcl_getblock` with no args, assert `MISSING_PARAM` envelope with correct `tool` and `param`
- auth: if `ZCL_MCP_BEARER_TOKEN` is set in the child env, unauth'd call returns `AUTH_REQUIRED`
- timeout: call a tool that sleeps (add a `zcl_testsleep` tool under `#ifdef ZCL_TESTING` just for this test) and assert `TOOL_TIMEOUT`

Add a `make test-e2e` target that builds a fresh `zclassic23` binary and runs this test against it. CI should run it.

### 4. Structured metrics — `tools/mcp/metrics.{h,c}`

Expose a Prometheus-style text endpoint over MCP so an operator can scrape live stats:

```
zcl_mcp_requests_total{tool="zcl_status",code="OK"} 412
zcl_mcp_request_duration_seconds_bucket{tool="zcl_getblock",le="0.005"} 183
zcl_mcp_rate_limited_total{kind="destructive"} 2
zcl_chain_state_commits_total{code="OK"} 3481
zcl_chain_state_commits_total{code="REJECTED_STALE_INDEX"} 0
zcl_utxo_count 1268980
zcl_peer_count 9
zcl_sync_height 2014948
```

Implementation:
- Hook into `EV_MCP_REQUEST` (already emitted) to build the histogram buckets in-process
- Hook into `EV_CHAIN_TIP_COMMIT` / `EV_CHAIN_TIP_REJECTED` (AGENT2's events) for chain-state counters
- Expose two new MCP tools:
  - `zcl_metrics` — returns the full Prometheus text as a JSON string
  - `zcl_metrics_reset` — clears counters (gated by the destructive-tool marker from wave 2)

Tests: `lib/test/src/test_mcp_metrics.c` — at least 8 cases (counter increment, histogram bucketing, label cardinality limit, reset behavior, text-format conformance).

### 5. Self-documenting schema export — `zcl_openapi`

Add a new tool `zcl_openapi` that returns a pseudo-OpenAPI JSON document derived from the routing table. This lets a client (Claude Code, a TypeScript type generator, an auto-test harness) know every tool's parameters and expected shape without hitting a second endpoint. Build it by walking `mcp_router_tools_list_json()` and wrapping each tool in an OpenAPI-style `paths` entry.

Don't make it a real OpenAPI spec — just give it the same flavor. One commit, ~150 lines.

---

## Rules (unchanged)

- Rebase/pull before every session. Push directly to `master` when `./test_zcl` is green.
- One commit per logical step. `agent3:` prefix.
- **Preserve all existing tool names and argument shapes.** New tools: any name, but document them in `CLAUDE.md` in the same commit.
- **Never touch** `config/src/boot.c`, `app/services/chain_state_repository.*`, `app/services/recovery_policy.*`, `app/services/block_index_integrity.*`, `app/services/db_txn.*` (AGENT2's pen).
- **Coordinate with AGENT2 on `app/models/src/database.c`** — if they're mid-flight, put your validator wiring in a separate file.
- Smoke-test after every push (`zcl_status`, `zcl_kpi`, `zcl_self_test`). Paste `zcl_self_test` results into Current Status — that's our regression trip-wire.
- Update "Current Status" each session.

## Definition of done for this wave

- [ ] All 17 models have validators wired via `db_register_validator`
- [ ] `mcp_middleware` with auth, rate limiting, per-tool timeout
- [ ] `test_mcp_e2e` runs the real `-mcp` binary and asserts envelope shapes
- [ ] `zcl_metrics` returns Prometheus text with request/chain-state counters
- [ ] `zcl_openapi` emits a schema document derived from the routing table
- [ ] `./test_zcl` still green — now with validators + middleware + metrics + e2e tests
- [ ] Live `zcl_self_test` still shows **0 fail** after all changes

---

## Current Status

*(Update each session. Keep it short.)*

- **2026-04-11 wave 1** — Phase 1 + 2a + 2b + 2c landed. 66 tools. `zcl_self_test`: 42 pass / 0 fail / 24 skip.
- **2026-04-11 wave 2** — **New plan, five deliverables above.** Start with #1 (validator hooks). Confirm with the latest master commit on `app/models/src/database.c` that AGENT2 isn't mid-flight there; if they are, land your wiring in a separate `database_validators.c` that `database.c` includes.
