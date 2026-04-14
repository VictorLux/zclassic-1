# AGENT — Wave 23: Fix Block Download Stall (Coordinator)

## Current Mission

Node is STUCK at height 2,016,355. Headers arrive (2025K+) but blocks are never downloaded. Fix the three compounding bugs.

---

## Status (2026-04-14)

### Completed (Wave 22)
- [x] LOG_FAIL spam fix
- [x] before_save hooks: mempool_entry, tx_index, wallet_tx
- [x] fprintf→LOG_ERR migration
- [x] block_pruning_service.c lock race fix
- [x] boot_index.c scan race protection
- [x] fread/fwrite audit + documentation
- [x] Memory RSS health check + uptime
- [x] Structured boot timing
- [x] bg_hash_verify SIGSEGV fix (cs_main lock)
- [x] Address backfill SIGSEGV fix (mmap_size=0)
- [x] Tests: ALL 95 STORIES PASSED

### In Progress (Wave 23)
- [ ] **Agent2**: Bug 1 — collect_needed_blocks pprev==NULL walk termination
- [ ] **Agent2**: Bug 2 — should_begin_blocks_download state gate
- [ ] **Agent3**: Bug 3 — false AT_TIP in activation controller
- [ ] **Agent3**: Re-enable bg_hash_verify + address backfill
- [ ] **Agent3**: Update CHECKLIST.md

---

## Root Cause Analysis

Headers arrive but blocks never queue because:
1. `syncsvc_collect_needed_blocks` walk terminates at `pprev==NULL` → count=0 → no getdata
2. `syncsvc_should_begin_blocks_download` requires `SYNC_HEADERS_DOWNLOAD` but state is already `SYNC_BLOCKS_DOWNLOAD`
3. `activation_request_connect` declares AT_TIP unconditionally after activate_best_chain returns true

---

## Agent Assignments

| Agent | Focus | Critical Files |
|-------|-------|----------------|
| Agent2 | Download pipeline: collect_needed_blocks + state gate | `header_sync_service.c` |
| Agent3 | Activation controller + re-enable fixed features | `chain_activation_controller.c`, CHECKLIST.md |
