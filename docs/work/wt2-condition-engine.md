# wt2 Assignment — Phase 0a: Condition Engine + First 3 Conditions

**Worktree:** `~/github/zclassic23-2`
**Branch:** `wt2/phase0-condition-engine`
**Phase:** 0 (foundation)
**Depends on:** scaffold commit (already in `main` when you start)
**Owns (no other worker may touch):**
- `lib/framework/include/framework/condition.h`  ← canonical include path (matches project convention `lib/<area>/include/<area>/foo.h`, e.g. `lib/util/include/util/mailbox.h`)
- `lib/framework/src/condition.c`
- `app/supervisors/src/self_heal.c`
- `app/supervisors/include/supervisors/self_heal.h`
- `app/conditions/src/block_failed_mask_at_tip.c`
- `app/conditions/src/contradiction_frozen.c`
- `app/conditions/src/chain_stalled_with_data.c`
- `app/conditions/include/conditions/condition_registry.h`
- `tools/mcp/controllers/conditions_controller.c` (new MCP tool: `zcl_conditions`)
- `lib/test/src/test_condition_engine.c` (unit tests)
- Edits to `config/src/boot_services.c` (one block: register self_heal supervisor)
- Edits to `tools/mcp/router.c` (one entry: wire `zcl_conditions`)
- Edits to `app/controllers/src/diagnostics_controller.c` (one entry: add `condition_engine` to `g_dumpers`)
- Edits to `CMakeLists.txt` / `Makefile` as needed for new source files
- Edits to `lib/test/src/test.c` to add `test_condition_engine()` call

**MUST NOT touch:**
- Any file under wt3's assignment scope (see `wt3-framework-shape-lint.md`)
- Any existing `app/services/src/*.c` mega-module
- `docs/REFACTOR_STATUS.md` (orchestrator only)
- `docs/FRAMEWORK.md`
- `CLAUDE.md`

---

## Why this matters

The live node is wedged because `process_block_revalidate` (Wave M)
shipped but is never called from any service. This assignment ships the
**condition engine** — the framework primitive that closes that gap. It
also delivers the first three conditions, the most important of which
(`block_failed_mask_at_tip`) calls `process_block_revalidate` and
unwedges the live node.

This is the proof that the framework pattern works in the most
adversarial environment.

---

## Architecture reference

Read [`docs/FRAMEWORK.md`](../FRAMEWORK.md) § 3.6 (Condition shape) and
§ 7.3 (cookbook for adding a Condition) BEFORE starting.

The Condition contract:

```c
CONDITION("name", SEVERITY = CRITICAL)
    POLL_SECS    = 5;
    BACKOFF_SECS = 30;
    MAX_ATTEMPTS = 5;

    DETECT  { return <predicate>; }
    REMEDY  { return <action() == OK>; }
    WITNESS { return <post-condition true>; }
END_CONDITION
```

The engine polls every condition every `POLL_SECS`. When `DETECT` returns
true, it records the condition as active. It applies `BACKOFF_SECS`
between remedy attempts. After each remedy, it polls `WITNESS` for up
to 60s; if true → cleared. If `MAX_ATTEMPTS` exhausted with no witness →
condition stays active and emits `EV_OPERATOR_NEEDED`.

---

## Tasks (in order)

### Task 1: Implement `lib/framework/include/framework/condition.h`

**Path note:** canonical include path is `lib/framework/include/framework/condition.h`
(included from C as `#include "framework/condition.h"`). The build's
`-I lib/framework/include` flag is added by wt3 in Makefile edits if
not already present. wt3 may also have created a tiny forward-declaration
stub at this exact path — you OVERWRITE it with the full header below.
(If wt3's branch merges first, you'll see the stub; just replace it.
If you merge first, wt3's stub task is moot — coordination handled by
this note.)

Public API. Define:

- `enum condition_severity { COND_INFO, COND_WARN, COND_CRITICAL }`.
- `enum condition_remedy_result { COND_REMEDY_OK, COND_REMEDY_FAILED, COND_REMEDY_SKIP }`.
- `struct condition` — name, severity, poll_secs, backoff_secs, max_attempts, detect fn, remedy fn, witness fn, witness_window_secs (default 60).
- `struct condition_state` (per-condition runtime state) — first_detect_unix, last_remedy_unix, attempts, last_outcome, currently_active, cleared_count.
- `void condition_register(const struct condition *cond);` — adds to registry.
- `void condition_engine_tick(void);` — runs one pass over registry. Called by self_heal supervisor child.
- `bool condition_engine_dump_state_json(struct json_value *out, const char *key);` — for `zcl_state subsystem=condition_engine`.
- `int  condition_engine_get_active_count(void);`
- `int  condition_engine_get_unresolved_count(void);` — conditions that exceeded MAX_ATTEMPTS without witness.

