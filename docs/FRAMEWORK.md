# The zclassic23 Framework

> **Repeatable pattern, observable by construction, LLM-legible.**
>
> Every piece of code is **exactly one of eight shapes**. You open a file, you
> know what's in it from the path. You write new code by copying a template.
> Failures are typed, observability is structural, auto-healing is the
> framework's job.

This is the canonical architecture doc. If you are a contributor (human or
AI), **read this before writing or moving any code**. Status board:
[`docs/REFACTOR_STATUS.md`](./REFACTOR_STATUS.md).

---

## 1. The mental model — two pipelines, eight shapes

Every operation in zclassic23 is one of:

```
SYNCHRONOUS (request/response)        ASYNCHRONOUS (cursor advance)

  external request                      cursor tick
        ↓                                   ↓
  Controller    (auth, parse, log)      Job        (idempotent, cursor-stamped)
        ↓                                   ↓
  Service       (orchestrate)           Service
        ↓                                   ↓
  Model         (validate, persist)     Model
        ↓                                   ↓
  Storage Adapter                       Storage Adapter
        ↓                                   ↓
  Event         (typed broadcast)       Event

                Supervisor watches every Job
                Condition watches every problem, dispatches remedy
                Projection serves every read
                Mailbox carries every inter-actor message
```

The eight shapes:

| # | Shape | What it does | Folder |
|---|---|---|---|
| 1 | **Controller** | Entry point — parse, authorize, log, delegate | `app/controllers/{mcp,rpc,p2p,explorer}/` |
| 2 | **Service** | Orchestrate a multi-step workflow | `app/services/{chain,sync,wallet,...}/` |
| 3 | **Model** | Business entity + persistence via ActiveRecord | `app/models/` |
| 4 | **Job** | Idempotent cursor-stamped async stage | `app/jobs/` |
| 5 | **Supervisor** | Liveness contract registry — restart policy | `app/supervisors/` |
| 6 | **Condition** | `(detect, remedy, witness)` — auto-healer | `app/conditions/` |
| 7 | **Event** | Typed message emitted on state change | `app/events/` |
| 8 | **Storage Adapter** | Hexagonal port to SQLite / flat file / network | `lib/adapters/` |

Plus two read-side helpers used by all shapes: **Projection** (MVCC read
view) and **Mailbox** (typed bounded queue for actor handoff).

---

## 2. Folder layout (the convention)

```
app/                       # the application — composition of framework shapes
  controllers/             # one folder per protocol surface
    mcp/                   #   MCP server (Claude-facing)
    rpc/                   #   JSON-RPC + CLI
    p2p/                   #   inbound peer message handlers
    explorer/              #   .onion HTTP UI
  services/                # multi-step workflows — one folder per domain
    chain/                 #   block validation pipeline
    sync/                  #   multi-source catchup
    wallet/                #   send / receive / balance
    market/                #   ZCL Market
    swap/                  #   atomic swaps
    msg/                   #   ZMSG messaging
    self_heal/             #   condition orchestration glue
  models/                  # business entities (AR lifecycle, before/after save hooks)
  jobs/                    # async cursor-stamped stages (Wave S target)
  supervisors/             # liveness trees — one root per domain
  conditions/              # auto-healers — one file per wedge class
  events/                  # typed event definitions
  views/                   # explorer page templates

lib/                       # the framework + crypto + protocol primitives
  framework/               # the eight shape primitives — load-bearing
    controller.h           #   REQUEST_BEGIN/END, PARAM_*, AUTHORIZE macros
    service.h              #   SERVICE_BEGIN/END, REQUIRE, Result macros
    model.h                #   MODEL/ATTR/QUERY/BEFORE_SAVE — AR with hooks
    job.h                  #   JOB(name, period_secs) — cursor-stamped stage
    supervisor.h           #   liveness contract registry
    condition.h            #   CONDITION(name) DETECT/REMEDY/WITNESS
    event_log.h            #   typed append-only event emission
    projection.h           #   MVCC read view
    mailbox.h              #   bounded MPSC queue
  domain/                  # pure consensus core (hexagonal "inside")
    consensus/             #   chain rules, validation predicates
    utxo/                  #   UTXO arithmetic
    crypto/                #   versioned algorithm registry (Phase 5)
  adapters/                # hexagonal "outside" — swappable per platform
    sqlite/  flatfile/  tor/  p2p_proto/  mcp_transport/  rpc_transport/
  net/  storage/  rpc/  …  # existing low-level primitives (incremental migration)

tools/lint/                # the ratcheting gates
  framework_shape_check.sh # enforces "every file is one of the 8 shapes"
  (...17 existing gates...)

docs/
  FRAMEWORK.md             # this doc — load-bearing
  REFACTOR_STATUS.md       # status board, updated every PR
  work/                    # per-worktree assignment specs
```

