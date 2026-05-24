# Worker Assignment — Phase 7c: Hot reload (stage dispatcher + .so swap)

**Worktree:** wt2 OR wt3 (either)
**Branch:** push DIRECT TO MAIN — no PR
**Phase:** 7 (Frontier — OPTIONAL)
**Depends on:**
- Phase 2 Wave S cutover COMPLETE (every stage authoritative; legacy paths
  deleted). Hot reload makes no sense if the stages don't own their cursors.
- Phase 4 storage unification COMPLETE (event_log is the only mutable
  hot-path store).
**Status: DRAFT — DEFER.** This is the highest-risk Phase 7 sub-phase. It is
mostly a development convenience (try a new validation strategy without
restarting the node). Production deployment is NOT expected.

**Owns:**
- NEW `lib/framework/include/framework/stage_loader.h` + `.c` — the
  dispatcher wrapper.
- NEW Makefile targets for per-stage `.so` builds (one per Wave S stage).
- EDIT each Wave S stage (S-1..S-9) — wrap its exported entrypoint in a
  function-pointer dispatcher (~20 LOC each).
- NEW MCP tool `zcl_stage_reload(stage_name, so_path)` — atomically swap.
- NEW `tools/lint/check_stage_abi_freeze.sh` — Gate #23 (WARN mode).
- NEW `lib/test/src/test_stage_reload.c` — 6 test cases.

**MUST NOT touch:**
- Stage cursor files on disk (those are the safety net).
- `lib/storage/src/event_log.{c,h}` (frozen).
- `docs/REFACTOR_STATUS.md`, `docs/FRAMEWORK.md`, `CLAUDE.md`.

---

## Why this matters

Once stages own their cursors on disk (post Wave S cutover), the only
thing keeping a stage's behavior is the in-memory function pointer to its
implementation. Hot reload replaces that pointer atomically without
restarting the node.

Use cases:
1. **Development:** try a new script validation algorithm; if it diverges,
   revert with another reload — no restart, no IBD replay.
2. **Bug-fix hot-patch:** a bug in a non-consensus stage (header probe,
   peer scoring) can be fixed via reload without disrupting the chain
   validation pipeline.
3. **A/B testing:** load two implementations of the same stage, route
   traffic via a coin flip, compare outputs (this is the dirtiest use
   case and is OUT OF SCOPE for 7c — would need Phase 8).

**This is dangerous.** A bad reload mid-block-validation could leave the
chain in an inconsistent state. Mitigations:
1. The dispatcher acquires a per-stage write-lock during reload; in-flight
   ticks finish first.
2. Stages with ON-DISK cursors are safe; stages with in-memory-only state
   are EXCLUDED from hot reload (registered in an allowlist).
3. Reload is admin-gated (`zcl_admin` privilege required).
4. Reloads are logged to a tamper-evident audit log + included in the
   postmortem capsule.

---

## Design

### Per-stage dispatcher

Each stage exports:

```c
/* lib/stages/include/stages/header_admit_stage.h */
typedef struct header_admit_stage header_admit_stage_t;

/* Exported entrypoint — fixed signature, ABI frozen. */
typedef int (*header_admit_tick_fn)(header_admit_stage_t *self);

/* The dispatcher holds the current implementation. Swappable. */
extern _Atomic(header_admit_tick_fn) header_admit_tick_dispatch;

/* Reload entry — called by stage_loader after dlopen(). */
int header_admit_stage_install(header_admit_tick_fn new_fn);
```

The `_Atomic` swap is the entire mechanism. dlopen() loads the .so;
extract the new function pointer; atomic store into
`header_admit_tick_dispatch`. Existing in-flight ticks finish on the old
implementation (they hold a borrowed pointer); new ticks pick up the new
implementation.

### Cursor invariant

Every stage must have its cursor in `progress.kv` (Wave S S-1). The
dispatcher's contract:
1. Before dispatching, read the cursor.
2. Tick.
3. Write the new cursor before returning.
4. If the new implementation reads a different cursor format, it MUST
   parse the old format and convert in its first tick.

Stages that don't follow this contract are listed in
`tools/lint/stage_reload_excluded.txt` and `zcl_stage_reload` refuses
them.

### ABI freeze

Hot reload requires the stage's exported entrypoint signature + struct
layout to stay stable across reloads. Gate #23 (WARN mode) greps the
stage headers for `_PUBLIC_ABI` markers and reports if a public symbol
was changed.

---

## Tasks (in order)

### Task 1: Loader primitive

NEW `lib/framework/include/framework/stage_loader.h`:

