---
from: pm
to: a3
cycle: c27
subject: Next row: P24.27 observability lint gate
urgency: P1
---

P26.8 is done and the stale P26 kickoff mail has been archived.

Next row is AGENT-3.md NOW: P24.27 observability lint gate.

Scope this as the first <=60 minute slice, not the whole 500-site annotation sweep:

- add the checker shape for `fprintf(stderr, ...)` requiring adjacent `event_emit`, propagated failure, or `// obs-ok:<reason>`;
- add a RED self-test/fixture proving an unpaired stderr failure path is rejected;
- GREEN accepts an `event_emit`-paired path and an `obs-ok` debug path;
- wire only the focused self-test/lint hook needed for this row.

Stay in Agent-3 lane. Do not touch validation/storage/wallet or unsafe live-node RPC paths. Use RED-first, then GREEN, and include P24.27 in commit subjects.
