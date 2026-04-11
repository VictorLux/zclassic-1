# AGENT3 — MCP MVC & Controller Refactor

**Worktree:** `~/zclassic23-3`
**Branch:** `agent3/mcp-mvc` (create from `origin/master`)
**Coordinator:** AGENT1 (main `~/zclassic23`)
**Peer:** AGENT2 (`~/zclassic23-2`) — working on chain-state repository, different files

---

## Mission

Turn `tools/mcp_server.c` from a **1,159-line flat dispatcher** into a clean MVC surface: routing table + parameter schemas + domain controllers + view formatters + structured logging. The MCP server is how AI agents (including you) control the node — it must be robust, introspectable, and add-a-tool easy.

## Why this matters

Today's MCP server is a ~270-line if/else chain with:

- no parameter schema validation (each tool manually calls `json_get(args, ...)`)
- no error envelope (some tools return strings, some objects, some just crash)
- no middleware (no auth, no rate-limit, no request logging)
- no test coverage for the dispatcher itself
- one monolithic file where every tool lives cheek-by-jowl with unrelated ones

Adding a 61st tool means pattern-matching 60 inconsistent examples. The user's explicit ask is "complete control and mastery of everything in zclassic23 over MCP" — that needs a surface you can trust.

## Deliverables

### Phase 1 — Routing Table & Parameter Schemas (primary)

Replace the if/else dispatch with a table. Create `tools/mcp/router.{h,c}`:

```c
struct mcp_param_spec {
    const char *name;
    enum mcp_param_type type;    /* MCP_STR, MCP_INT, MCP_BOOL, MCP_ARRAY, MCP_OBJECT */
    bool required;
    const char *description;
    /* Validators */
    int64_t min_int, max_int;    /* for MCP_INT */
    size_t  min_len, max_len;    /* for MCP_STR / MCP_ARRAY */
    const char *pattern;         /* optional regex for MCP_STR */
};

struct mcp_tool_route {
    const char *name;              /* e.g. "zcl_getblock" */
    const char *domain;            /* "wallet" | "chain" | "net" | "ops" | "app" */
    const char *description;
    const struct mcp_param_spec *params;
    size_t                       num_params;
    mcp_handler_fn               handler;
};

/* Handlers get pre-validated, type-checked args. No more json_get(). */
typedef int (*mcp_handler_fn)(const struct mcp_request *req,
                               struct mcp_response *res);
```

Requirements:

- **Schema-driven JSON output** for `tools/list` generated from the table (no more hand-maintained duplication between `tools[]` and the dispatcher)
- **Parameter validation runs before the handler** — required fields, types, ranges, lengths all checked; handler sees only valid input
- **Consistent error envelope**: `{ "error": { "code": "...", "message": "...", "tool": "...", "param": "..." } }`
- **Structured request log** emitted to `EV_MCP_REQUEST` — tool name, duration, result code
- **Test coverage**: `lib/test/src/test_mcp_router.c` with at least 15 tests (missing required param, wrong type, out-of-range int, etc.)

### Phase 2 — Domain controllers

Split the tool handlers out of `mcp_server.c` into files by domain:

```
tools/mcp/
├── router.h / router.c
├── controllers/
│   ├── chain_controller.c      # zcl_getblock, zcl_getblockcount, zcl_getblockchaininfo, ...
│   ├── wallet_controller.c     # zcl_balance, zcl_listunspent, zcl_sendtoaddress, ...
│   ├── net_controller.c        # zcl_peers, zcl_networkinfo, zcl_pingpeer, ...
│   ├── ops_controller.c        # zcl_health, zcl_kpi, zcl_benchmark, zcl_dbstats, ...
│   └── app_controller.c        # zcl_name_*, zcl_msg_*, zcl_market_*, zcl_swap_*, ...
└── views/
    └── json_formatters.c       # shared formatters (block -> json, tx -> json, ...)
```

`tools/mcp_server.c` shrinks to: boot the router, register all routes, loop on stdin. Target: **<300 lines.**

