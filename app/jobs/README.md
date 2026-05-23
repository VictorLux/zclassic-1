# app/jobs/

**Shape:** Job — idempotent, cursor-stamped async stage.

Each file in `src/` is one Job using the `JOB(...)` macro from
`lib/framework/job.h`. Jobs run periodically (their `PERIOD_SECS`), read
state via models, mutate via models, advance a cursor in `progress.kv`,
and emit events.

See [`docs/FRAMEWORK.md`](../../docs/FRAMEWORK.md) § 3.4 for the contract
and § 7.2 for the cookbook to add a new Job.

Canonical existing stage (pre-framework, target migration):
`app/services/src/header_admit_stage.c`, `validate_headers_stage.c`,
`body_fetch_stage.c`. These move here as Phase 2 (Wave S finish).
