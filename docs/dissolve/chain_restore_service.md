# Dissolve plan: `chain_restore_service.c` → 3 Jobs + 1 Service + 1 Condition

**Module:** `app/services/src/chain_restore_service.c` (1,674 LOC)
**Header:** `app/services/include/services/chain_restore_service.h`
**Phase:** 3 (Dissolve mega-modules)
**Gated on:** Wave S cutover C-9 shipped + soaked (so the saga owns
the forward-tip path before we touch reorg/restore)

---

## Why this exists today

`chain_restore_service.c` handles three distinct responsibilities
that are tangled together:

1. **Reorg execution** — when a competing chain wins, disconnect blocks
   from current tip back to the fork point, then connect blocks forward
   along the winning chain.
2. **Restore from anchor** — recover after corruption by finding a
   known-good ancestor and rebuilding forward.
3. **Post-restore integrity checks** — validate the rebuilt chain
   matches consensus expectations.

These are 3 separate Jobs in the framework sense, with one shared
Service for the planning layer.

---

## The decomposition

### Replacement A — `services/chain/restore_planner.c` (NEW, ~200 LOC)

Pure planning: given (current_tip, target_tip), compute the disconnect/
connect plan. No side effects.

```c
struct restore_plan {
    struct block_index *fork_point;
    int               disconnect_count;
    struct block_index **disconnect_path;  /* tip → fork_point */
    int               connect_count;
    struct block_index **connect_path;     /* fork_point → target_tip */
};

int restore_planner_compute(struct block_index *current_tip,
                            struct block_index *target_tip,
                            struct restore_plan *out);
void restore_planner_free(struct restore_plan *plan);
```

### Replacement B — `jobs/reorg_disconnect.c` (NEW, ~250 LOC)

Disconnects blocks one at a time. Cursor on disk. Idempotent.

- Cursor: `reorg_disconnect_cursor = (target_tip_hash, blocks_disconnected_so_far)`
- Per step: disconnect one block from current tip, advance cursor.
- Until cursor reaches the fork point.

### Replacement C — `jobs/reorg_connect.c` (NEW, ~250 LOC)

Connects blocks one at a time along the new chain. Cursor on disk.

- Cursor: `reorg_connect_cursor = (target_tip_hash, blocks_connected_so_far)`
- Per step: connect one block from current tip toward target. Uses
  S-5..S-9 saga stages (after cutover).

### Replacement D — `jobs/restore_from_anchor.c` (NEW, ~200 LOC)

The "I'm corrupt; help" job. Finds a known-good ancestor and runs the
reorg_connect job from there.

### Replacement E — Condition `chain_integrity_failed` (~100 LOC)

- **detect** — `chain_integrity_check_post_restore` finds zero_nbits,
  tip_window_holes, total_holes > threshold, or mismatches > 0.
- **remedy** — trigger `restore_from_anchor` job pointing at the most
  recent known-good snapshot.
- **clear** — integrity check passes.
- **max_attempts**: 2 (anchor restore is heavy; if 2 attempts fail,
  page the operator).

---

## Migration sequence (4 PRs)

### PR-1: Extract `restore_planner.c`

Move planning logic from `chain_restore_plan` into the new service.
Existing `chain_restore_service.c` calls the planner internally — no
behavior change.

### PR-2: Replace `chain_restore_execute` with the 2 Jobs

`reorg_disconnect` + `reorg_connect`. The existing `chain_restore_execute`
function becomes a thin shim that calls the jobs in sequence. Each job
has its own cursor, so a kill -9 mid-reorg resumes correctly.

### PR-3: Replace `chain_restore_create_anchor` + post-restore checks

`restore_from_anchor` job + `chain_integrity_failed` condition.

### PR-4: DELETE `chain_restore_service.{c,h}`

Move the boot-time snapshot recovery (`chain_restore_boot_snapshot`)
into a one-shot helper in `config/boot.c`. Update the 5-ish call sites.

Net: 1,674 LOC out, ~1,000 LOC in. **~700 LOC net deletion.**

---

## Acceptance gates per PR

- `make test_parallel` PASS.
- Live smoke includes a synthetic reorg test (the existing
  `test_reorg_safety.c` covers it — must stay green).
- 24h live soak across kill-9 mid-reorg; the cursor-based jobs
  resume correctly.

---

## Status

DRAFT — actionable after Wave S cutover C-9 shipped.
