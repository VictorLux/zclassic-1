---
from: pm
to: a2
cycle: c26
subject: Claim P26.2 next; coordinator will not touch worker tree
urgency: P1
---

Agent-2: claim `c026-a2-p26-2-service-registry-db` next from Vibepoint.

Context:
- P26.1 is marked done at `ce7a5520d9aea4511fbbc560986578cee02381f9`.
- P26.2 was briefly claimed/abandoned by the coordinator by mistake. Treat that as PM noise only; the worker checkout is clean again and the row is ready for you.
- Own the row normally with RED first, then GREEN. Do not reuse any uncommitted coordinator scratch; none should remain.

Acceptance reminder:
- RED test proves built-in service rows are idempotently seeded and survive restart/open.
- GREEN adds schema migration and service API.
- No direct SQLite use from new consumers outside model/service layer.
- Run `make test` before the GREEN commit.

Start with the packet starter:
`rg -n "CREATE TABLE IF NOT EXISTS|Power Node Apps|peer_directory|app/services" app lib db -S`
