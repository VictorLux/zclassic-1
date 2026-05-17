# Power-Node Event Taxonomy Audit

Audit result for service registry, onion gateway, ZClassicDNS, and
hosted-service health visibility. This is a proposed event contract only; it
does not change consensus, P2P wire format, wallet behavior, or storage
behavior.

## Current Coverage

Existing power-node observability is strong for chain, validation, boot,
database, MCP dispatch, disk, mempool, peer policy, and crash/recovery paths.
The event enum and name table live in `lib/event/include/event/event.h` and
`lib/event/src/event.c`.

The audited power-node surfaces are currently visible only indirectly:

- `service_registry`: boot and service ownership paths in
  `config/src/boot_services.c` start worker services, but there is no generic
  service lifecycle event.
- `hosted-service health`: `app/services/src/node_health_service.c` computes
  bounded health snapshots for status surfaces, but health state changes are
  not emitted as event transitions.
- `onion route serving`: `lib/net/src/onion_service.c` handles `/`,
  `/status`, `/directory`, `/directory.json`, `/search`, `/explorer`, `/store`,
  `/blog`, and fallback routes, but request routing and rate limits are not
  visible in the event log.
- `onion directory publishing`: `populate_directory_from_chain()` and
  `register_self()` update the peer directory, but publish/load outcomes are
  log-only or silent.
- `ZClassicDNS`: `app/controllers/src/name_controller.c` resolves and
  registers ZNAM names, but successful lookups, misses, validation failures,
  ready-to-publish OP_RETURNs, and broadcasts are not emitted as typed events.

## Minimal Additions

Add these events after `EV_SAPLING_PERSIST_FAIL` and before `EV_NUM_TYPES`.
Add matching string names in `event_type_name()`.

| Enum | Name | Payload | Primary callsites |
|---|---|---|---|
| `EV_SERVICE_STATE_CHANGE` | `service.state_change` | `service=NAME from=STATE to=STATE reason=TEXT` | `config/src/boot_services.c` service start/join wrappers |
| `EV_SERVICE_HEALTH_CHANGE` | `service.health_change` | `service=NAME from=STATE to=STATE reason=TEXT` | `app/services/src/node_health_service.c` when snapshot health/degraded state changes |
| `EV_ONION_REQUEST` | `onion.request` | `method=METHOD route=ROUTE code=N bytes=N dur_us=N` | `lib/net/src/onion_service.c:onion_service_handle_request()` after each route handler returns |
| `EV_ONION_DIRECTORY_PUBLISH` | `onion.directory_publish` | `source=self|chain status=ok|failed count=N reason=TEXT` | `lib/net/src/onion_service.c:populate_directory_from_chain()` and `register_self()` |
| `EV_NAME_RESOLVE` | `name.resolve` | `name=NAME type=TYPE found=true|false reason=TEXT` | `app/controllers/src/name_controller.c:rpc_name_resolve()` and `rpc_name_resolve_api()` |
| `EV_NAME_PUBLISH` | `name.publish` | `name=NAME type=TYPE status=ready|broadcast|rejected txid=HEX size=N reason=TEXT` | `app/controllers/src/name_controller.c:rpc_name_register()` |

These six events cover the requested visibility without adding per-controller
event families or duplicating existing `EV_MCP_REQUEST`, chain, or validation
events.

## Emission Notes

`EV_SERVICE_STATE_CHANGE` should be emitted only at lifecycle edges: service
start success/failure, stop requested, and joined. It should not be emitted by
tight service loops. Suggested states are `stopped`, `starting`, `running`,
`stopping`, and `failed`.

`EV_SERVICE_HEALTH_CHANGE` should be edge-triggered from the health snapshot,
not emitted on every collection. Suggested states are `healthy`, `degraded`,
and `unhealthy`. The first implementation can track process-global previous
states for `node`, `db_service`, `sync`, `tor`, and `onion`.

`EV_ONION_REQUEST` should normalize routes before emission to avoid unbounded
payloads and high-cardinality query strings. Suggested route labels are `/`,
`/status`, `/directory`, `/directory.json`, `/search`, `/explorer`, `/store`,
`/blog`, and `fallback`. Rate-limit responses should use `code=429`.

`EV_ONION_DIRECTORY_PUBLISH` should report chain directory load counts and
self-registration outcomes. It should not include private keys, filesystem
paths, or full SQL errors beyond a compact reason label.

`EV_NAME_RESOLVE` should distinguish `found=false reason=not_found` from
`found=false reason=invalid` and `found=false reason=db_unavailable`. It should
include the resolved target type only when a record exists.

`EV_NAME_PUBLISH` should fire for validation rejection, existing-name
rejection, manual OP_RETURN readiness, and wallet broadcast success/failure.
It should not include the target value for shielded or transparent payment
addresses; the name, type, status, txid, size, and reason are enough for
operator diagnostics.

## Acceptance Checklist

- Add enum entries and comments in `lib/event/include/event/event.h`.
- Add all six names in `lib/event/src/event.c:event_type_name()`.
- Add focused unit coverage that each new enum maps to the expected name.
- Emit service lifecycle events from `config/src/boot_services.c` without
  changing service ownership or thread behavior.
- Emit edge-triggered service health events from
  `app/services/src/node_health_service.c`.
- Emit onion request and directory publish events from
  `lib/net/src/onion_service.c`.
- Emit name resolve and publish events from
  `app/controllers/src/name_controller.c`.
- Keep payloads compact structured text under `EVENT_PAYLOAD_SIZE`.
- Do not change serialized block/transaction format, consensus constants,
  P2P wire format, wallet behavior, or storage behavior.
