# Wave 30 Task 2: Fix sync_service test failure

## The Failure

```
sync_service refuses needed-block collection for forked headers... FAIL (!result.chains_from_tip)
```

This is the only test failure out of 2470 tests. It's pre-existing (not from recent changes).

## What To Do

1. Find the test — grep for "refuses needed-block collection for forked headers" in `lib/test/`
2. Read the test, understand what `chains_from_tip` means and why the assertion fails
3. Check `app/services/src/sync_service.c` and related header sync code
4. Fix either the test expectation or the sync_service logic
5. `make -j$(nproc) && make test` — verify 0 FAIL lines
6. Commit with descriptive message

## Context

- The sync_service handles block download scheduling during IBD (initial block download)
- "forked headers" means headers that branch off the main chain
- `chains_from_tip` likely indicates whether the block collection starts from our current chain tip
- The test expects the service to refuse collecting blocks for forked headers, but the assertion `!result.chains_from_tip` fails — meaning chains_from_tip is true when it shouldn't be

## Build & Test

```bash
make -j$(nproc)
make test    # should show 0 FAIL
```
