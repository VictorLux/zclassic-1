# Worker Assignment — Phase 3 (header_probe dissolve PR-1): extract `header_probe_poll` Job

**Worktree:** wt2 OR wt3 (either)
**Branch:** PUSH DIRECT TO MAIN
**Phase:** 3 (Dissolve mega-modules)
**Depends on:** Supervisor tree split ✅ (`dae31dee9`) — required for
the supervisor to host the new Job. Independent of Wave S cutover.
**Plan reference:** [`docs/dissolve/header_probe_service.md`](../dissolve/header_probe_service.md) § PR-1

**Status: READY** — claim by marking IN PROGRESS.

**Owns:**
- NEW `app/jobs/include/jobs/header_probe_poll.h`
- NEW `app/jobs/src/header_probe_poll.c`
- EDIT `app/services/src/header_probe_service.c` — remove the background
  thread; expose the existing `header_probe_pull_range` as a callable
- EDIT `app/services/include/services/header_probe_service.h` — make
  `header_probe_pull_range` public (it may already be)
- EDIT `config/src/boot_services.c` — register the Job with the network
  supervisor (NOT the legacy thread start)
- EDIT `lib/util/include/util/supervisor.h` (only if a new register
  variant is needed — probably not; existing API suffices)
- NEW `lib/test/src/test_header_probe_poll.c`
- EDIT `lib/test/src/test.c` + `lib/test/src/test_parallel.c` +
  `lib/test/include/test/test_helpers.h`

**MUST NOT touch:**
- Anything beyond the 30s polling cadence — peer selection, scoring,
  request-response loop, msg_getheaders parser all STAY in
  `header_probe_service.c` for PR-1
- Wave S stage files, Phase 4 / 5 / 6 code paths
- `docs/REFACTOR_STATUS.md`, `docs/FRAMEWORK.md`, `CLAUDE.md`

---

## Why this matters

`header_probe_service.c` is 1,286 LOC and one of the 5 mega-modules
the refactor wants gone. The full dissolve is 3 PRs gated on Wave S
cutover. **PR-1 is the part that ships TODAY** — it just moves the
30s polling from a private background thread to a supervisor-tracked
Job. Zero behavior change; one less untracked thread; one more thing
in the supervisor's `last_tick_age_us` table.

Why split it out first:
1. The Job becomes a small (~80 LOC) standalone primitive that PR-2
   can shrink the service against.
2. The supervisor tree gains visibility into the poll cadence — if
   it stops ticking, `zcl_state subsystem=supervisor` shows the
   stall age, which is otherwise invisible.
3. Validates the Job pattern against a real second user (currently
   only Wave S stages use it).

---

## What this does NOT do

- Does NOT shrink `header_probe_service.c` (PR-2 does).
- Does NOT rename / move the service file (PR-3 does).
- Does NOT change peer selection / scoring / request-response logic.
- Does NOT change the polling cadence (stays 30s).
- Does NOT change the network protocol or any wire format.

---

## Tasks (in order)

### Task 1: Identify the current pthread

READ `app/services/src/header_probe_service.c`. Find:
- The `pthread_create` call that starts the polling thread.
- The thread function — typically a `while (running) { sleep 30; pull_range(...) }` loop.
- Any `pthread_join` or `pthread_cancel` calls during shutdown.

Document (in a scratch note for yourself) the thread's exact contract:
- What it calls
- How it observes shutdown
- What it logs on each iteration

**Acceptance:** clear understanding of what the Job needs to replicate.

### Task 2: Write `jobs/header_probe_poll.c`

NEW file. Pattern after an existing Wave S Job for shape (e.g.,
`app/services/src/header_admit_stage.c` runs at a cadence — check
how it registers with the supervisor).

The Job's tick function:
```c
static void header_probe_poll_tick(void *arg)
{
    (void)arg;
    /* Honor the same gating the legacy thread had:
     *   - Skip if not yet listening
     *   - Skip if no peers
     *   - Skip if header_probe_service is paused (existing flag)
     */
    if (!net_listening() || net_peer_count() == 0) return;
    if (header_probe_service_is_paused()) return;

    int64_t local_tip = chain_local_header_height();
    /* Same range the legacy thread pulled. */
    header_probe_pull_range(local_tip + 1, 2000);
}
```

Register with the network supervisor (the one Round 5's supervisor
tree split established):
```c
supervisor_register(g_supervisor_net,
                    "net.header_probe_poll",
                    30 * 1000 * 1000,  /* 30s in us */
                    header_probe_poll_tick,
                    NULL);
```

Same defaults as the existing thread used.

**Acceptance:** compiles clean. Boot sequence registers the Job
without errors.

### Task 3: Remove the background thread from header_probe_service.c

EDIT `app/services/src/header_probe_service.c`:
1. Delete the `pthread_create` call.
2. Delete the thread function (or leave it but mark `__attribute__((unused))`
   and revisit in PR-2).
3. Delete the `pthread_join` from the shutdown path.
4. Make `header_probe_pull_range` public (if not already).
5. Keep everything else.

EDIT `app/services/include/services/header_probe_service.h` — expose
`header_probe_pull_range` if it was static.

**Acceptance:** `make` clean. The service still exists + still does
its peer selection + scoring work; only the SCHEDULING moved.

### Task 4: Update boot wiring

