# app/conditions/

**Shape:** Condition — `(detect, remedy, witness)` auto-healer.

Each file in `src/` is one Condition using `CONDITION(...)` from
`lib/framework/condition.h`. A Condition declares:

- `DETECT { ... }` — predicate that returns true when the wedge is present
- `REMEDY { ... }` — action that attempts to fix it (calls existing
  service/model APIs)
- `WITNESS { ... }` — observable post-condition that confirms the
  remedy worked

Plus tuning: `POLL_SECS`, `BACKOFF_SECS`, `MAX_ATTEMPTS`.

The condition engine (in `lib/framework/condition.{c,h}`) polls every
registered condition, dispatches remedies under backoff, and pages the
operator only if `MAX_ATTEMPTS` is exhausted with `WITNESS = false`.

**Every wedge class becomes a file here.** This is the auto-resolution
substrate. See [`docs/FRAMEWORK.md`](../../docs/FRAMEWORK.md) § 3.6
for the contract and § 7.3 for the cookbook.

Phase 0 ships:
- `block_failed_mask_at_tip.c` — wires `process_block_revalidate`
- `contradiction_frozen.c`     — chain_evidence frozen rebuild
- `chain_stalled_with_data.c`  — body available but tip not advancing
