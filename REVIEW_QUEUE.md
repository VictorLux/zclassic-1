# AGENT1 Review Queue

Items delivered by AGENT2 / AGENT3 that need AGENT1 expert review before
going further. AGENT1 processes this queue at the start of each
coordination pass.

## Format

```
| ID | Date       | Source   | Item                           | Kind          | Status    |
```

Kinds: `investigation` | `proposal` | `fix-sketch` | `api-change`
Statuses: `PENDING` | `IN-REVIEW` | `APPROVED` | `REJECTED` | `DELEGATED`

---

## Pending

| ID | Date       | Source | Item                             | Kind          | Status  |
|----|------------|--------|----------------------------------|---------------|---------|
| 1  | 2026-04-11 | agent2 | `PHGR13_INVESTIGATION.md` (343L) | investigation | DELEGATED |

### Notes

**#1 PHGR13 investigation** — AGENT2 identified two independent consensus
bugs in `lib/sapling/src/bn254.c` that together cause every historical
joinsplit to fail PHGR13 verification. Root cause: VK file parser assumes
wrong format + G2 decompressor misreads `Fq2` FE2IP encoding.

**Disposition (wave 8):** DELEGATED back to AGENT2 with a self-certify
escape hatch. See `WAVE_8.md` AGENT2 item: AGENT2 may commit the fix
directly if both bugs are addressed, a `test_phgr13_fix.c` is added, the
live node connects at least one block beyond h=2,014,948, and the commit
references this investigation by filename.

If the self-certify path fails (live node doesn't advance), AGENT2
reverts and this row moves back to `PENDING` for a hands-on AGENT1 read.

---

## Done

*(Move completed rows here with disposition and date.)*
