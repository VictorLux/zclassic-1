# Power Node Architecture Contract

This spec defines the power-node contract surface. It is intentionally a
contract document only: implementations may add adapters later, but this file
does not authorize app, network, storage, or daemon code changes.

## File Anchors

- Service registry: `app/include/app/services.h`, `app/services.c`
- Service-layer examples: `app/services/register_binding_service.c`,
  `app/services/store_address_service.c`, `app/services/test_guard_service.c`,
  `app/services/pipeline_observer_service.c`
- MCP base controller and route surface:
  `tools/qedc_mcp/controllers/application_ctrl.h`,
  `tools/qedc_mcp/controllers/*_ctrl.c`, `tools/qedc_mcp/routes.c`
- Event model and audit log: `tools/qedc_mcp/models/event.h`,
  `tools/qedc_mcp/models/event.c`,
  `tools/qedc_mcp/controllers/events_ctrl.c`
- Current agent/control-plane protocol: `docs/MCP_KANBAN.md`,
  `docs/MCP_COORDINATION.md`, `docs/MCP_ARCHITECTURE_NEXT.md`

If a named power-node component has no code anchor yet, the implementation
must land behind one of these existing surfaces or update this contract first.

## node_state_api

`node_state_api` is the read-only state contract for a local power node. It
must expose a single coherent snapshot with these fields:

- `node_id`: stable local identifier, never derived from a private key in logs.
- `sync_state`: one of `offline`, `starting`, `syncing`, `ready`, `degraded`,
  or `stopping`.
- `services`: the service names and health states registered in
  `service_registry`.
- `network`: onion gateway and ZClassicDNS reachability summary, with no raw
  secret material.
- `permissions`: effective permission scope for the caller.
- `events_head`: newest event id or timestamp visible to the caller.

The API is side-effect free. Any mutation must go through an explicit service
action and must emit an event as described below.

## service_registry

`service_registry` extends the existing central services pattern in
`app/include/app/services.h`. The registry is the only authority for whether a
power-node service is available, degraded, or disabled.

Required invariants:

- Each service has a stable snake-case name, lifecycle state, health summary,
  and permission scope.
- Registration is idempotent across repeated initialization.
- A service may depend on another service only by name, not by storing raw
  implementation pointers from another module.
- A disabled or degraded service remains visible in `node_state_api`; it is not
  silently omitted.

The initial service names reserved by this contract are `node_state_api`,
`onion_gateway`, `zclassicdns`, `mcp_surface`, and `event_stream`.

## onion gateway

The onion gateway is the network privacy boundary. It may advertise local
reachability and accept routed requests, but it must not own wallet secrets,
consensus state, or persistent application data.

Required invariants:

- The gateway is off by default unless configuration explicitly enables it.
- It reports only reachability state through `node_state_api`.
- It routes service calls through `service_registry` and permission checks.
- It emits lifecycle and access-denied events without recording hidden-service
  private keys, client addresses, or request bodies containing secrets.

## ZClassicDNS

ZClassicDNS maps ZClassic-facing names to power-node service endpoints. It is a
name-resolution contract, not an authority to bypass permissions.

Required invariants:

- Resolution returns a typed target: onion endpoint, local MCP endpoint, or
  unavailable.
- Each answer carries freshness metadata and the resolving service name.
- Names are normalized before lookup and preserved verbatim only in bounded
  audit fields.
- A resolved target still passes through the onion gateway or MCP surface
  permission checks before it can invoke a service.

## MCP surface

The MCP surface follows the existing controller pattern anchored by
`tools/qedc_mcp/controllers/application_ctrl.h` and the route registration in
`tools/qedc_mcp/routes.c`.

Required invariants:

- Every power-node MCP route has a named controller action and before-filter
  chain.
- Write actions require an explicit write permission and must validate body
  fields through a typed model or equivalent structured validator.
- Read actions may expose `node_state_api` snapshots, service health, and event
  summaries, but not secrets or unrestricted local filesystem paths.
- Route names must be stable and documented before dependent agents or clients
  are allowed to rely on them.

## permissions

Permissions are checked before a request reaches a service implementation.
Power-node permissions are capability strings with these reserved scopes:

- `node.read`: read `node_state_api` and service health.
- `node.admin`: start, stop, or reconfigure registered services.
- `network.read`: read onion gateway and ZClassicDNS health.
- `network.admin`: change onion gateway or ZClassicDNS configuration.
- `events.read`: read event summaries.

The default caller has no write permissions. Permission failures must return a
structured error through the caller's surface and must emit an event.

## event expectations

Power-node events use the audit-log shape in `tools/qedc_mcp/models/event.h`:
model name, action, row id, bounded payload, and timestamp. The payload is a
small JSON snapshot of facts needed for audit, not a copy of the request body.

Required events:

- `PowerNode.start`, `PowerNode.stop`, and `PowerNode.degraded`
- `Service.register`, `Service.health_change`, and `Service.disabled`
- `OnionGateway.enabled`, `OnionGateway.disabled`, and
  `OnionGateway.access_denied`
- `ZClassicDNS.resolve`, `ZClassicDNS.stale_answer`, and
  `ZClassicDNS.access_denied`
- `McpSurface.call`, `McpSurface.validation_failed`, and
  `McpSurface.permission_denied`

Events must be append-only from application code. Redaction happens before
event insertion, and every event payload must be bounded to the event model's
payload capacity.

## Non-Goals

- No wallet custody or key-management semantics are defined here.
- No peer-to-peer consensus behavior is defined here.
- No storage schema is defined beyond event expectations.
- No network listener is required until a later implementation task updates
  this contract with the concrete adapter.
