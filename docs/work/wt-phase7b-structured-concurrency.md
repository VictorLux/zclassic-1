# Worker Assignment — Phase 7b: Structured concurrency (scope_t)

**Worktree:** wt2 OR wt3 (either)
**Branch:** push DIRECT TO MAIN — no PR
**Phase:** 7 (Frontier — OPTIONAL)
**Depends on:** Phase 3 supervisor tree split SHIPPED ✅ (`dae31dee9`); jobs
adoption SHIPPED ✅. The scope primitive composes ABOVE the supervisor (a
scope groups tasks that share a lifetime).
**Status: DRAFT — DEFER.** Do NOT dispatch until user explicitly approves.

**Owns:**
- NEW `lib/util/include/util/scope.h` + `lib/util/src/scope.c` — the scope
  primitive (open / spawn / close / cancel).
- NEW `lib/test/src/test_scope.c` — 8 test cases covering open/close,
  spawn+wait, cancel, leak detection.
- EDIT `lib/util/include/util/job.h` + `lib/util/src/job.c` — jobs are spawned
  inside an implicit "supervisor scope"; scope_close blocks until the job's
  next idle quanta. Job API stays compatible.
- INCREMENTAL EDIT (one PR per call site) of the ~30 `pthread_create` callers
  to spawn into a scope instead. THIS SPEC ONLY COVERS THE PRIMITIVE + JOB
  INTEGRATION; per-call-site rewrite is a follow-on assignment per subsystem.
- EDIT `tools/lint/check_no_raw_pthread_create.sh` — new lint gate (WARN
  mode) that flags `pthread_create` outside `lib/platform/` / `lib/util/`.

**MUST NOT touch:**
- `lib/supervisor/` (supervisor primitive is below scopes; do not invert).
- Any concrete pthread_create callsite outside `lib/util/src/workpool.c`
  (do that in follow-on PRs).
- `docs/REFACTOR_STATUS.md`, `docs/FRAMEWORK.md`, `CLAUDE.md`.
- Wave S, Phase 4 projections, Phase 5/6 work.

---

## Why this matters

Today the codebase has ~30 `pthread_create` callsites + ad-hoc atomic-flag
shutdown patterns. Each handler implements its own lifecycle, so:
- shutdown ordering is implicit and fragile,
- thread leaks slip in (test_zcl/parallel catches some but not all),
- crash logs are noisy because dying threads don't get cancelled cleanly.

Structured concurrency removes the entire class. A scope owns its spawned
tasks; the scope can't return until all tasks complete or are cancelled.
You cannot leak a thread by construction.

The MCP tool `zcl_state subsystem=scopes` lists every active scope + its
spawned tasks + their last-progress timestamp. Diagnostics become a SELECT
query rather than a guess.

**This only matters if leak-bugs become a regression source.** Today the
supervisor tree catches stalls before they matter. So this is DEFER until
the user pulls the trigger.

---

## Design

### Primitive

```c
/* lib/util/include/util/scope.h */

typedef struct scope scope_t;

/* Open a scope with a human label (logged + reported via zcl_state). */
scope_t *scope_open(const char *label);

/* Spawn a task into this scope. Returns 0 on success.
 * The fn receives (void *arg, scope_t *self) so it can spawn nested tasks. */
int scope_spawn(scope_t *s, void (*fn)(void *arg, scope_t *self), void *arg);

/* Block until all spawned tasks complete or scope is cancelled.
 * After scope_close returns, the scope_t* is invalid (do not reuse). */
void scope_close(scope_t *s);

/* Request cancellation. Cooperative — tasks must poll. */
void scope_cancel(scope_t *s);
bool scope_cancel_requested(const scope_t *s);

/* Introspection (for zcl_state subsystem=scopes). */
bool scope_dump_state_json(struct json_value *out, const char *key);
```

### Implementation

- A scope has: label, atomic counter of pending tasks, condvar for "all
  done", atomic cancel-requested flag, parent pointer (for nesting),
  intrusive list of children (for `zcl_state`).
- Each spawned task runs on the global workpool (`lib/util/src/workpool.c`)
  — scopes do NOT own threads. They own *task slots*. This is the same
  pattern Erlang processes use: lightweight ownership without heavyweight
  OS threads.
- `scope_close` waits on the condvar with a deadline; if the deadline
  expires, escalate to abort + dump (which jumps into the postmortem
  capsule path from Phase 6b — neat composition).

