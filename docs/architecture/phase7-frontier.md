# Phase 7 — Frontier (io_uring, structured concurrency, hot reload)

**Status:** PLAN (draft 2026-05-23) — OPTIONAL
**Phase:** 7 (after Phase 6 simulator)
**Estimated scope:** 3 independent sub-phases, optional, defer indefinitely if not needed

> "Phase 7 is performance polish you can defer indefinitely."

---

## Why "frontier"

Phases 0-6 deliver the core architectural goals: correctness,
auto-healing, deletable mega-modules, deterministic replay, signed
reproducible releases. Phase 7 is optimization work that only makes
sense ONCE the core is in place — these changes have steep risk
profiles and modest payoffs, so they should be defended individually
when (if) the user wants them.

---

## Sub-phases (each independent)

### 7a — io_uring for disk I/O

Replace the current `pread`/`pwrite` + `fsync` calls on the event log
hot path with `io_uring` submission/completion queues. Result: ~2-5×
throughput on append-only writes, dramatically reduced syscall
overhead.

**Scope:** rewrite `lib/storage/src/event_log.c`'s write path. ~200
LOC changed, ~300 LOC added (io_uring queue management).

**Risk:** io_uring has had kernel CVEs; pinning a known-good kernel
+ defensive runtime checks needed.

**Payoff:** event log append throughput from ~50K/sec to ~200K/sec.
Only matters if we hit ingest-bound performance, which we currently
don't.

### 7b — Structured concurrency

Replace the current ad-hoc `pthread_create` + atomic flag shutdown
pattern with structured scopes (every spawned task is owned by a
parent scope; the parent scope can't exit until all children
complete or are cancelled).

NEW: `lib/util/include/util/scope.h`.

```c
typedef struct scope scope_t;

/* Open a scope. All tasks spawned inside cancel when the scope exits. */
scope_t *scope_open(const char *label);

/* Spawn a task within this scope. */
int scope_spawn(scope_t *s, void (*fn)(void *), void *arg);

/* Block until all spawned tasks complete; then close the scope. */
void scope_close(scope_t *s);

/* Cancel all tasks in the scope (cooperative — tasks check
 * scope_cancel_requested()). */
void scope_cancel(scope_t *s);
bool scope_cancel_requested(const scope_t *s);
```

After 7b: thread leaks become impossible by construction. Shutdown
ordering becomes mechanical (close child scopes before parent).

**Scope:** rewrite the ~30 `pthread_create` call sites + their
shutdown handlers. ~500 LOC touched.

**Risk:** subtle deadlocks if a scope's task waits on something
outside its scope.

**Payoff:** eliminates the entire class of "thread leaked on shutdown"
bugs. Cleaner crash logs.

### 7c — Hot reload

Replace a stage's implementation WITHOUT restarting the node.

Mechanism: each stage is wrapped in a small dispatcher that holds a
function pointer to the current implementation. Loading a new shared
library + atomically swapping the pointer = hot reload. The stage's
cursor lives on disk so it picks up where it left off.

**Scope:** dispatcher wrapper for every stage (~50 LOC each × 9
stages = 450 LOC). Shared library build target per stage.

**Risk:** ABI compatibility across reloads (struct layouts must
match); subtle bugs if a stage's persistent state's format changes.

**Payoff:** experimental — try a new validation strategy without
restarting. Mostly useful for development; production probably won't
hot-reload.

---

## Status

DRAFT — entirely OPTIONAL. None of these is required to ship the
sovereignty stack. They're listed for completeness so future-us has
the menu when (if) we want them.

Defer to user decision before dispatching any 7a/7b/7c work.
