# Power Node Architecture Contract

This file is a compatibility pointer for older links.

The canonical zclassic23 power-node contract is
[`../spec/power-node-contract.md`](../spec/power-node-contract.md). It anchors
the current code paths for:

- `node_state_api`
- `service_registry`
- onion gateway
- ZClassicDNS / ZNAM
- MCP surface
- permissions
- event expectations

Current zclassic23 anchors:

- Service boundary: `app/services/README.md`
- App MCP tools: `tools/mcp/controllers/app_controller.c`
- Event taxonomy: `lib/event/include/event/event.h`

Do not add stale cross-project paths or standalone service registry plans here.
Update the canonical contract instead.