```c
typedef struct stage_loader stage_loader_t;

stage_loader_t *stage_loader_open(const char *base_so_path);

/* dlopen the new .so; extract the entrypoint named `symbol`; call
 * `install_fn(new_entrypoint)` atomically. Returns 0 on success. */
int stage_loader_reload(stage_loader_t *l, const char *so_path,
                        const char *symbol,
                        int (*install_fn)(void *new_fn));

void stage_loader_close(stage_loader_t *l);

/* Introspection. */
bool stage_loader_dump_state_json(struct json_value *out, const char *key);
```

NEW `lib/framework/src/stage_loader.c` — implements dlopen + dlsym + the
install callback pattern. Keeps a list of loaded .so handles (dlclose on
shutdown). Atomic operations come from the install_fn the stage provides.

**Acceptance:** unit test loads a trivial test .so, reloads it twice,
unloads. No leaks under valgrind.

### Task 2: First stage conversion (header_admit)

EDIT `lib/sync_stages/src/header_admit_stage.c`:
- Define `_Atomic(header_admit_tick_fn) header_admit_tick_dispatch =
  &header_admit_tick_default`.
- Replace the direct call in the tick path with a load + invoke of the
  dispatch pointer.
- Add `header_admit_stage_install(header_admit_tick_fn new_fn)` — atomic
  store + log.

ADD Makefile target `lib/sync_stages/libheader_admit.so` (PIC build of
the stage + minimal deps).

**Acceptance:** stage behaves identically to today via the default
pointer; .so target builds.

### Task 3: MCP tool

NEW handler in `tools/mcp/controllers/ops_controller.c`:

```c
{ "zcl_stage_reload", "ops",
  "Phase 7c: atomically swap a stage's implementation by loading a "
  ".so and pointing the dispatcher at its entrypoint. ADMIN-GATED. "
  "Stage must be in the allowlist (cursor-on-disk only). Logs the "
  "reload to the audit trail.",
  ...args..., h_zcl_stage_reload, 1 /*destructive*/, "admin" },
```

The handler:
1. Verifies admin token.
2. Looks up the stage in the allowlist.
3. Calls `stage_loader_reload(loader, so_path, sym, install_fn)`.
4. Records to the audit log.

### Task 4: Lint gate (ABI freeze)

NEW `tools/lint/check_stage_abi_freeze.sh` — Gate #23. WARN mode.
Greps every header in `lib/stages/include/` and `lib/sync_stages/include/`
for symbols marked `_PUBLIC_ABI`; compares against
`tools/lint/stage_abi_baseline.txt`; reports any drift.

EDIT `DEFENSIVE_CODING.md` — document Gate #23.

### Task 5: Tests

NEW `lib/test/src/test_stage_reload.c` — 6 cases:
1. Load default impl, tick, verify cursor advanced.
2. Reload to test impl (different behavior), tick, verify new behavior.
3. Reload back to default, verify behavior reverted.
4. Reload while a tick is in flight — old tick must complete cleanly.
5. Reload with a .so that fails to dlopen — original stays active,
   error returned.
6. Reload a stage NOT in the allowlist — refused with audit log entry.

### Task 6: Audit log

NEW `lib/storage/src/reload_audit_log.c` — append-only file of
`{ts, stage, old_path, new_path, requester}` entries. Read via
`zcl_state subsystem=reload_audit_log`.

### Task 7: Documentation

EDIT `docs/architecture/phase7-frontier.md` — flip section 7c from
"draft" to "shipped"; list which stages are reloadable.

EDIT `BOOT_INVARIANTS.md` — note that reloads do NOT change boot stage;
they happen post-boot only.

### Task 8: Verify + push

```bash
make -j$(nproc)
make lint
./test_parallel --jobs=$(nproc)
git pull --rebase origin main
git push origin main
```

Append Completion section.

---

## Acceptance

- One reloadable stage (header_admit) ships with the loader.
- `zcl_stage_reload` works end-to-end (load test .so, observe new
  behavior, reload back).
- Audit log captures every reload.
- Gate #23 active in WARN mode with empty baseline.

---

## What this does NOT do

- Does NOT convert all 9 Wave S stages (only header_admit). Per-stage
  conversions are follow-on PRs gated on user interest.
- Does NOT touch consensus rules.
- Does NOT enable reload by default (admin-gated).

---

## Risk summary

This is the highest-risk Phase 7 sub-phase. Recommend deferring
indefinitely and revisiting only if hot-patching becomes a clear
operational need.

---

## Commit cadence

One commit per task. Push after Task 5.

---

## Status

**DRAFT — DEFER.** Do NOT claim until user explicitly approves Phase 7
dispatch AND specifically asks for hot reload (vs the other 7 sub-phases).
