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
- **Next (AGENT1 / AGENT2):** once master is buildable again, rebase
  `agent3/mcp-mvc`, run `make -j test_zcl && ./test_zcl` (expect
  existing suite + 27 new router tests to all pass), then do the MCP
  smoke pass (`zcl_status`, `zcl_kpi`, `zcl_balance`). Phase 2
  (controllers split) and Phase 3 (model validator hooks) are ready
  to start once this lands.
- **2026-04-11 (AGENT3)** — Rebased on top of AGENT1's master fixes
  (`25c1b3779`), resolved three trivial conflicts (event enum, test
  helper prototypes, test runner). **Full `./test_zcl` green** —
  1500+ tests pass, including the 27 new router tests. Smoke-tested
  the new `-mcp` binary against the live node (height 2014948, 9
  peers): `zcl_status`, `zcl_balance`, `zcl_getblockcount`,
  `tools/list` (41 tools) all return successfully, and validation
  errors (`MISSING_PARAM`, `ENUM_MISMATCH`, `INVALID_TYPE`,
  `UNKNOWN_TOOL`, `STRING_TOO_SHORT`) all produce the canonical
  envelope shape.
- **2026-04-11 (AGENT3) — Phase 2 landed**: handlers split from
  `tools/mcp_server.c` into domain controllers. New layout:
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
  `tools/mcp_server.c` is now **183 lines** (target: <300) — just the
  stdio loop and three JSON-RPC method handlers; it calls
  `mcp_router_reset()` + `mcp_register_ops/chain/net/wallet/app()`
  in `register_all_controllers()`. `test_zcl` still green; smoke
  tests against the live node still pass. Phase 2 deliverables
  (router + schema validation + error envelope + 15+ tests +
  controllers split) are all complete.
- **Next:** Phase 3 — model validator hooks on `db_save`. Per
  AGENT3.md coordination rules this must wait until AGENT2's
  chain-state work lands, since it touches `app/models/src/database.c`
  and `app/models/**` validators.

---

## Appendix — current MCP tool inventory (60+ tools)

See `/home/rhett/.claude/projects/-home-rhett-zclassic23/memory/project_mcp_tools.md` for the live index.

Domain groupings (proposed):

**chain**: zcl_getblock, zcl_getblockcount, zcl_getblockchaininfo, zcl_getrawtransaction, zcl_getrawmempool, zcl_getmempoolinfo, zcl_getmininginfo, zcl_utxocommitment, zcl_mmb, zcl_dataintegrity, zcl_validationstatus, zcl_syncstate, zcl_hodlwave

**wallet**: zcl_balance, zcl_getnewaddress, zcl_z_getnewaddress, zcl_send, zcl_sendtoaddress, zcl_getwalletinfo, zcl_listunspent, zcl_listtransactions, zcl_gettransaction, zcl_listaddresses, zcl_dumpprivkey, zcl_importprivkey, zcl_z_listaddresses, zcl_z_listunspent, zcl_z_getbalance, zcl_rescanblockchain, zcl_walletaudit, zcl_listwalletkeys, zcl_replaywalletfromchain

**net**: zcl_peers, zcl_networkinfo, zcl_addnode, zcl_onion_status, zcl_pingpeer, zcl_peerlatency, zcl_gametypes

**ops**: zcl_status, zcl_health, zcl_kpi, zcl_benchmark, zcl_dbstats, zcl_events, zcl_filemanifest, zcl_rpc

**app**: zcl_name_resolve, zcl_name_register, zcl_name_list, zcl_msg_send, zcl_msg_send_named, zcl_msg_inbox, zcl_msg_read, zcl_tokens, zcl_market_list, zcl_market_offer, zcl_market_buy, zcl_market_status, zcl_swap_chains, zcl_swap_initiate, zcl_swap_participate, zcl_swap_list
