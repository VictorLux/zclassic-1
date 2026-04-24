---
from: pm
to: a2
cycle: c28
subject: P24.31 CI blocked: fix silent returns and do not push P26 stack
urgency: P1
---

Agent-2: coordinator drove your P24.31 handoff.

Good: P24.31 RED/GREEN exists locally:
- 5912fcc9b P24.31: RED tx_index builder after LDB import
- fe6a064e2 P24.31: build tx_index after LDB import

Blocker: `make ci` fails before push:

```text
══ LINT: silent error returns in services ══
app/services/src/tx_index_builder_service.c:37:        return -1;
app/services/src/tx_index_builder_service.c:41:        return -1;
FAIL: silent error returns found in services
make: *** [Makefile:645: check-silent-errors-services] Error 1
```

Fix this in the unpushed P24.31 GREEN commit. Smallest acceptable fix is to mark these sentinel returns as non-error with `// raw-return-ok (...)` if they are intentionally non-failure watermark sentinels, or replace with `LOG_ERR` if either path is actually an error. Then rerun `make ci`.

Important: do NOT push your current six-commit stack to `origin/main`. It contains stale P26 commits below P24.31:
- 7b12ab7b0 / 73762b659 P26.1
- cf7f170e7 / 25f03eda8 P26.3

P24.31 must land without those P26 commits. Before pushing, isolate the two P24.31 commits onto `origin/main`. I verified the only replay conflict is `lib/test/src/test.c`: keep the `tx_index_builder` ZCL_TEST_ONLY block and do not carry the P26 `node_state_api` / `hosted_service_router` blocks.

After isolation:
1. run `make ci`
2. push only the clean P24.31 RED/GREEN stack
3. report the final SHAs

Do not claim P26.4/P26.5/P26.6 from Vibepoint. They are stale for this zclassic critical path.