Use atomics for cross-thread reads (condition_state fields). Use a mutex
for the registry list itself.

**Acceptance:** `lib/framework/include/framework/condition.h` compiles standalone
(`echo '#include "framework/condition.h"' | gcc -x c -c - -Ilib/framework/include`).

### Task 2: Implement `lib/framework/src/condition.c`

The engine. Implementation notes:

- Registry: fixed-size array, max 64 conditions (assert if exceeded — fail loud).
- Polling: `condition_engine_tick()` walks registry; for each condition NOT in backoff, calls `detect()`. If true:
  - If `first_detect_unix == 0` → set it; emit `EV_CONDITION_DETECTED`.
  - If `attempts < max_attempts` and time since `last_remedy_unix > backoff_secs`:
    - Run `remedy()`. Set `last_remedy_unix`, increment `attempts`, record `last_outcome`.
    - Emit `EV_CONDITION_REMEDY_ATTEMPTED`.
  - Run `witness()`. If true → mark cleared, increment `cleared_count`, reset state, emit `EV_CONDITION_CLEARED`.
  - If `attempts >= max_attempts` and witness still false → keep active, emit `EV_OPERATOR_NEEDED` (rate-limited, every 60s).
- If `detect()` returns false:
  - If condition was active → run witness one more time; if true → emit `EV_CONDITION_CLEARED`, reset state.
  - Otherwise: ensure state is reset (idempotent).

Add helpful logging at each transition via `LOG_FAIL` / `printf` to
stderr with `[condition_engine]` prefix.

`condition_engine_dump_state_json` returns:
```json
{
  "registered_count": N,
  "active_count": N,
  "unresolved_count": N,
  "conditions": [
    {
      "name": "...",
      "severity": "critical",
      "currently_active": true,
      "first_detect_unix": ...,
      "attempts": ...,
      "last_outcome": "ok|failed|skip",
      "cleared_count": ...,
      "thresholds": { "poll_secs": ..., "backoff_secs": ..., "max_attempts": ... }
    },
    ...
  ]
}
```

**Acceptance:** unit test (Task 7) passes.

### Task 3: `app/supervisors/include/supervisors/self_heal.h` + `app/supervisors/src/self_heal.c`

Registers a supervisor child named `self_heal.engine` with
`period_secs = 5`, on_tick = `condition_engine_tick`, deadline_secs = 0.
Exposes `void self_heal_register(struct main_state *ms);`.

Match the pattern of `app/services/src/chain_tip_watchdog.c` (the just-shipped
single-purpose supervisor child) — atomic supervisor_child_id, idempotent register.

**Acceptance:** compiles, registers cleanly, supervisor sees it tick.

### Task 4: Implement `app/conditions/src/block_failed_mask_at_tip.c`

The condition that unwedges the live node. Pseudocode:

```c
#include "framework/condition.h"
#include "validation/process_block_revalidate.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include "validation/block_index.h"

extern struct main_state g_ms;  // verify the actual symbol name in tree

static bool detect_fn(void) {
    int64_t tip = (int64_t)active_chain_height(&g_ms.chain_active);
    struct block_index *bi = block_index_find_by_height(tip + 1);
    return bi && (bi->nStatus & BLOCK_FAILED_MASK);
}

static enum condition_remedy_result remedy_fn(void) {
    int64_t target = (int64_t)active_chain_height(&g_ms.chain_active) + 1;
    bytes32 out_hash = {0};
    enum reval_result r = process_block_revalidate((int)target, &g_ms, &out_hash);
    return (r == REVAL_OK) ? COND_REMEDY_OK : COND_REMEDY_FAILED;
}

static bool witness_fn(int64_t target_at_detect) {
    return (int64_t)active_chain_height(&g_ms.chain_active) > target_at_detect;
}

static const struct condition c = {
    .name = "block_failed_mask_at_tip",
    .severity = COND_CRITICAL,
    .poll_secs = 5,
    .backoff_secs = 30,
    .max_attempts = 5,
    .detect = detect_fn,
    .remedy = remedy_fn,
    .witness = witness_fn,
    .witness_window_secs = 60,
};

void register_block_failed_mask_at_tip(void) { condition_register(&c); }
```

Verify actual symbols by reading:
- `lib/validation/include/validation/process_block_revalidate.h`
- `lib/validation/include/validation/main_state.h`
- `app/services/src/chain_tip_watchdog.c` (uses the same patterns)

Use the same `LOG_*` macros + `fprintf(stderr, ...)` pattern as elsewhere.

