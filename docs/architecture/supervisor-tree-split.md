# Supervisor tree split — from flat list to domain-grouped supervisors

**Status:** PLAN (draft 2026-05-23)
**Phase:** 2/3 boundary (Wave S "S-10" equivalent)
**Gated on:** No hard gate — can land in parallel with Wave S cutover and
mega-module dissolves. Adds structure without removing behavior.

---

## What this is

Today: there's ONE supervisor (`self_heal`) holding the condition engine
tick. Future Conditions, Jobs, and the per-stage tickers ALL register
into the same root.

After this split: 7 domain supervisors, each owning the children that
relate to its area. The root supervisor owns the domain supervisors;
each domain supervisor owns its leaf children.

```
                    root_sup (the process itself)
                         │
       ┌──────────┬──────┴──────┬──────────┬──────────┬──────────┬──────────┐
       │          │             │          │          │          │          │
   chain_sup  net_sup     mempool_sup  wallet_sup  feature_sup onion_sup op_sup
       │          │             │          │          │          │          │
   stages     peers         eviction    backup    games        tor      health
   conds      addrman       limits      rescan    market       directory  heartbeat
   probe      bandwidth                            znam         dynhost     metrics
   restore                                         zmsg
```

---

## Why split

Today's flat supervisor:
- Lists every Condition + every Job + the heartbeat in one row.
- Hard to reason about WHICH subsystem is sick when something goes red.
- Operator pages can't distinguish "chain area is broken" vs "wallet area
  is broken."

Domain supervisors:
- `zcl_state subsystem=supervisor` returns 7 small dumps instead of one
  100-child dump. Easier on the eyes, easier on the JSON parser.
- Domain-specific aggregate metrics: "any chain_sup child stalled?" is
  one query.
- Future: domain supervisors can independently restart their children
  (e.g., restart all chain stuff after a chain_advance corruption,
  without restarting Tor or the wallet).

---

## The 7 domain supervisors

### `chain_sup`

Owns:
- All 9 Wave S stages (S-1..S-9) — both shadow and authoritative
- `block_failed_mask_at_tip`, `legacy_mirror_stuck`,
  `contradiction_frozen` conditions
- `chain_evidence_controller` ticker
- After Phase 3: every condition extracted from sync_watchdog (all 8)

### `net_sup`

Owns:
- `header_probe` job (after Phase 3 dissolve)
- `peer_scoring` ticker
- `peer_bandwidth` ticker
- `peer_lifecycle` ticker
- `outbound_floor_kick` (existing supervisor child)
- `peer_floor_violated`, `sync_violation_lag` conditions
- `connman.tick`

### `mempool_sup`

Owns:
- Mempool eviction ticker
- `mempool_limits` enforcement
- Dandelion timer

### `wallet_sup`

Owns:
- Wallet rescan job
- Wallet backup job (if periodic — currently on-demand)
- Wallet flush rollback ticker

### `feature_sup`

Owns the optional features that aren't chain-critical:
- ZSLP processor
- ZNAM ticker
- ZMSG dispatcher
- File market ticker
- P2P game framework
- Atomic swap watcher

### `onion_sup`

Owns:
- Tor bootstrap state
- Onion directory ticker
- Dynhost re-publish loop

### `op_sup`

Owns the operator-facing infrastructure:
- `heartbeat` ticker (existing)
- `metrics` flush
- Health check ticker
- `node_health_service` ticker
- Condition engine root ticker (NEW — currently in self_heal)

---

## Migration sequence (1 PR)

Single PR — no multi-stage rollout because it's pure refactoring of
registration, no behavior change.

### Task 1: Add `supervisor_create_domain(label)` API

Extend `lib/util/include/util/supervisor.h`:

```c
typedef struct supervisor_domain supervisor_domain_t;

/* Create a domain supervisor. Children register against this domain. */
supervisor_domain_t *supervisor_create_domain(const char *label);

/* Register a child against a specific domain (instead of the root). */
supervisor_child_id supervisor_register_in_domain(
    supervisor_domain_t *domain,
    struct liveness_contract *c);

/* Dump state for a single domain. */
bool supervisor_domain_dump_state_json(
    supervisor_domain_t *domain,
    struct json_value *out);
```

Existing `supervisor_register(...)` continues to work — it registers
against the root domain.

### Task 2: Create the 7 domain supervisors at boot

In the appropriate init point (search `supervisor_register` call sites
+ `boot.c`), create the 7 domains at startup. Each domain gets a
single fixed name and is referenced by all subsystems that register
into it.

### Task 3: Migrate existing children to their domains

For each existing `supervisor_register(...)` call site:
- Identify which domain it belongs to.
- Replace with `supervisor_register_in_domain(<domain>, ...)`.

Roughly 15-30 call sites to migrate.

### Task 4: Update `zcl_state subsystem=supervisor`

Make it return:
```json
{
  "domains": [
    {"name": "chain", "child_count": 12, "children": [...]},
    {"name": "net", "child_count": 8, "children": [...]},
    ...
  ],
  "root_orphans": []   // should be empty after migration
}
```

Plus an alias: `zcl_state subsystem=supervisor.chain` returns just the
chain domain.

### Task 5: Lint gate — every supervisor child MUST be in a domain

NEW: `tools/lint/check_supervisor_domain.sh`. Greps for
`supervisor_register\b` (root form) and rejects new occurrences.
WARN baseline 0 (all migrated in Task 3); ratcheted to FAIL after
24h soak.

---

## Acceptance gates

- `make test_parallel` PASS.
- Live `zcl_state subsystem=supervisor` returns the 7-domain structure.
- `root_orphans` is empty (every child has a home).
- No new operator pages in the 24h soak.

---

## What this enables

After the split:

- **Better operator UX:** "what's wrong?" answer is a domain name, not
  a list of 30 children.
- **Selective restart in Phase 4+:** a domain's children can be
  restarted together if their shared state (e.g., a projection) gets
  rebuilt.
- **Cleaner per-PR scoping in future dissolves:** "extract from
  net_sup" is a clear boundary instead of "extract from the global
  root."
- **Pre-Phase-4 prerequisite for proper child-of-child supervision**
  (Phoenix-style trees), if we ever need it.

---

## Status

DRAFT — can dispatch in parallel with Wave S cutover work. Single PR.
Low risk (pure registration refactor). Roughly 1 worker session.
