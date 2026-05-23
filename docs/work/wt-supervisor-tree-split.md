# Worker Assignment — Supervisor tree split (parallel-safe)

**Worktree:** wt2 OR wt3 (either) — pick when assigned
**Branch:** `wt?/supervisor-tree-split`
**Phase:** 2/3 boundary
**Depends on:** Nothing — parallel-safe with any current work
**Plan reference:** [`docs/architecture/supervisor-tree-split.md`](../architecture/supervisor-tree-split.md)

**Owns:**
- EDIT `lib/util/include/util/supervisor.h` — extend API with domains
- EDIT `lib/util/src/supervisor.c` — implement domains
- EDIT every `supervisor_register(...)` call site (~15-30 sites — grep)
  to use the appropriate `supervisor_register_in_domain(...)`
- NEW `tools/lint/check_supervisor_domain.sh`
- EDIT `Makefile` (add lint gate)
- EDIT `DEFENSIVE_CODING.md` (document gate #21 if that's the next number)
- EDIT MCP tool `zcl_state subsystem=supervisor` to return per-domain view
- NEW `lib/test/src/test_supervisor_domains.c`
- Edits to `test.c`, `test_parallel.c`, `test_helpers.h`

**MUST NOT touch:**
- Any Wave S stage (.c files) — read only
- Any condition (.c files) — read only
- `lib/framework/`, `lib/platform/`
- `docs/REFACTOR_STATUS.md`, `docs/FRAMEWORK.md`, `CLAUDE.md`

---

## Why this matters

Today's supervisor is a flat list. Every Condition, Job, and ticker
registers as a leaf child of one root. After this PR, 7 domain
supervisors group them by area (chain, net, mempool, wallet, feature,
onion, op). Operators can ask "is the chain area healthy?" without
filtering the whole list manually.

This is pure structural cleanup — no observable behavior change.
Parallel-safe with any other Wave S or Phase 3 work because it only
touches registration plumbing.

Read `docs/architecture/supervisor-tree-split.md` BEFORE starting.

---

## Tasks (in order)

### Task 1: Extend supervisor API with domains

In `lib/util/include/util/supervisor.h` + `.c`:

```c
typedef struct supervisor_domain supervisor_domain_t;

supervisor_domain_t *supervisor_create_domain(const char *label);

supervisor_child_id supervisor_register_in_domain(
    supervisor_domain_t *domain,
    struct liveness_contract *c);

bool supervisor_domain_dump_state_json(
    supervisor_domain_t *domain,
    struct json_value *out);
```

The existing `supervisor_register(...)` continues to work — it registers
against the implicit root domain.

Implementation: just an array of domains, each holding its own children
slice. Reuse the existing tick machinery.

**Acceptance:** unit test creates 2 domains, registers 1 child in each,
dump_state returns both correctly.

### Task 2: Create the 7 domain supervisors at boot

Find the existing supervisor init (grep `supervisor_init` or where
`self_heal` first registers). Add code that creates the 7 domains
right after supervisor init:

```c
g_chain_sup   = supervisor_create_domain("chain");
g_net_sup     = supervisor_create_domain("net");
g_mempool_sup = supervisor_create_domain("mempool");
g_wallet_sup  = supervisor_create_domain("wallet");
g_feature_sup = supervisor_create_domain("feature");
g_onion_sup   = supervisor_create_domain("onion");
g_op_sup      = supervisor_create_domain("op");
```

Expose them via `app/supervisors/include/supervisors/domains.h`:
```c
extern supervisor_domain_t *g_chain_sup;
/* ... */
```

### Task 3: Migrate existing call sites

Grep `supervisor_register\b` across `app/` and `lib/`. For each site,
classify by subsystem (see `docs/architecture/supervisor-tree-split.md`
for the mapping). Replace:

```c
g_id = supervisor_register(&contract);
```

with:

```c
g_id = supervisor_register_in_domain(g_chain_sup, &contract);
```

Add `#include "supervisors/domains.h"` where needed.

Expected ~15-30 sites. Per-site change is one line plus the include.

### Task 4: Update `zcl_state subsystem=supervisor`

In `app/controllers/src/diagnostics_controller.c` (or wherever
`supervisor_dump_state_json` is wired), change the output to:

```json
{
  "domains": [
    {"name": "chain",   "child_count": N, "children": [...]},
    {"name": "net",     "child_count": N, "children": [...]},
    {"name": "mempool", "child_count": N, "children": [...]},
    {"name": "wallet",  "child_count": N, "children": [...]},
    {"name": "feature", "child_count": N, "children": [...]},
    {"name": "onion",   "child_count": N, "children": [...]},
    {"name": "op",      "child_count": N, "children": [...]}
  ],
  "root_orphans": []
}
```

If `root_orphans` is non-empty after Task 3, something was missed —
investigate.

Also support `zcl_state subsystem=supervisor.chain` (return just the
chain domain). Optional: alias `supervisor` → all domains
(backwards-compatible).

### Task 5: Add lint gate

NEW: `tools/lint/check_supervisor_domain.sh`:

```bash
#!/usr/bin/env bash
# Gate #21: every supervisor registration must specify a domain.
# After Task 3, baseline is 0 (every site migrated).

set -e
COUNT=$(grep -rnE 'supervisor_register\b\s*\(' app/ lib/ --include='*.c' \
        | grep -v 'supervisor_register_in_domain' \
        | grep -v 'lib/util/src/supervisor.c' \
        | grep -v '// supervisor-root-ok:' \
        | wc -l)

MODE="${ZCL_LINT_MODE:-WARN}"
if [ "$COUNT" -gt 0 ]; then
    grep -rnE 'supervisor_register\b\s*\(' app/ lib/ --include='*.c' \
        | grep -v 'supervisor_register_in_domain' \
        | grep -v 'lib/util/src/supervisor.c' \
        | grep -v '// supervisor-root-ok:'
    echo "[check_supervisor_domain] $COUNT violation(s) (mode: $MODE)"
    if [ "$MODE" = "FAIL" ]; then exit 1; fi
fi
echo "[check_supervisor_domain] PASS"
```

Add to `Makefile` `lint` target. Default mode FAIL (since baseline is 0).
Document gate #21 in `DEFENSIVE_CODING.md`.

### Task 6: Tests

`lib/test/src/test_supervisor_domains.c`:
- Create 3 domains; register 2 children in each.
- Verify `supervisor_domain_dump_state_json` returns each domain
  correctly.
- Verify root has no orphans.
- Verify `supervisor_child_count_total` sums children across all
  domains.

Register in `test.c`, `test_parallel.c`, `test_helpers.h`.

### Task 7: Final verify + push

```bash
make -j$(nproc)
make lint                                # gate #21 = FAIL, 0 violations
./test_parallel --jobs=$(nproc)          # all green
git push origin wt?/supervisor-tree-split
```

Append Completion section per `docs/work/agent-protocol.md`.

---

## Commit cadence

One commit per task. Push after tasks 3, 5, 6.

---

## Status

**READY (any worker)** — parallel-safe with Wave S cutover and Phase 3
dissolves. Pick this up if you're between assignments or as an
explicit dispatch from the orchestrator.

<!-- Worker: append a Completion section below when done. -->
