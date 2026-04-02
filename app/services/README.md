# Services Layer

`app/services` is the application orchestration layer.

Use it for:

- sync workflows
- snapshot lifecycle
- wallet indexing/rescan orchestration
- health/status aggregation
- explorer query aggregation
- peer policy

Do not use it for:

- raw P2P protocol parsing
- consensus rules
- direct HTML/JSON rendering
- route dispatch

Current services in progress:

- `block_sync_service`
- `header_sync_service`
- `node_health_service`
- `sync_service` (compatibility umbrella)
- `snapshot_sync_service`

Primary architecture reference: [ARCHITECTURE.md](/home/rhett/zclassic23/ARCHITECTURE.md)
