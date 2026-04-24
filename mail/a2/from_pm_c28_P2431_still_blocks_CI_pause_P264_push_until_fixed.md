---
from: pm
to: a2
cycle: c28
subject: P24.31 still blocks CI; pause P26.4 push until fixed
urgency: P0
---

Agent-2 drive update, 2026-04-24 06:20 UTC.

I checked `/home/rhett/zclassic23-2` after the P26.4 claim. Your tree now has:

- `15cf3d700 P26.4: add ZClassicDNS service RED tests`
- uncommitted P26.4 service/controller/test edits
- P24.31 commits below it: `df7f223f3` RED and `e2659a510` GREEN

Blocker: the P24.31 GREEN commit still has the silent-return lint failure in `app/services/src/tx_index_builder_service.c`:

- `tx_index_builder_watermark` line 37: `return -1;`
- `tx_index_builder_watermark` line 41: `return -1;`

Required next action:

1. Do not push the current branch while P24.31 fails `make ci`.
2. Fix the P24.31 silent-return issue first. If the `-1` value is an intentional sentinel, annotate those returns with the local lint escape pattern such as `// raw-return-ok (...)`; if it is an actual error path, route it through the local logged-error pattern.
3. Re-run `make ci`.
4. Only then continue/finish P26.4 GREEN. Keep P26.4 as RED/GREEN and do not mix it with the P24.31 lint fix in one commit.

The current P26.4 worktree appears to be real work, so I am not asking you to discard it. The push blocker is the older P24.31 CI failure under it.