**Rule:** every new `.c` file under `app/` lives in one of the eight
folders. The lint gate enforces this from Phase 0 onward (warn mode → fail
mode by Phase 2).

---

## 3. The eight shapes — what each looks like

### 3.1 Controller — entry point

```c
// app/controllers/mcp/block_controller.c
#include "framework/controller.h"

MCP_TOOL("zcl_getblock", "Block by height or hash") {
    REQUEST_BEGIN(req);                       // structured log: request_id, ts
    PARAM_INT_REQUIRED(req, "height");        // typed parse + validate
    PARAM_INT_DEFAULT(req, "verbose", 0);
    AUTHORIZE(req, "block:read");             // permission gate (no-op for public)

    Result r = chain_service_get_block(req->height, req->verbose);

    RESPOND(req, r);                          // structured response; auto-formats errors
    REQUEST_END(req);                         // log duration_us, result_code
}
```

**Contract:** parse input → authorize → call ONE service → return.
Controllers do not contain business logic. They do not touch storage. They
do not catch their own errors. They are dumb glue.

### 3.2 Service — orchestrate

```c
// app/services/chain/block_service.c
#include "framework/service.h"

Result chain_service_get_block(int64_t height, int verbose) {
    SERVICE_BEGIN("chain.get_block");         // span_id, parent_request_id
    LOG_PARAM(height);

    Block *b = block_model_find_by_height(height);
    REQUIRE(b, NOT_FOUND, "no block at height");

    Result r = verbose
             ? result_ok_json(block_to_json_verbose(b))
             : result_ok_str(block_to_hex(b));

    SERVICE_END(r);
    return r;
}
```

