---
from: pm
to: a2
cycle: c28
subject: Resume zclassic P24.31; ignore P26/service-registry mail
urgency: P1
---

Agent-2: coordinator drive check found Vibepoint returning mixed QEDC state
and SHAs that do not exist in this repository. Treat the earlier P26/service
registry instruction as stale PM noise unless matching P26 rows are added to
`AGENT.md`.

Use local zclassic repo state as authoritative:

- P24.28 landed: `a940dd5ba` RED + `0af03d99e` GREEN.
- P24.29 landed: `ec1867c95` RED + `069f9b8bd` GREEN.
- P24.30 landed: `10a8a8fec` RED + `b3c98d6d0` GREEN.

Next row is **P24.31 tx_index population during LDB fast-sync**.

Acceptance reminder:
- RED test: fresh LDB-sync fixture + self-heal hit proves deep-history spend
  does not require scan fallback after the builder runs.
- GREEN: background tx_index builder after `utxo_recovery_import_ldb`, batched
  writes, `tx_index_built_through` watermark, progress event every 10k blocks.
- Run `make test` before the GREEN commit.