### Phase 3 — Model validation hooks (coordinate with AGENT2)

Models currently have validation functions that are *not* called automatically on save. Add a hook registration in `app/models/src/database.c`:

```c
/* Registered once at boot. Save paths look up the hook and run it. */
void db_register_validator(const char *table, db_validator_fn fn);

/* Returns false and logs on failure; save returns error. */
bool db_run_validators_for(const char *table, const void *row);
```

Then wire validators for the 17 models. This is a cross-cutting change — do it AFTER Phase 1 and 2 are merged so conflicts with AGENT2's work are minimized.

### Phase 4 — Operator tooling (stretch)

- `zcl_tools_list` — dump the routing table as JSON (schema, descriptions, domain grouping)
- `zcl_self_test` — calls every MCP tool with safe defaults and reports which ones work
- `zcl_logtail` — tail the structured log over MCP for remote debugging

---

## Coordination rules

### Syncing with main

You work on a separate clone at `~/zclassic23-3`. Your workflow:

```bash
cd ~/zclassic23-3
git fetch origin
git rebase origin/master        # before starting any session
# ... work ...
make -j$(nproc) test_zcl && ./test_zcl   # MUST pass before push
git push origin agent3/mcp-mvc
```

**Never force-push to `master`.** Open PRs into master or let AGENT1 merge your branch manually.

### Files you own (safe to edit freely)

- `tools/mcp_server.c` (refactor target — you get to rewrite this)
- `tools/mcp/**` (new subdirectory — yours)
- `lib/test/src/test_mcp_router.c` (new)
- `lib/test/src/test_mcp_controllers.c` (new)
- `AGENT3.md` — update "Current Status" at the bottom each session

### Files you share with AGENT1 (coordinate in commits)

- `app/controllers/**` — these are the HTTP/RPC controllers, not MCP. You will read them (your MCP controllers call into them) but avoid editing without a reason.
- `app/models/**` — Phase 3 only, and only after AGENT2's chain-state work lands

### Files AGENT2 owns — DO NOT TOUCH

- `app/services/chain_state_repository.{h,c}`
- `app/services/recovery_policy.{h,c}`
- `app/services/block_index_loader.{h,c}`
- `app/services/chain_state_validator.{h,c}`
- `config/src/boot.c`
- `lib/validation/src/chainstate.c`, `process_block.c`, `connect_block.c`

### Commit discipline

- **One commit per logical step.** Adding the router is one commit. Migrating each controller is another.
- **Every commit must build and pass `./test_zcl`.** No exceptions.
- **Commit messages start with `agent3:`** — e.g., `agent3: add mcp router with parameter schemas`
- **Never commit generated binaries** (`zclassic23`, `test_zcl`, `speedrun`, `export_snapshot`).
- **Preserve existing tool names** — external users and `CLAUDE.md` reference them. Don't rename `zcl_getblock` to `chain.getBlock`.

### Compatibility contract

The MCP surface is already documented in `CLAUDE.md`. While refactoring:

- **Every existing tool name must still exist and accept the same params.**
- **Output JSON shapes should not change** unless you're fixing an obvious bug. If you must change a shape, add a new field instead of removing one.
- **Run the "smoke test" after every push**: start a dev node, run `zcl_status`, `zcl_kpi`, `zcl_benchmark`, `zcl_balance` — all should return without error.

### When you're blocked

Write the blocker into the "Current Status" section of this file and push. AGENT1 will pick it up on the next coordination pass.

---

## Definition of done for Phase 1+2

- [ ] `tools/mcp/router.{h,c}` exists with schema validation
- [ ] Parameter validation runs before handlers for all 60+ tools
- [ ] Consistent error envelope across all tools
- [ ] `tools/mcp_server.c` is <300 lines
- [ ] All tool handlers live in domain controller files under `tools/mcp/controllers/`
- [ ] `test_mcp_router.c` has 15+ passing tests
- [ ] `./test_zcl` passes (1500+ tests, 0 failures)
- [ ] `make deploy` → all existing tools still work (run smoke test)
- [ ] `zcl_tools_list` returns the routing table as JSON