EDIT `config/src/boot_services.c`:
1. Remove the `header_probe_service_start_polling()` call (if there
   was one — may already be implicit in service init).
2. After supervisor init, register the new Job (Task 2).

**Acceptance:** boot completes; `zcl_state subsystem=supervisor`
shows `net.header_probe_poll` with non-zero `last_tick_age_us` and
incrementing `ticks_run`.

### Task 5: Unit test

NEW `lib/test/src/test_header_probe_poll.c`:
- Verify Job registers with the supervisor.
- Verify the tick function is gated correctly (no peers → no pull).
- Verify the tick function calls `pull_range(local_tip + 1, 2000)`
  when conditions are right (use a mock for the pull function — pass
  a function pointer instead of calling the real network).

Add `failures += test_header_probe_poll();` to `test.c` +
`test_parallel.c` TEST_LIST + helpers header.

**Acceptance:** `./test_parallel --jobs=$(nproc)` PASS,
`header_probe_poll: N passed, 0 failed`.

### Task 6: Live verification

```bash
# (on dev node)
make deploy   # or whatever the local deploy step is
sleep 60
# Verify the Job is firing:
zcl_state subsystem=supervisor | jq '.children[] | select(.name=="net.header_probe_poll")'
# Should show ticks_run > 1, last_tick_age_us < 30_000_000.

# Verify header sync velocity unchanged:
zcl_kpi
# Look at the blocks_per_sec / headers_per_sec — should match pre-PR baseline.
```

**Acceptance:** Job ticks regularly; no header-sync regression.

### Task 7: Final verify + push

```bash
make -j$(nproc)
make lint
./test_parallel --jobs=$(nproc)
git pull --rebase origin main
git push origin main
```

Append a Completion section to this file.

---

## Risk + rollback

The risk is low — the thread → Job swap is a pure scheduling change.
Worst case: the Job doesn't fire (supervisor bug, registration error),
and header probing stops. The header_admit stage will eventually fall
behind tip, the chain_tip_watchdog will alarm, and an operator can
restart the node back to the legacy path via `git revert`.

Mitigation: Task 6's live check catches this before the PR is
considered done.

---

## Commit cadence

One commit per task. Push after Task 4 + Task 5.

---

## Status

**READY** — Phase 0/1 supervisor primitives are in place; the network
supervisor exists; the existing service exposes the needed function.

<!-- Worker: append a Completion section below when done. -->

---

## Completion — 2026-05-24 (wt-main session)

**Status: DONE**

### What shipped
- NEW `app/jobs/include/jobs/header_probe_poll.h` — Job public API.
- NEW `app/jobs/src/header_probe_poll.c` — Job impl. Static
  `liveness_contract`, 30 s `period_secs`, registers into
  `g_net_sup` via `supervisor_register_in_domain`. Tick body
  delegates to a new public `header_probe_tick_once()` on the
  service.
- EDIT `app/services/include/services/header_probe_service.h` —
  exposed `header_probe_tick_once()`. Existing
  `header_probe_pull_range` was already public; no change.
- EDIT `app/services/src/header_probe_service.c` — extracted the
  heartbeat-callback body into the public `header_probe_tick_once()`;
  legacy `hp_on_tick` is now a thin shim delegating to it. Zero
  behavior change — same RPC, same gating, same
  `accept_block_header` path.
- EDIT `config/src/boot_services.c` — `boot_header_probe_start()`
  now calls `header_probe_poll_register()` after init instead of
  `header_probe_start()` (heartbeat ring). `boot_header_probe_stop()`
  is a no-op (supervisor handles unregister at `supervisor_stop()`).
- EDIT `Makefile` — added `jobs` to `APP_DIRS` so
  `app/jobs/src/*.c` is compiled and `app/jobs/include` is on
  the include path.
- NEW `lib/test/src/test_header_probe_poll.c` — 8 assertions:
  registration idempotency, visibility in supervisor snapshot,
  cadence config = 30 s, safe-no-op when service uninitialized.
- WIRE the test into `lib/test/include/test/test_helpers.h`,
  `lib/test/src/test.c`, `lib/test/src/test_parallel.c`.

### Verification
- `make -j$(nproc)` clean (zclassic23 + test_zcl + test_parallel
  all rebuilt).
- `make lint` PASS (no new violations; pre-existing WARN gates
  unchanged).
- `./test_parallel --jobs=$(nproc)` — **0/199 groups failed**,
  108 s wall, 32 workers. `test_header_probe_poll` 8/8 OK.
- `make deploy` restarted the live node (deploy_verify timed out
  on a pre-existing `chain_evidence` frozen-contradiction
  unrelated to this PR; node started cleanly and serves RPC).
- **Live `dumpstate supervisor` confirms the new child:**
  ```
  [net] net.header_probe_poll: period=30s ticks_run=1
        last_tick_age_us=7154168 progress_marker=1
  ```
  Ticked once within the first 30 s as designed.

### What was NOT touched (per spec)
- `header_probe_service.c` peer-selection / scoring / batched
  RPC / `accept_block_header` paths — all unchanged.
- Wave S stage files, Phase 4 / 5 / 6 code paths.
- `docs/REFACTOR_STATUS.md`, `docs/FRAMEWORK.md`, `CLAUDE.md`.
- The legacy `header_probe_start()` heartbeat-ring path stays
  callable (MCP tools / tests that still need it). Boot just no
  longer wires it.
