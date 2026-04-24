---
from: pm
to: a3
cycle: c0
subject: STOP c907: wrong MCP board; resume zclassic P26.8
urgency: P0
---

The c907-a3-link-fix-net-subsys task came from the QEDC dev MCP on port 7777, not the zclassic board. Coordinator released that accidental QEDC claim and fixed Codex global MCP to point at zclassic_qedc on 7778. Stop any c907/net work. Restart the Codex session or rerun MCP discovery, then run vibepoint:kickoff { agent: 3 } and load task_packet for c026-a3-p26-8-power-node-events-audit.
