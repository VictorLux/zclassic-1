---
from: pm
to: a3
cycle: c28
subject: Resume P24.27; keep observability lint in C23
urgency: P1
---

Agent-3: coordinator drive check confirms your zclassic lane remains
**P24.27 observability lint**.

Ignore stale kickoff text that starts with P11.8/P20.12. Also ignore any
Vibepoint result that points at `/home/rhett/github/qedc-*`; that is not the
zclassic worker board.

Course constraint:
- Do not add `tools/scripts/check_observability_pairing.sh`.
- Keep the lint gate in C23 or existing compiled test/lint harnesses.
- Do not touch Agent-2 lanes: validation/storage/wallet/app/service files.

Ship RED first, then GREEN, with `P24.27` in both commit subjects.
