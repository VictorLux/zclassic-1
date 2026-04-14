# AGENT — Wave 22: Defensive Hardening (Coordinator)

## Current Mission

Harden `zclassic23` with Rails-way patterns, thread safety, and defensive coding.

**Node is AT TIP and synced.** This wave is about reliability, not features.

---

## Status (2026-04-14)

- Node: running, 4GB RAM, synced to tip
- Tests: 1 mystery failure (fast_sync PoW LOG_FAIL spam masks it)
- Architecture audit complete — gaps identified, work assigned

---

## Agent Assignments

| Agent | Focus | Files |
|-------|-------|-------|
| Agent2 | Thread safety fixes + raw allocator cleanup | `disk_block_io.c`, `block_pruning_service.c`, `boot_index.c`, `connect_block.c`, `update_coins.c`, `process_block.c`, `msg_headers.c` |
| Agent3 | LOG_FAIL spam fix + before_save hooks + fprintf→LOG_ERR migration | `fast_sync.c`, `mempool_entry.c`, `tx_index.c`, `wallet_tx.c`, services with bare fprintf |

---

## Critical Findings from Audit

### Thread Safety (Agent2 — CRITICAL)
1. `block_pruning_service.c:160-165` — lock released BEFORE `unlink()`, g_cached_file can point to deleted file
2. `boot_index.c` scan called from P2P thread (`msg_headers.c:412`) without file lock while writer active
3. `open_disk_file` has no internal locking — relies entirely on caller discipline

### LOG_FAIL Spam (Agent3 — HIGH)
- `fast_sync_verify_pow()` calls LOG_FAIL on every failed nonce (~1M lines)
- Masks real test failures in `make test` output

### Missing Before-Save Hooks (Agent3 — MEDIUM)
- `mempool_entry`, `tx_index`, `wallet_tx` go through AR lifecycle but no before_save guard

### Raw Allocators (Agent2 — MEDIUM)
- `connect_block.c:592`, `update_coins.c:51`, `process_block.c:822,1100`, `msg_headers.c:271`
- Use raw `malloc`/`realloc` with bare `fprintf` instead of `zcl_*` + `LOG_ERR`

### fprintf vs LOG_ERR (Agent3 — LOW)
- ~10 service sites use `fprintf(stderr,...)` instead of structured `LOG_ERR`

---

## Coordination Rules

- Run `make -j$(nproc) && make test` before every push
- Commit with `wave 22 task N:` prefix
- `git pull origin master` before starting any task
- File boundaries are strict — see each AGENT file
