# lib/framework/

**The framework primitives — the eight shapes' headers + implementations.**

This is the "Rails" of zclassic23. Every shape under `app/` includes
exactly one of these headers and uses its macros.

Files (target — empty in Phase 0 scaffold, populated incrementally):

| File | Shape | Status |
|---|---|---|
| `controller.h` | Controller — REQUEST_BEGIN/END, PARAM_*, AUTHORIZE | Phase 1 |
| `service.h`    | Service — SERVICE_BEGIN/END, REQUIRE, Result macros | Phase 1 |
| `model.h`      | Model — re-export of existing `app/models/include/models/activerecord.h` with MODEL/ATTR/QUERY/BEFORE_SAVE | Phase 1 |
| `job.h`        | Job — thin wrapper over existing `lib/util/stage.h` with `JOB(name, period_secs)` macro | Phase 1 |
| `supervisor.h` | Supervisor — re-export of existing `lib/util/supervisor.h` plus `SUPERVISOR_ROOT(...)` | Phase 1 |
| `condition.h`  | Condition — `CONDITION(...) DETECT/REMEDY/WITNESS` (NEW) | Shipped — struct-registration (no macro); see FRAMEWORK.md §198 |
| `event_log.h`  | Event — `EVENT(name, ARGS)` declaration + typed emit | Phase 4 |
| `projection.h` | Projection — re-export of existing `lib/util/projection.h` with read-view macros | Phase 1 |
| `mailbox.h`    | Mailbox — re-export of existing `lib/util/mailbox.h` with typed-message macros | Phase 1 |

See [`docs/FRAMEWORK.md`](../../docs/FRAMEWORK.md) for the architecture.

**Rule:** every file in `app/` includes EXACTLY ONE of these headers
(the one matching its shape). Lint gate enforces this from Phase 1.

**Hexagonal:** these headers are PORTS (interfaces). Adapters (the
implementations behind them) live in `lib/adapters/` (to be created in
Phase 3). Domain logic — pure consensus arithmetic — lives in
`lib/domain/` (to be created in Phase 3). Today, lots of code is in
the wrong place; the refactor is methodical migration.
