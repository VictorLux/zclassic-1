# app/supervisors/

**Shape:** Supervisor — liveness tree with restart policy.

Each file in `src/` is one supervisor root using `SUPERVISOR_ROOT(...)`
from `lib/framework/supervisor.h`. A supervisor declares a tree of
supervised children (Jobs, other supervisors) with restart policies
(TRANSIENT / PERMANENT / TEMPORARY — Erlang/OTP semantics).

See [`docs/FRAMEWORK.md`](../../docs/FRAMEWORK.md) § 3.5 for the contract.

Existing supervisor primitives at `lib/util/supervisor.{c,h}` will move
to `lib/framework/supervisor.{c,h}` in Phase 1.

First file landing here (Phase 0): `self_heal.c` — registers the
condition engine as a supervisor child.