**Acceptance:** compiles. Manual sanity: when block at `tip+1` has
BLOCK_FAILED_MASK, detect returns true.

### Task 5: Implement `app/conditions/src/contradiction_frozen.c`

```
DETECT:  chain_evidence.sync_state == "contradiction_frozen"
REMEDY:  synthesize evidence from active tip (call existing repair path —
         find it via grep in chain_evidence_controller.c — likely
         chain_evidence_repair_active_tip_evidence() or similar)
WITNESS: chain_evidence.sync_state != "contradiction_frozen"
```

Look at `app/services/src/chain_evidence_controller.c` for the existing
repair function name and signature.

**Acceptance:** compiles.

### Task 6: Implement `app/conditions/src/chain_stalled_with_data.c`

```
DETECT:  legacy_mirror.last_error contains "body data available but activation did not advance"
         AND active_tip not advanced in last 60 seconds
REMEDY:  call chain_advance_coordinator_force_mirror_promotion("condition:chain_stalled_with_data")
WITNESS: active_tip advances within 60 seconds
```

For DETECT, you'll need access to legacy_mirror state. Read
`app/services/src/legacy_mirror_sync_service.c` for the accessor (likely
`legacy_mirror_get_last_error()` or similar). Add an accessor if one
doesn't exist (one function, no behavior change).

**Acceptance:** compiles.

### Task 7: Register all 3 conditions + boot wiring

Create `app/conditions/include/conditions/condition_registry.h`:

```c
void condition_registry_register_all(void);
```

And `app/conditions/src/condition_registry.c`:

```c
void condition_registry_register_all(void) {
    register_block_failed_mask_at_tip();
    register_contradiction_frozen();
    register_chain_stalled_with_data();
}
```

In `config/src/boot_services.c`, after the existing `chain_tip_watchdog_register(svc->state);` line, add:

```c
#include "supervisors/self_heal.h"
#include "conditions/condition_registry.h"

// ... in the same registration block:
condition_registry_register_all();
self_heal_register(svc->state);
```

**Acceptance:** node boots with conditions registered; supervisor shows
`self_heal.engine` child.

### Task 8: Wire MCP tool `zcl_conditions`

Add `condition_engine_dump_state_json` to `app/controllers/src/diagnostics_controller.c`'s
`g_dumpers[]` table:

```c
{ "condition_engine", condition_engine_dump_state_json,
    "self-heal engine: registered conditions with active/cleared status, attempts, thresholds" },
```

So `zcl_state subsystem=condition_engine` works.

ALSO add a dedicated tool. Create `tools/mcp/controllers/conditions_controller.c` with
one tool `zcl_conditions` that's a sugar wrapper over the state dump (so
it's discoverable separately). Match pattern of other ops_controller tools.

Wire into `tools/mcp/router.c`.

**Acceptance:** `./zclassic-cli dumpstate condition_engine` returns JSON;
`./zclassic-cli rpc zcl_conditions` (or via MCP) returns the same.

### Task 9: Unit test `lib/test/src/test_condition_engine.c`

Test cases:
- Register condition; tick engine; detect returns true; remedy called.
- Backoff: second tick within backoff_secs does NOT call remedy again.
- Witness: after remedy + witness true → cleared.
- Max attempts: after max_attempts with witness false → stays active, EV_OPERATOR_NEEDED emitted.
- Idempotent registration: registering same name twice is rejected or no-op.

Add `failures += test_condition_engine();` to `lib/test/src/test.c` and
declaration to `lib/test/include/test/test_helpers.h`.

**Acceptance:** `make test_parallel` passes.

### Task 10: Final verification — live unwedge check

After deploying the branch to a test instance (if possible) — verify:

1. `./zclassic-cli dumpstate condition_engine` returns the 3 conditions registered.
2. If the live wedge is still present (block 3121685 with FAILED_MASK), check that condition becomes active within 5s, remedy fires within 30s, witness confirms tip advance.
3. If the live wedge is gone (chain advanced past it), at least verify the condition is registered with `currently_active: false`.

**Acceptance:** condition mechanism is alive on the test node, or
explicitly noted that live verification was deferred to orchestrator.

---

## Commit cadence

One commit per task, in this order. Each commit ends with:
```
Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
```

After commits 3, 6, and 9: `git push origin wt2/phase0-condition-engine` to back up.

---

## Push final

```bash
make test_parallel    # all green
make lint             # all green
git push origin wt2/phase0-condition-engine
```

---

## Status

**READY** — waiting for human to start `claude` in `~/github/zclassic23-2`.

<!-- Worker: append a Completion section below when done, per agent-protocol.md -->