### Integration with Jobs

A Job (Phase 1 primitive) currently registers a periodic callback with the
supervisor. With scopes, each Job runs *inside* an implicit scope (the
scope of its supervisor). When the supervisor's scope_close fires, all
its child jobs receive scope_cancel_requested = true on their next tick.

This is mechanical: the Job framework's scheduler queries
`scope_cancel_requested(self)` before each tick.

---

## Tasks (in order)

### Task 1: Primitive header + impl

NEW `lib/util/include/util/scope.h` + `lib/util/src/scope.c`. Single
self-contained file. No dependencies on supervisor or job.

**Acceptance:** compiles standalone; no unresolved symbols.

### Task 2: Unit tests

NEW `lib/test/src/test_scope.c` — 8 test cases:
1. open + close (no tasks) — should be instantaneous.
2. spawn 1 task that returns immediately — close blocks until it finishes.
3. spawn 100 tasks, scope_close waits for all.
4. nested scope: parent waits on child, child waits on grandchild.
5. cancel: spawn 10 long-running tasks, call scope_cancel, all tasks see
   `scope_cancel_requested() == true` on next poll.
6. deadline: scope_close with deadline; aborts cleanly if exceeded.
7. label introspection: zcl_state subsystem=scopes returns the label.
8. leak detection: scope_close called without joining spawned tasks
   should LOG_FAIL + return.

**Acceptance:** all 8 pass via `./test_parallel --jobs=$(nproc)`.

### Task 3: zcl_state integration

EDIT `app/controllers/src/diagnostics_controller.c` — register
`scope_dump_state_json` under key `"scopes"`.

EDIT `tools/mcp/controllers/ops_controller.c` — add `"scopes"` to the
`zcl_state` enum_csv.

EDIT `lib/test/src/test_mcp_controllers.c` — bump the expected enum_csv
list.

**Acceptance:** `zcl_state subsystem=scopes` returns a JSON object listing
the (likely empty) set of currently-open scopes.

### Task 4: Job integration

EDIT `lib/util/src/job.c` — scheduler queries the job's parent scope (set
at registration time) for cancellation. Job API stays compatible (existing
jobs work without changes).

**Acceptance:** existing job tests pass; new test:
`test_job_scope_cancel.c` spawns a periodic job, calls scope_cancel,
verifies the job stops at its next tick.

### Task 5: Lint gate (WARN mode)

NEW `tools/lint/check_no_raw_pthread_create.sh` — Gate #22. Greps for
`pthread_create(` outside `lib/platform/`, `lib/util/src/workpool.c`,
`lib/util/src/scope.c`. WARN mode initially.

EDIT `Makefile` to wire it into `make lint`.

EDIT `DEFENSIVE_CODING.md` — document Gate #22.

**Acceptance:** lint runs; reports current count (~30 violations).

### Task 6: First migration (workpool internals)

The first callsite to convert is `lib/util/src/workpool.c`'s own thread
spawning — it stays as raw pthread (with `// scope-ok: workpool primitive`
comment to silence the lint) because workpool sits below scope.

Document this in the lint gate's exception list.

### Task 7: Verify + push

```bash
make -j$(nproc)
make lint
./test_parallel --jobs=$(nproc)
git pull --rebase origin main
git push origin main
```

Append Completion section.

---

## Follow-on PRs (NOT in this assignment)

Each subsystem gets its own per-PR conversion:
- 7b-1: net pthread → scope (peer manager + outbound floor)
- 7b-2: chain pthread → scope (sync watchdog, body fetch)
- 7b-3: rpc pthread → scope (HTTP listener pool)
- 7b-4: tor pthread → scope (onion bootstrap)
- 7b-5: validation pthread → scope (script + proof workers)

Each is a small (~50-200 LOC) edit and rides the scope primitive shipped
by this PR.

When all migrations land, flip Gate #22 from WARN → FAIL.

---

## What this does NOT do

- Does NOT remove pthread_create call sites (deferred per-subsystem).
- Does NOT change the supervisor primitive (scopes compose above, not below).
- Does NOT replace the workpool (scopes use the workpool).

---

## Commit cadence

One commit per task. Push after Task 5.

---

## Status

**DRAFT — DEFER.** Do NOT claim until user explicitly approves Phase 7
dispatch.
