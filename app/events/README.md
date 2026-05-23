# app/events/

**Shape:** Event — typed append-only message.

Each file in `src/` declares typed events using `EVENT(...)` from
`lib/framework/event_log.h`. Events are emitted by Models (AR
after-save hooks) and Jobs. Subscribers (observers, MCP tail, audit log)
receive events asynchronously.

In Phase 4, events become the append-only substrate from which all
projections (formerly: separate sqlite tables) are derived. Today they
fan out to in-memory observers and the `node.log` event log.

See [`docs/FRAMEWORK.md`](../../docs/FRAMEWORK.md) § 3.7 for the
contract and § 7 cookbook.

Existing event primitives at `lib/event/` will move (or get re-exposed)
through `lib/framework/event_log.h` over Phases 1-4.