---

## Current Status

*(Update this every session with what you did and what's next. Keep it short.)*

- **2026-04-11** — Plan created by AGENT1. AGENT3 has not started.
- **2026-04-11 (AGENT3)** — **Phase 1 landed on `agent3/mcp-mvc`**:
  - `tools/mcp/router.{h,c}` — schema-driven tool dispatch with parameter
    validation (type / int range / string length / enum / required),
    consistent error envelope, and `EV_MCP_REQUEST` structured logging
    (new event type in `lib/event/include/event/event.h`).
  - `tools/mcp_server.c` rewritten: every one of the 41 existing tools
    is now registered through a `mcp_tool_route` table and dispatched
    via `mcp_router_dispatch`. `tools/list` is generated from the table
    — no more hand-maintained duplication. All existing tool names and
    argument shapes preserved (compat contract honoured).
  - `lib/test/src/test_mcp_router.c` — **27 tests**, all passing in
    isolation (standalone harness linking router.c + json.c + event.c).
    Covers: register/find/count, duplicates, unknown tool, missing
    required param, null args, wrong type, int range, string length,
    enum, handler failure, null body, schema output (type / required /
    min-max / enum), tools/list array, envelope escaping, reset.
  - Wired `test_mcp_router` into `lib/test/src/test.c` and Makefile
    picks up `tools/mcp/*.c` via a new `MCP_SRCS` variable added to
    `ALL_SRCS`. Every file I touched compiles cleanly under the real
    production CFLAGS (`-std=c23 -O3 -Werror -pedantic`).
- **🚧 BLOCKER (not AGENT3's files):** `origin/master` cannot build
  `test_zcl` — confirmed by cleaning and building pristine
  `80aae3d50`. The pre-existing errors live in files AGENT2 owns or
  that depend on AGENT2's in-progress refactor:
  - `config/src/boot.c:2157` — `__atomic_store` on non-void pointer.
  - `config/src/boot_services.c:32` — `#include "controllers/game_controller.h"`
    header doesn't exist.
  - `lib/net/src/msgprocessor.c` — `struct msg_dispatch_entry`
    incomplete type, `SNAPSYNC_OFFER_REJECTED_BLACKLISTED` undeclared,
    `snapsync_get_anchor` / `snapsync_check_stall` implicit decls,
    `process_getheaders` / `process_block_msg` signature mismatches.
  AGENT1's working copy at `~/zclassic23` has a local (unpushed) fix
  for `msgprocessor.c`, and `config/src/boot.c` is in AGENT2's
  DO-NOT-TOUCH list, so I did not attempt to fix these here.
- **2026-04-11 (AGENT1 COORDINATOR)** — **Phase 1 router MERGED to master** at commit `7499e1281`. Full `./test_zcl` green on merged master (all 15 test suites pass including the 27 new router tests). Master is unblocked; blocker resolved via AGENT1 commits `00bb201b1`, `dd3ada67c`, `729e41033`, `5be355b9e`. Proceed with Phase 2 below.
- **2026-04-11 (AGENT3)** — Rebased on top of AGENT1's master fixes (`25c1b3779`), resolved three trivial conflicts (event enum, test helper prototypes, test runner). **Full `./test_zcl` green** — 1500+ tests pass, including the 27 new router tests. Smoke-tested the new `-mcp` binary against the live node (height 2014948, 9 peers): `zcl_status`, `zcl_balance`, `zcl_getblockcount`, `tools/list` (41 tools) all return successfully, and validation errors (`MISSING_PARAM`, `ENUM_MISMATCH`, `INVALID_TYPE`, `UNKNOWN_TOOL`, `STRING_TOO_SHORT`) all produce the canonical envelope shape.
- **2026-04-11 (AGENT3) — Phase 2 landed**: handlers split from `tools/mcp_server.c` into domain controllers. New layout:
  ```
  tools/mcp/
  ├── router.{h,c}
  ├── rpc_client.{h,c}          # moved node_rpc() out of mcp_server.c
  ├── controllers.h             # single header, one register fn per domain
  └── controllers/
      ├── ops_controller.c      # zcl_status, zcl_health, zcl_events, zcl_rpc, zcl_filemanifest
      ├── chain_controller.c    # zcl_getblock*, zcl_mmb, zcl_syncstate, zcl_validationstatus, zcl_dataintegrity, zcl_utxocommitment, zcl_hodlwave
      ├── net_controller.c      # zcl_peers, zcl_networkinfo, zcl_addnode, zcl_onion_status, zcl_gametypes, zcl_pingpeer, zcl_peerlatency
      ├── wallet_controller.c   # zcl_balance, zcl_getnewaddress, zcl_z_getnewaddress, zcl_send
      └── app_controller.c      # zcl_tokens, zcl_name_*, zcl_msg_*, zcl_market_*, zcl_swap_*
  ```
  `tools/mcp_server.c` is now **183 lines** (target: <300) — just the stdio loop and three JSON-RPC method handlers; it calls `mcp_router_reset()` + `mcp_register_ops/chain/net/wallet/app()` in `register_all_controllers()`. `test_zcl` still green; smoke tests against the live node still pass. Phase 2b deliverables (controllers split) complete.
- **Still pending:** Phase 2a (backfill 22 missing tools — per AGENT1 coordinator direction below) and Phase 2c (operator tooling: `zcl_tools_list`, `zcl_self_test`, `zcl_logtail`). Phase 3 (model validator hooks) waits for AGENT2's chain-state call-site migration.
- **2026-04-11 (AGENT3) — Phase 2a + Phase 2c complete. Total MCP tools: 66.**
  - **Phase 2a backfill (22 tools)** landed across existing controllers:
    - **chain** (1): `zcl_getrawtransaction`
    - **ops** (6): `zcl_kpi` (flagship one-shot dashboard combining
      height / peer_count / sync / validation / health / mempool /
      wallet / chain / network), `zcl_getmempoolinfo`,
      `zcl_getrawmempool`, `zcl_getmininginfo`, `zcl_benchmark`,
      `zcl_dbstats`.
    - **wallet** (15): `zcl_getwalletinfo`, `zcl_listunspent`,
      `zcl_listtransactions`, `zcl_gettransaction`,
      `zcl_sendtoaddress`, `zcl_listaddresses` (projects
      `listwalletkeys` into `{t_addresses, z_addresses}`),
      `zcl_dumpprivkey`, `zcl_importprivkey`, `zcl_z_listaddresses`,
      `zcl_z_listunspent`, `zcl_z_getbalance`, `zcl_rescanblockchain`,
      `zcl_walletaudit`, `zcl_listwalletkeys`,
      `zcl_replaywalletfromchain` (guarded — requires confirm=true).
  - **Phase 2c operator tools** (`tools/mcp/controllers/meta_controller.c`, 253 lines):
    - `zcl_tools_list` — dumps the full routing table as JSON via
      `mcp_router_tools_list_json`. Self-documenting MCP surface.
    - `zcl_self_test` — iterates every registered tool, calls it with
      empty args, skips destructive or required-param-without-default,
      reports `{tool, domain, status, reason}` plus
      `{total, pass, fail, skip}` summary. **Live result against the
      running node: 66 total / 42 pass / 0 fail / 24 skip.**
    - `zcl_logtail` — wraps `eventlog` RPC, optional `domain` prefix
      filter on event `type` field, returns
      `{sync_state, filter, events, matched}`. Verified filtering
      works (`domain=msg` → 34 matches, types all start with
      "msg.").
  - **Defensive fix in `lib/json/src/json.c`:** `json_get(obj, key)`
    now null-checks `obj` and `key` instead of crashing. This fixes a
    real SIGSEGV discovered by `zcl_self_test`: handlers that call
    `json_get(req->args, ...)` would crash when the router passed
    `args=NULL`. The router already tolerates NULL; the individual
    handlers do not, and this one-line change makes the whole surface
    NULL-safe.
  - **Current line counts:** `tools/mcp_server.c` 184, `router.c` 549,
    `rpc_client.c` 159, `controllers/app_controller.c` 349,
    `chain_controller.c` 138, `meta_controller.c` 253,
    `net_controller.c` 94, `ops_controller.c` 216,
    `wallet_controller.c` 443. Target (`mcp_server.c` < 200) met.
  - **Build / tests:** `./test_zcl` — 15 suites green, 0 failures,
    ~2000 individual test cases. Full `-O3 -flto -Werror -pedantic`
    build of both `test_zcl` and `zclassic23` clean.
  - **Smoke tests against live node (height 2014948, 9 peers):**
    `zcl_status`, `zcl_kpi`, `zcl_balance`, `zcl_benchmark`,
    `zcl_getwalletinfo`, `zcl_getmempoolinfo`, `zcl_dbstats`,
    `zcl_listaddresses`, `zcl_tools_list`, `zcl_self_test`,
    `zcl_logtail` — all return success and produce well-formed
    bodies. Validation error envelopes (`MISSING_PARAM`, `INVALID_TYPE`,
    `OUT_OF_RANGE`, `ENUM_MISMATCH`, `UNKNOWN_TOOL`) all shape-correct.
- **Still pending:** Phase 3 (model validator hooks on 17 models).
  Waits for AGENT2's next chain-state milestone to avoid conflicts in
  `app/models/src/database.c`.

---

## COORDINATOR DIRECTION — Phase 2 & Phase 3

Phase 1 router landed cleanly. The MVC surface is now introspectable and schema-validated — exactly what was asked for. Here's the Phase 2 work, which has grown a little since the branch was cut.

### Phase 2a — Backfill the 22 missing tools (NEW, do this FIRST)

When you rewrote `mcp_server.c` you inherited 41 tools from the old tracked master. AGENT1's local working tree had **22 additional tools** that were in use but never committed (that's why the memory file at `project_mcp_tools.md` says "60+"). They're documented in `CLAUDE.md` and the agent memory. You need to re-register them via the router so the public surface matches the docs.

**Missing wallet tools (15):**
```
zcl_getwalletinfo         — one-shot wallet health snapshot
zcl_listunspent           — UTXOs available to spend
zcl_listtransactions      — wallet tx history
zcl_gettransaction        — single tx by id
zcl_sendtoaddress         — simpler variant of zcl_send
zcl_listaddresses         — all t-addresses in wallet
zcl_dumpprivkey           — export WIF for an address
zcl_importprivkey         — import WIF key
zcl_z_listaddresses       — all z-addresses
zcl_z_listunspent         — shielded UTXOs
zcl_z_getbalance          — single z-address balance
zcl_rescanblockchain      — manual rescan trigger
zcl_walletaudit           — reconcile wallet vs on-chain
zcl_listwalletkeys        — all keys (WIFs + metadata)
zcl_replaywalletfromchain — rebuild wallet from chain
```

**Missing ops tools (6):**
```
zcl_getmempoolinfo  — mempool size/bytes/usage
zcl_getrawmempool   — txids currently in mempool
zcl_getmininginfo   — mining stats
zcl_kpi             — one-call KPI dashboard (height/peers/sync/validation/health/mempool/wallet/chain/network)
zcl_benchmark       — sha256d, malloc, hash160 ops/sec (calls RPC `benchmark`)
zcl_dbstats         — table counts and sizes (calls RPC `db_info`)
```

**Missing chain tool (1):**
```
zcl_getrawtransaction — tx by id
```

Each of these was a thin wrapper calling `node_rpc("method", params)` in the old implementation. Register them through the router with proper parameter schemas. **`zcl_kpi` is the important one** — it's how AGENT1 does everything in one call during debugging. Lay out its JSON shape carefully (see `project_mcp_tools.md` for the field list) and treat it as the flagship operator tool.

After backfill, the total should be 63 tools. Update `AGENT3.md` Current Status with the final count.

### Phase 2b — Controller split (as originally planned)

Once the surface is complete, split handlers out of `tools/mcp_server.c` into files by domain:

```
tools/mcp/
├── router.{h,c}          ← already done
├── controllers/
│   ├── chain_controller.c
│   ├── wallet_controller.c
│   ├── net_controller.c
│   ├── ops_controller.c
│   └── app_controller.c
└── views/
    └── json_formatters.c ← shared (block->json, tx->json, utxo->json)
```

Target: `tools/mcp_server.c` < 200 lines (just the stdio loop + controller registration calls).

### Phase 2c — Operator tooling

Add these three operator tools (they'll pay for themselves within a day):

1. **`zcl_tools_list`** — returns the routing table as JSON (name, domain, description, param schemas). Trivial to implement; makes the whole server self-documenting.
2. **`zcl_self_test`** — iterates the routing table, calls every tool with safe defaults (or skips ones marked `destructive`), reports which succeeded. Mark `zcl_stop`, `zcl_sendtoaddress`, `zcl_send`, `zcl_importprivkey`, `zcl_rescanblockchain`, `zcl_replaywalletfromchain` as destructive so `self_test` skips them.
3. **`zcl_logtail`** — returns the last N lines of the structured event log. Takes a `domain` filter so I can watch just `MCP_REQUEST` events during a debug session.

### Phase 3 — Model validator hooks

Same as the original plan. Do it AFTER Phase 2 is merged so it doesn't interact with AGENT2's chain-state work. Wire `db_register_validator()` into `app/models/src/database.c` and add validators for the 17 models. Keep model validator state out of the router — it's a lower layer.

### Rules of engagement

- **Rebase on `origin/master` before every session.** AGENT2's work is now on master too — you'll pick up chain_state_repository.
- **Every commit must build and `./test_zcl` must pass.** No exceptions.
- **Compat contract stands**: every tool name in `CLAUDE.md` must still exist and accept the same params. If a tool shape must change (e.g., a new required field), add it as optional and document the migration plan in your commit message.
- **Smoke test after every push**: start a dev node, run `zcl_status`, `zcl_kpi`, `zcl_balance`, `zcl_benchmark` — paste the results into your Current Status update so AGENT1 can see they work.
- **When you finish a chunk, push the branch and update this Current Status section.** I'll review and merge.

---

## Appendix — current MCP tool inventory (60+ tools)

See `/home/rhett/.claude/projects/-home-rhett-zclassic23/memory/project_mcp_tools.md` for the live index.

Domain groupings (proposed):

**chain**: zcl_getblock, zcl_getblockcount, zcl_getblockchaininfo, zcl_getrawtransaction, zcl_getrawmempool, zcl_getmempoolinfo, zcl_getmininginfo, zcl_utxocommitment, zcl_mmb, zcl_dataintegrity, zcl_validationstatus, zcl_syncstate, zcl_hodlwave

**wallet**: zcl_balance, zcl_getnewaddress, zcl_z_getnewaddress, zcl_send, zcl_sendtoaddress, zcl_getwalletinfo, zcl_listunspent, zcl_listtransactions, zcl_gettransaction, zcl_listaddresses, zcl_dumpprivkey, zcl_importprivkey, zcl_z_listaddresses, zcl_z_listunspent, zcl_z_getbalance, zcl_rescanblockchain, zcl_walletaudit, zcl_listwalletkeys, zcl_replaywalletfromchain

**net**: zcl_peers, zcl_networkinfo, zcl_addnode, zcl_onion_status, zcl_pingpeer, zcl_peerlatency, zcl_gametypes

**ops**: zcl_status, zcl_health, zcl_kpi, zcl_benchmark, zcl_dbstats, zcl_events, zcl_filemanifest, zcl_rpc

**app**: zcl_name_resolve, zcl_name_register, zcl_name_list, zcl_msg_send, zcl_msg_send_named, zcl_msg_inbox, zcl_msg_read, zcl_tokens, zcl_market_list, zcl_market_offer, zcl_market_buy, zcl_market_status, zcl_swap_chains, zcl_swap_initiate, zcl_swap_participate, zcl_swap_list
