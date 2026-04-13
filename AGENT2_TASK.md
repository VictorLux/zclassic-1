# Agent 2 Task: Header Sync Stall Detection & Recovery

## Problem

The node gets stuck in `headers_download` state indefinitely. Right now it's stuck at height 2,015,124 while peers are at 3,077,062. The stall recovery code in `block_sync_service.c` only handles block download stalls. There is NO detection or recovery for header sync stalls.

Peers receive 1GB+ of data but headers don't advance — responses are silently rejected or peers never respond to `getheaders`.

## Files to read first

- `CLAUDE.md` and `DEFENSIVE_CODING.md` — mandatory coding rules
- `app/services/src/header_sync_service.c` — header sync logic, getheaders interval/backoff
- `app/services/src/block_sync_service.c` — stall recovery pattern to follow
- `lib/net/src/msgprocessor.c` lines 4109-4146 (periodic getheaders) and 1764-1963 (process_headers)
- `lib/net/include/net/download.h` — timeout constants
- `lib/net/include/net/net.h` — p2p_node struct fields

## Tasks

### 1. Per-peer header response tracking

Add fields to `struct p2p_node` (in `lib/net/include/net/net.h`):
- `int64_t last_useful_headers_time` — last time this peer delivered accepted headers
- `uint64_t total_headers_delivered` — lifetime count of accepted headers from this peer

Update `process_headers()` in msgprocessor.c to set these when `accepted > 0`.

When a peer hasn't delivered useful headers in 120s during IBD (`syncsvc_is_initial_block_download` returns true), disconnect it with a log message. Check this in the periodic send loop (~line 4109).

### 2. Header sync stall detection

In the periodic send loop (msgprocessor.c ~4109), add:
- Track `pindex_best_header->nHeight` and a timestamp of last advance
- If `sync_state == SYNC_HEADERS_DOWNLOAD` and `pindex_best_header` hasn't advanced in 120s:
  - Log: `"HEADER STALL: best_header stuck at %d for %llds, disconnecting worst peer"`
  - Find the outbound peer with lowest `total_headers_delivered` and disconnect it
  - This makes room for a new outbound connection that might work better

### 3. Header request from inbound peers as fallback

Currently `syncsvc_should_request_headers()` skips inbound peers (`if (node->inbound) return false`). During a header stall (no advance in 120s), allow requesting headers from inbound peers too. Add a parameter or check sync state to enable this fallback.

### 4. Tests

Write tests in `lib/test/src/test_header_sync_stall.c` following `test_header_sync.c` and `test_sync_service.c` patterns:
- Test that `last_useful_headers_time` is updated on accepted headers
- Test that stall detection fires after 120s with no advance
- Test that inbound fallback activates during stall
- Register tests in `lib/test/src/test.c`

## Rules

- Follow `DEFENSIVE_CODING.md`: use `LOG_FAIL()`, `zcl_malloc()`, `log_macros.h`
- Run `make test` before committing — all tests must pass
- Commit with descriptive messages
- Do NOT touch files unrelated to this task
