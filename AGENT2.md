# AGENT2 — Chain State & Recovery Hardening

**Worktree:** `~/zclassic23-2`
**Branch:** `agent2/chain-state-repo` (create from `origin/master`)
**Coordinator:** AGENT1 (main `~/zclassic23`)
**Peer:** AGENT3 (`~/zclassic23-3`) — working on MCP/controllers, different files

---

## Mission

Eliminate the root cause of catastrophic UTXO loss: **fragmented chain-tip state** across six independent sources of truth. Build a single-writer chain state repository and a recovery policy layer that refuses destructive operations when the blast radius is large.

## Why this matters

On 2026-04-10 two different boot paths wiped 1.3M UTXOs because `block_index.bin` had a stale `h=60` entry but the SQLite `utxos` table held the real data at `h=3,073,476`. The code trusted whichever metadata it happened to read first and deleted everything "above tip." AGENT1 patched the two smoking guns in `config/src/boot.c` (lines ~2010 and ~2333), but the architecture that allowed it is still there.

There are **67 call sites across 21 files** that mutate chain-tip state (`active_chain_set_tip`, `coins_view_cache_set_best_block`, `node_db_wipe_utxos`). Any one of them can desynchronize the six sources:

1. `block_index.bin` (in-memory + flushed)
2. SQLite `blocks` table
3. `coins_best_block` hash
4. `chain_active` tip pointer
5. `pindexBestHeader`
6. wallet's notion of scan height

## Deliverables

### Phase 1 — Chain State Repository (primary)

Create `app/services/chain_state_repository.{h,c}` (~600 lines) with:

```c
/* Single entry point for any tip mutation. All 67 existing call sites
 * must go through this. Internal locks guarantee the six sources stay
 * consistent or the commit fails and NOTHING changes. */
struct chain_state_commit {
    struct block_index *new_tip;     /* must be in block_map */
    struct uint256 new_coins_best;   /* must match new_tip->hash */
    int64_t expected_utxo_count;     /* refuse if actual differs by >X% */
    const char *reason;              /* logged, shown in events */
};

enum csr_result {
    CSR_OK,
    CSR_REJECTED_STALE_INDEX,        /* block_index.bin disagrees with SQLite */
    CSR_REJECTED_UTXO_DELTA_TOO_BIG, /* would delete too many rows */
    CSR_REJECTED_MISSING_PREV,       /* new_tip->pprev not in block_map */
    CSR_REJECTED_HASH_MISMATCH,      /* SQLite blocks.hash != new_tip->hash */
};

enum csr_result csr_commit_tip(struct chain_state_repository *csr,
                                const struct chain_state_commit *commit);

/* Read-only introspection — never mutates */
void csr_snapshot(const struct chain_state_repository *csr,
                   struct chain_state_view *out);
```

Requirements:

- **Atomic**: the six sources are updated inside one mutex and either all succeed or none do
- **Validated**: every commit cross-checks `block_index.bin` against the SQLite `blocks` table (this catches the h=60 vs h=3M bug)
- **Observable**: every commit emits an event (`EV_CHAIN_TIP_COMMIT` / `EV_CHAIN_TIP_REJECTED`) with before/after heights and the reason
- **Logged**: structured log line on every commit and every rejection
- **Test-covered**: at least 20 tests in `lib/test/src/test_chain_state_repo.c` covering happy path, each rejection code, and concurrency

Then migrate call sites in this order (small PRs, one per file):

1. `lib/validation/src/chainstate.c` (1 site)
2. `lib/validation/src/process_block.c` (5 sites)
3. `lib/validation/src/connect_block.c` (3 sites)
4. `app/services/src/snapshot_sync_service.c` (5 sites)
5. `app/services/src/chain_restore_service.c` (1 site)
6. `config/src/boot.c` (27 sites — biggest, do last)
7. Remaining files (see `Grep` results in appendix)

### Phase 2 — Recovery Policy Layer (after Phase 1 lands)

Create `app/services/recovery_policy.{h,c}` (~400 lines) that gates all destructive operations:

```c
struct recovery_policy {
    int64_t max_utxo_wipe_rows;      /* default 1000 */
    int64_t max_block_rollback;      /* default 100 */
    bool    require_backup_verified; /* refuse destructive ops without backup */
};

enum policy_decision {
    POLICY_ALLOW,
    POLICY_REFUSE_TOO_LARGE,
    POLICY_REFUSE_NO_BACKUP,
    POLICY_PROMPT_OPERATOR,  /* writes a marker file, waits for human ack */
};

enum policy_decision policy_check_utxo_wipe(
    const struct recovery_policy *p,
    int64_t proposed_row_count,
    const char *reason,
    struct event_bus *eb);
```

Then move the 7 existing `node_db_wipe_utxos()` call sites in `boot.c` behind this gate. Each one must supply a reason string.

### Phase 3 — Boot Decomposition (stretch)

`config/src/boot.c` is **2,624 lines**. Extract:

- `app/services/block_index_loader.{h,c}` — owns `block_index.bin` load/validate
- `app/services/chain_state_validator.{h,c}` — pre-boot sanity checks
- `app/services/utxo_recovery_service.{h,c}` — orchestrates wipe/recover decisions via `recovery_policy`

Target: reduce `boot.c` to <800 lines. Do not touch boot code AGENT1 hasn't signed off on — boot is the most dangerous file in the repo.

---

## Coordination rules

### Syncing with main

You work on a separate clone at `~/zclassic23-2`. Your workflow:

```bash
cd ~/zclassic23-2
git fetch origin
git rebase origin/master        # before starting any session
# ... work ...
make -j$(nproc) test_zcl && ./test_zcl   # MUST pass before push
git push origin agent2/chain-state-repo
```

**Never force-push to `master`.** Open PRs into master or let AGENT1 merge your branch manually.

### Files you own (safe to edit freely)

- `app/services/chain_state_repository.{h,c}` (new)
- `app/services/recovery_policy.{h,c}` (new)
- `app/services/block_index_loader.{h,c}` (new, phase 3)
- `app/services/chain_state_validator.{h,c}` (new, phase 3)
- `app/services/utxo_recovery_service.{h,c}` (new, phase 3)
- `lib/test/src/test_chain_state_repo.c` (new)
- `lib/test/src/test_recovery_policy.c` (new)
- `AGENT2.md` — update "Current Status" at the bottom each session

### Files you share with AGENT1 (coordinate in commits)

- `config/src/boot.c` — AGENT1 holds the pen here. Ask before editing.
- `lib/validation/src/chainstate.c`, `process_block.c`, `connect_block.c` — migrate these *one at a time*, each in its own commit

### Files AGENT3 owns — DO NOT TOUCH

- `tools/mcp_server.c`
- `app/controllers/**`
- `app/models/**`

### Commit discipline

- **One commit per logical step.** Migrating a call site is one commit. Adding the service is another.
- **Every commit must build and pass `./test_zcl`.** No exceptions.
- **Commit messages start with `agent2:`** — e.g., `agent2: add chain_state_repository service`
- **Never commit generated binaries** (`zclassic23`, `test_zcl`, `speedrun`, `export_snapshot`).

### When you're blocked

Write the blocker into the "Current Status" section of this file and push. AGENT1 will pick it up on the next coordination pass.

---

## Definition of done for Phase 1

- [ ] `chain_state_repository.{h,c}` exists, compiles, has 20+ passing tests
- [ ] All 67 call sites migrated (track in a checklist below)
- [ ] `./test_zcl` passes (1500+ tests, 0 failures)
- [ ] `make deploy` brings up a node that survives 10 consecutive reboots with no UTXO loss
- [ ] Events `EV_CHAIN_TIP_COMMIT` and `EV_CHAIN_TIP_REJECTED` appear in `zcl_events` output
- [ ] New MCP view of chain-state health (coordinate with AGENT3 for the MCP tool name)

---

## Current Status

*(Update this every session with what you did and what's next. Keep it short.)*

- **2026-04-11** — Plan created by AGENT1. AGENT2 has not started.
- **2026-04-11** — Phase 1 service landed. `app/services/{include/services,src}/chain_state_repository.{h,c}` provides the single-writer `csr_commit_tip()` API with full validation: NULL/init checks, block_map cross-check, pprev presence, SQLite hash/height agreement, stale-index gap guard, expected-utxo drift, orphan-rows rollback guard. `csr_snapshot()` exposes a read-only view. Both `EV_CHAIN_TIP_COMMIT` and `EV_CHAIN_TIP_REJECTED` events are emitted on every outcome and registered in `event.c`'s name table. 28 unit tests in `lib/test/src/test_chain_state_repo.c` cover happy path, every rejection code, observability, tunables, and a 4-thread × 100-iter concurrency stress; full `./test_zcl` is green (`ALL TESTS PASSED (0 failures)`). **Next**: Phase 1 call-site migration — start with `lib/validation/src/chainstate.c` (1 site), then `process_block.c` (5 sites). 67 sites remain.

---

## Appendix — call sites to migrate

```
app/models/include/models/database.h       1
app/models/src/database.c                   1
app/services/src/snapshot_sync_service.c    5
app/services/src/chain_restore_service.c    1
config/src/boot_services.c                  2
config/src/boot_index.c                     4
config/src/boot.c                          27
app/controllers/src/blockchain_controller.c 2
app/controllers/src/sync_controller.c       1
lib/coins/include/coins/coins_view.h        1
lib/coins/src/coins_view.c                  2
lib/test/src/test_sync_service.c            3
lib/net/src/msgprocessor.c                  2
lib/test/src/test_coins.c                   2
lib/test/src/test_chain.c                   1
lib/test/src/test_validation.c              1
lib/test/src/test_node_health_service.c     1
lib/validation/include/validation/chainstate.h   1
lib/validation/src/chainstate.c             1
lib/validation/src/process_block.c          5
lib/validation/src/connect_block.c          3
                                           ──
                                           67
```

Grep pattern used:
`active_chain_set_tip|coins_view_cache_set_best_block|node_db_wipe_utxos`