**Contract:** receives typed inputs, calls models + other services,
returns a typed `Result`. Services may call other services (composition).
Services do not parse input (that's controllers). Services do not touch
storage directly (that's models).

### 3.3 Model — business entity + persistence

```c
// app/models/block.c
#include "framework/model.h"

MODEL(block, "blocks")                        // table name
    ATTR(int64_t,  height,       NOT_NULL)
    ATTR(bytes32,  hash,         NOT_NULL UNIQUE)
    ATTR(int64_t,  time,         NOT_NULL)
    ATTR(int32_t,  status,       DEFAULT 0)

    INDEX(hash)
    INDEX(height)

    BEFORE_SAVE(block_verify_chainwork)       // hook
    AFTER_SAVE(block_emit_observed_event)     // hook → event

    QUERY(find_by_height,
          "SELECT * FROM blocks WHERE height = ?", int64_t height)
    QUERY(find_by_hash,
          "SELECT * FROM blocks WHERE hash    = ?", bytes32 hash)
END_MODEL
```

**Contract:** wraps the existing `AR_*` AR lifecycle. Hooks fire on every
save. Queries are typed. Models are the ONLY way to read or write
persistent state.

### 3.4 Job — async idempotent stage

```c
// app/jobs/body_fetch.c
#include "framework/job.h"

JOB("body_fetch", PERIOD_SECS = 2) {
    int64_t cursor = job_cursor_get("body_fetch");
    Block *next = block_model_find_by_height(cursor + 1);
    if (!next) return JOB_IDLE;

    if (!peer_service_fetch_body(next)) return JOB_RETRY;

    job_cursor_advance("body_fetch", next->height);
    event_emit(EV_BLOCK_BODY_OBSERVED, next);
    return JOB_PROGRESS;
}
```

**Contract:** cursor-stamped in `progress.kv`, idempotent under same
input (re-running with same cursor is a no-op). Returns one of `JOB_IDLE`
(nothing to do), `JOB_PROGRESS` (made progress), `JOB_RETRY` (transient
failure, will retry next tick), `JOB_FATAL` (give up, escalate).

Jobs are the Wave S stages, generalized.

### 3.5 Supervisor — liveness tree

```c
// app/supervisors/chain.c
#include "framework/supervisor.h"

SUPERVISOR_ROOT("chain") {
    SUPERVISE_JOB("header_admit",      RESTART = TRANSIENT);
    SUPERVISE_JOB("validate_headers",  RESTART = TRANSIENT);
    SUPERVISE_JOB("body_fetch",        RESTART = TRANSIENT);
    SUPERVISE_JOB("body_persist",      RESTART = TRANSIENT);
    SUPERVISE_JOB("script_validate",   RESTART = TRANSIENT);
    SUPERVISE_JOB("proof_validate",    RESTART = TRANSIENT);
    SUPERVISE_JOB("utxo_apply",        RESTART = PERMANENT);
    SUPERVISE_JOB("tip_finalize",      RESTART = PERMANENT);
}
```

**Contract:** declares a tree of supervised children. Each child has a
restart policy (TRANSIENT = restart on stall, PERMANENT = always restart,
TEMPORARY = never restart). The supervisor framework polls liveness and
applies the policy. Erlang/OTP "one for one" semantics.

### 3.6 Condition — auto-healer

```c
// app/conditions/block_failed_mask_at_tip.c
#include "framework/condition.h"

CONDITION("block_failed_mask_at_tip", SEVERITY = CRITICAL)
    POLL_SECS    = 5;
    BACKOFF_SECS = 30;
    MAX_ATTEMPTS = 5;        // then page operator

    DETECT {
        int64_t next = active_tip_height() + 1;
        Block *b = block_model_find_by_height(next);
        return b && (b->status & BLOCK_FAILED_MASK);
    }

    REMEDY {
        int64_t next = active_tip_height() + 1;
        bytes32 out_hash;
        return process_block_revalidate(next, &g_ms, &out_hash) == REVAL_OK;
    }

    WITNESS {
        return active_tip_height() > evidence.target_height_at_detect;
    }
END_CONDITION
```

**Contract:** registered with the condition engine at boot. Engine polls
every `POLL_SECS`. When `DETECT` returns true, engine records a typed
ledger entry. Engine applies `BACKOFF_SECS` between remedy attempts; runs
`REMEDY` up to `MAX_ATTEMPTS`. After each attempt, runs `WITNESS` for a
configurable window (default 60s); if witness returns true → condition
cleared, victory logged. If `MAX_ATTEMPTS` exhausted with `WITNESS = false`
→ condition stays active, operator paged via `EV_OPERATOR_NEEDED`.

**Every wedge class becomes one of these files.** Adding a new
auto-healing rule is ~50 LOC.

### 3.7 Event — typed broadcast

```c
// app/events/chain_events.c
#include "framework/event_log.h"

EVENT(BLOCK_BODY_OBSERVED,
      ARG(int64_t, height),
      ARG(bytes32, hash),
      ARG(int64_t, size_bytes))

EVENT(BLOCK_ACTIVATED,
      ARG(int64_t, height),
      ARG(bytes32, hash),
      ARG(int64_t, prior_tip))

EVENT(BLOCK_ORPHANED,
      ARG(int64_t, height),
      ARG(bytes32, hash),
      ARG(const char *, reason))
```

**Contract:** events are typed, append-only, totally ordered. Emitting
an event writes to the append-only event log (Phase 4 unifies storage
around this). Subscribers (observers, MCP tail, audit log) receive events
asynchronously.

### 3.8 Storage Adapter — hexagonal port

```c
// lib/adapters/sqlite/utxo_adapter.c
#include "framework/storage_adapter.h"

STORAGE_ADAPTER(utxo, "sqlite")
    OPEN  (utxo_sqlite_open)
    CLOSE (utxo_sqlite_close)
    PUT   (utxo_sqlite_put)
    GET   (utxo_sqlite_get)
    DELETE(utxo_sqlite_delete)
    SCAN  (utxo_sqlite_scan)
END_STORAGE_ADAPTER
```

**Contract:** the domain depends on the *port* (interface). Multiple
adapters can satisfy the same port (sqlite, lsm, in-memory). Swapping
storage engines = swapping one file. Phase 4 unifies storage around an
append-only log adapter.

---

## 4. Observability is built into the framework

Every Controller / Service / Job emits a span automatically:

```
{request_id, span_id, parent_span_id, layer, action, duration_us, result_code, args, error}
```

Spans thread through automatically via thread-local context. One MCP
call: `zcl_trace request_id=abc123` returns the full tree:

```
abc123 [Controller] zcl_getblock              12ms  OK
  abc124 [Service]    chain.get_block         11ms  OK
    abc125 [Model]      block.find_by_height   2ms  OK
    abc126 [Model]      block.to_json_verbose  8ms  OK
```

**You never grep `dump_state` JSONs again.** Every operation is a trace.

---

## 5. Hexagonal cut — what's inside vs outside

```
                      ┌─────────────────────┐
                      │      DOMAIN         │
                      │  lib/domain/        │
                      │  - consensus rules  │
                      │  - validation       │
                      │  - UTXO arithmetic  │
                      │  - crypto registry  │
                      └──────────┬──────────┘
                                 │ depends on
                  ┌──────────────┼──────────────┐
                  │              │              │
            ┌─────▼─────┐  ┌─────▼─────┐  ┌─────▼─────┐
            │  PORTS    │  │  PORTS    │  │  PORTS    │
            │ (lib/     │  │ (lib/     │  │ (lib/     │
            │ framework)│  │ framework)│  │ framework)│
            └─────┬─────┘  └─────┬─────┘  └─────┬─────┘
                  │              │              │
       ┌──────────┼──────────────┼──────────────┼──────────┐
       │          │              │              │          │
   ┌───▼──┐   ┌───▼──┐       ┌───▼──┐       ┌───▼──┐   ┌───▼──┐
   │SQLite│   │Flat- │       │ Tor  │       │ MCP  │   │ P2P  │
   │      │   │file  │       │      │       │      │   │      │
   └──────┘   └──────┘       └──────┘       └──────┘   └──────┘
              ADAPTERS  ─  lib/adapters/  ─  ADAPTERS
```

**Dependency rule:** inward only. Controllers depend on Services. Services
depend on Models. Models depend on Storage Adapters. **Storage Adapters
depend on Domain, NEVER the other way.** Domain is pure — no I/O, no
clock, no RNG.

This is what makes the system 50-year-replaceable: swap any adapter, the
domain doesn't move.

---

## 6. The composition rule (lint-enforced)

`make lint` fails if any of (ratcheting per phase — see status board):

1. **Folder shape**: every `.c` file under `app/` lives in one of the
   eight folders. No exceptions.
2. **Size cap**: no `.c` file under `app/` exceeds 800 LOC. (Current
   violators allowlisted; cap ratchets down per phase.)
3. **Read discipline**: no raw `sqlite3_prepare_v2` outside
   `lib/adapters/` or models.
4. **Platform discipline**: no raw `clock_gettime` or `getrandom`
   outside `lib/platform/`.
5. **Shape macros**: every file in `app/conditions/` uses
   `CONDITION(...)`; every file in `app/jobs/` uses `JOB(...)`; etc.
6. **Hexagonal direction**: `lib/domain/` has no `#include` from
   `lib/adapters/` or `app/`.

Gates start as **WARN** in Phase 0 (count violations, don't break
build), tighten to **FAIL** as each phase's violations are fixed.

---

## 7. Cookbook — adding a new operation

### 7.1 Add an MCP tool that reads chain state

1. Create `app/controllers/mcp/<thing>_controller.c` with `MCP_TOOL(...)`.
2. Create `app/services/<domain>/<thing>_service.c` if no service exists.
3. Add a `QUERY(...)` to the relevant model in `app/models/`.
4. Wire the tool registration in `tools/mcp/controllers/<domain>_controller.c`.
5. Test: `./zclassic-cli <thing>` works; spans appear in `zcl_trace`.

### 7.2 Add an async stage to the sync pipeline

1. Create `app/jobs/<stage>.c` with `JOB(...)`.
2. Add cursor to `progress.kv` schema if new.
3. Register in `app/supervisors/chain.c`.
4. Test: stage runs, cursor advances, idempotent under restart.

### 7.3 Add an auto-healer for a new wedge class

1. Create `app/conditions/<wedge_name>.c` with `CONDITION(...)`.
2. Implement `DETECT` (reads model state).
3. Implement `REMEDY` (calls existing service/model APIs).
4. Implement `WITNESS` (observable post-condition).
5. Set `POLL_SECS`, `BACKOFF_SECS`, `MAX_ATTEMPTS`.
6. Test: induce wedge in test, verify condition fires + heals.
7. **No need to add an escalator anywhere else.** The engine takes
   care of dispatch, backoff, witness, paging.

### 7.4 Add a new business entity

1. Create `app/models/<entity>.c` with `MODEL(...)`.
2. Add migration (auto-generated from `ATTR` + `INDEX`).
3. Add `BEFORE_SAVE` / `AFTER_SAVE` hooks if needed.
4. Add `QUERY(...)` for each access pattern.
5. Reference in services.

### 7.5 Replace a storage backend

1. Create new adapter under `lib/adapters/<new_engine>/<entity>_adapter.c`
   with `STORAGE_ADAPTER(...)`.
2. Verify it satisfies the same port (compile time check).
3. Switch the model's adapter binding (one line in `app/models/<entity>.c`).
4. Run conformance test suite against the new adapter.

---

## 8. The roadmap — phase summary

Full per-phase detail in [`docs/REFACTOR_STATUS.md`](./REFACTOR_STATUS.md).

| Phase | Topic | Ships |
|---|---|---|
| **0** | Condition engine + scaffold | `lib/framework/condition.{c,h}`, first 3 conditions, folder scaffold, lint gate (warn mode), status board. **UNWEDGES LIVE NODE.** |
| **1** | Adopt the four unused primitives | Wire `mailbox`, `projection`, `platform.clock`, `platform.rng` into real call sites. Lint gates ratchet to fail mode. |
| **2** | Wave S → S-12 cutover | Land S-5..S-9 (body_persist, script_validate, proof_validate, utxo_apply, tip_finalize). Delete `chain_advance_coordinator.c`, `legacy_mirror_sync_service.c`, `sync_watchdog_service.c`, etc. **~7,000 LOC deletion.** |
| **3** | Dissolve remaining mega-modules | `chain_restore_service.c`, `header_probe.c`, `utxo_recovery_service.c` decompose into jobs + conditions. |
| **4** | Storage unification | One append-only event log + N projections. Replaces 5 storage layers. **Generational win.** |
| **5** | Crypto agility + reproducible builds | Versioned crypto registry; `flake.nix`; cosign + Rekor. |
| **6** | Determinism + simulator | Replay any bug from 64-bit seed. Continuous chaos in CI. |
| **7** | Frontier (optional) | io_uring; structured concurrency; hot reload. |

Estimated total: ~30 sessions, ~4-5 months at current cadence.

---

## 9. What survives 50 years vs what gets rewritten

**Frozen (the spec):**

- The eight shapes and their contracts
- The lint gates (the ratchet)
- The folder layout
- The typed event log schema (with version stamps)
- Chain consensus rules (chain-encoded)

**Replaceable (the implementation):**

- C23 → next language
- SQLite → next storage engine
- systemd → next supervisor
- Tor v3 → next onion routing
- Specific crypto algorithms (with crypto-agility ladder)

The framework holds; the implementation moves.

---

## 10. Glossary

- **Shape** — one of the eight allowed kinds of code (Controller, Service, Model, Job, Supervisor, Condition, Event, Storage Adapter).
- **AR (ActiveRecord)** — the existing model lifecycle pattern (`AR_BEGIN_SAVE` / `AR_FINISH_SAVE`, before/after save hooks).
- **Cursor** — durable position marker in `progress.kv` that a Job advances; enables crash-safe idempotent replay.
- **Witness** — observable post-condition that confirms a Condition's Remedy worked.
- **Span** — structured log entry with `(request_id, span_id, parent_span_id, layer, action, duration_us, result_code)`. Forms a tree per request.
- **Conformance** — degree to which the codebase follows the framework. Measured by `tools/lint/framework_shape_check.sh`. Goes up each PR.
- **Ratchet** — lint gates that tighten over time. Once on, never off.
- **Strangler** — execution model where new code lands in the new shape; old code moves only when its replacement is verified. Vs. big-bang rewrite.

---

## 11. Where to start (per role)

**Reading this for the first time:** also read
[`docs/REFACTOR_STATUS.md`](./REFACTOR_STATUS.md) (current phase) and
[`docs/USER_BENCHMARKS.md`](./USER_BENCHMARKS.md) (the five user-facing
acceptance numbers + operator-paging clause that everything is judged
against).

**Implementing a worker assignment:** read
[`docs/work/agent-protocol.md`](./work/agent-protocol.md), then your
assignment under `docs/work/`.

**Reviewing a PR:** check that every changed file matches its folder's
shape; check that lint passes; check that the status board is updated.

**Adding a new feature/healing/job:** use Section 7 (cookbook).

**Confused:** open the file's folder. The folder name tells you what
shape the file is. The shape tells you what's allowed. There is no
gray area.
