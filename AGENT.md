# AGENT — Wave 22b: Thread Safety + Observability (Coordinator)

## Current Mission

Harden `zclassic23` — fix crash-causing races, add observability, investigate disabled features.

**Node syncing (2M → 3M+), 17 peers, tests pass.**

---

## Status (2026-04-14)

### Completed (Wave 22a — Agent3)
- [x] LOG_FAIL spam removed from fast_sync PoW solve loop
- [x] before_save hooks wired: mempool_entry, tx_index, wallet_tx
- [x] fprintf→LOG_ERR migration: bg_validation, chain_state_repository, snapshot_sync, utxo_recovery
- [x] Raw allocators in lib/validation/ already using zcl_* (was a false gap)
- [x] Tests: ALL 95 STORIES PASSED, 0 failures

### In Progress (Wave 22b)
- [ ] **Agent2**: block_pruning_service.c lock bug (CRITICAL)
- [ ] **Agent2**: boot_index.c scan race protection (CRITICAL)
- [ ] **Agent2**: fread/fwrite audit + documentation
- [ ] **Agent3**: Memory RSS health check
- [ ] **Agent3**: Structured boot timing
- [ ] **Agent3**: bg_hash_verify SIGSEGV investigation
- [ ] **Agent3**: Address backfill SIGSEGV investigation

---

## Agent Assignments

| Agent | Focus | Critical Files |
|-------|-------|----------------|
| Agent2 | Thread safety: fix 2 race conditions + audit | `block_pruning_service.c`, `boot_index.c` (scan fns), `disk_block_io.c` |
| Agent3 | Observability + crash investigation | `node_health_service.c`, `boot.c`, `bg_hash_verification_service.c`, `boot_index.c` (backfill) |

---

## Coordination Rules

- Run `make -j$(nproc) && make test` before every push
- Commit with `wave 22/22b task N:` prefix
- `git pull origin master` before starting any task
- File boundaries are strict — see each AGENT file
