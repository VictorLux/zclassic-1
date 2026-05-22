# ZClassic23 Architecture

*Canonical architecture doc. Supersedes `ARCHITECTURE_DIAGRAMS.md` (kept for the Mermaid boot diagrams). Pivoted 2026-05-22 to the Personal Sovereignty Stack vision — see [`adr/0001-personal-sovereignty-stack.md`](adr/0001-personal-sovereignty-stack.md).*

---

## North Star

> **The database is the only truth. Every byte in the process is a derived projection that can be torn down and rebuilt from disk in bounded time. Chain progress is a stage cursor on disk — it either advances or names a typed blocker.**

Every other choice is a consequence of that sentence. Wedges become unreachable by construction because there is no in-memory state whose loss matters: a stalled actor restarts, reads its cursor, and resumes. Bugs become a 64-bit seed and an event trace in a deterministic simulator.

---

## The Layer Cake (destination)

```
L7  Operator plane          MCP (200+ tools), CLI, JSON-RPC, block explorer, wallet UI
L6  Service supervisors     chain_sup, net_sup, mempool_sup, wallet_sup, feature_sup,
                            onion_sup, op_sup — restart trees + liveness contracts
L5  Domain actors           8 chain stages (header_admit → tip_finalize), peer actors,
                            mempool, wallet, znam_index, zmsg_inbox, market_book,
                            swap_state_machine, games_engine, onion_publisher
L4  Storage engines         blocks.log, headers.kv, utxo.lsm, consensus.log, wallet.db,
                            mempool.snap, progress.kv — one writer actor per store
L3  Kernel primitives       supervisor, blocker, boot_phase, event, AR lifecycle,
                            stage, mailbox, projection, reset_state, retry, Result<T,E>
L2  Platform                clock, rng, io_uring, arena — all injectable (sim-ready)
L1  ABI                     libsapling (in-tree), libsqlite, libsecp256k1, libtor,
                            libsodium, libleveldb — all vendored static
```

Every layer's interface to the layer above is read-only data plus typed commands; every layer's relationship to the layer below is a single ownership pointer that survives restart.

---

## Layer-by-layer mapping (current shipped code → target)

### L1 — ABI (vendored, static)

| Module | LOC | Status |
|--------|----:|--------|
| `lib/sapling/` (hand-written pure-C zk-SNARK math) | 13,765 | bedrock |
| `vendor/sqlite/`, `vendor/libsodium/`, `vendor/libsecp256k1/`, `vendor/tor/` | n/a | bedrock |
| `vendor/dcrdex/` (atomic-swap HTLC reference) | n/a | bedrock |

### L2 — Platform abstractions

| Module | LOC | Status |
|--------|----:|--------|
| direct `clock_gettime` / `getrandom` call sites scattered through tree | — | TRANSFORM |
| `lib/platform/clock.{c,h}`, `lib/platform/rng.{c,h}` | 0 | **MISSING — Wave F-5** |
| arena allocator + io_uring runtime | 0 | **MISSING — Wave T** |

### L3 — Kernel primitives

| Module | LOC | Status |
|--------|----:|--------|
| `lib/util/src/supervisor.c` (Round 5 — root of liveness tree) | 423 | bedrock |
| `lib/util/src/blocker.c` (Round 6 — typed blocker primitive) | 515 | bedrock |
| `lib/util/src/boot_phase.c` (boot stage state machine) | 182 | bedrock |
| `lib/event/src/event.c` (event ring) | 945 | bedrock |
| `app/models/include/models/activerecord.h` (AR lifecycle) | included in 9,167 LOC model tree | bedrock |
| `lib/util/src/stage.{c,h}` (cursor + idempotent step) | 0 | **MISSING — Wave F-2** |
| `lib/util/src/mailbox.{c,h}` (bounded MPSC, drop-policy) | 0 | **MISSING — Wave F-3** |
| `lib/util/src/projection.{c,h}` (MVCC snapshot handle) | 0 | **MISSING — Wave F-4** |

### L4 — Storage engines

| Module | LOC | Status |
|--------|----:|--------|
| `lib/storage/` (SQLite schema, migration, coins_view_sqlite) | 4,024 | TRANSFORM — single-writer actor per store (Wave S) |
| `app/services/src/block_index_loader.c` (hand-rolled ORM) | 677 | REPLACE → `headers.kv` writer actor |
| `adapters/outbound/persistence/` (hexagonal scaffolding) | 1,430 | TRANSFORM — becomes the L4 writer-actor surface |
| `blocks.log` append-only + `progress.kv` LMDB | 0 | **MISSING — Wave S-1, S-5** |
| `utxo.lsm` (SHA3-snap every 1024) | 0 | **MISSING — post-Wave S** |
| `consensus.log` append-only audit trail | 0 | **MISSING — Wave S** |

### L5 — Domain actors

| Module | LOC | Status |
|--------|----:|--------|
| `lib/net/src/connman.c` | 1,991 | TRANSFORM → `peer_pool` actor under `net_sup` |
| `lib/net/src/msgprocessor*.c` (handshake, snapshot, inv, sync, ping) | ~7,000 | TRANSFORM → per-peer actors |
| `lib/net/src/fast_sync.c` + `flyclient.c` | 2,060 | bedrock (extends into stage `header_admit`) |
| `lib/net/src/onion_service.c` (embedded Tor + .onion HTTPS) | 868 | bedrock → L5 actor under `onion_sup` |
| `app/services/src/chain_advance_coordinator.c` (wedge engine) | 1,715 | REPLACE → 8 staged-sync actors + thin source selector |
| `app/services/src/legacy_mirror_sync_service.c` (blocking-RPC monolith) | 1,406 | REPLACE → async supervised state machine |
| `application/`, `domain/`, `mutator/` (hexagonal skeleton) | 502 | TRANSFORM — becomes the staged-pipeline actor bodies |
| 8 chain stages: header_admit, validate_headers, body_fetch, body_persist, script_validate, proof_validate, utxo_apply, tip_finalize | 0 | **MISSING — Wave S-2..S-9** |
| ZNAM, ZMSG, ZSLP, ZSWP, Market, games, explorer actors | various | TRANSFORM — each becomes one isolated L5 actor under `feature_sup` |

### L6 — Service supervisors

| Module | LOC | Status |
|--------|----:|--------|
| `lib/util/src/supervisor.c` (Round 5 root + 3 children) | 423 | bedrock |
| `config/src/boot_services.c` (kitchen-sink boot orchestrator) | 3,003 | REPLACE → service registry + per-actor `startup_fn` |
| `config/src/boot.c` (CLI parse + boot driver) | 3,293 | TRANSFORM — shrinks to `for_each(actors) sup.start(actor)` |
| `chain_sup`, `net_sup`, `mempool_sup`, `wallet_sup`, `feature_sup`, `onion_sup`, `op_sup` | 0 (one root supervisor today) | **MISSING — Wave S-10 onward** |

### L7 — Operator plane

| Module | LOC | Status |
|--------|----:|--------|
| `tools/mcp/` (router, 6 controllers, middleware, metrics, replay) | 5,790 | bedrock — 93 tools live, target 200+ |
| JSON-RPC server (`lib/net/src/https_server.c` + 85+ RPC handlers) | included | bedrock |
| Block explorer + MVC web framework over .onion | included in `app/controllers/` | TRANSFORM — read-only over L5 projections |
| Wallet UI consent panel (Wave M-5) | 0 | **MISSING — Wave M** |
| `wallet_agent`, `inbox_agent`, `market_agent` MCP controllers | 0 | **MISSING — Wave M** |

---

## What stays bedrock (do not propose deleting these)

The following modules earn their keep and are the foundation the destination architecture is built **on top of**, not against:

- `lib/util/src/supervisor.c` — root of the liveness tree (Round 5).
- `lib/util/src/blocker.c` — typed blocker primitive (Round 6).
- `lib/util/src/boot_phase.c` — boot stage state machine.
- `lib/event/src/event.c` — event ring.
- `app/models/include/models/activerecord.h` — AR lifecycle (every write goes through it).
- `tools/mcp/` (router + controllers + middleware + metrics + replay) — 93 tools is a feature, not a problem.
- `lib/sapling/` — hand-written pure-C zk-SNARK math, 13.7K LOC, no Rust dep.
- `lib/net/src/onion_service.c`, `connman.c`, `fast_sync.c`, `msgprocessor_snapshot.c`, `flyclient.c`, `p2p_game.c`, `dandelion.c`.
- All 17 lint gates (see `DEFENSIVE_CODING.md`).
- The hexagonal scaffolding (`ports/`, `adapters/`, `application/`, `domain/`, `mutator/`) — recent work; transforms into the staged-sync L4 storage surface and the parallel-verifier diff harness.
- `lib/storage/src/schema_migration.c`.

See [`adr/0001-personal-sovereignty-stack.md`](adr/0001-personal-sovereignty-stack.md) for the full rationale on why "build forward, transform in place" replaced the earlier "audit-first" framing.

---

## What we're building

Active roadmap lives at `~/.claude/plans/zclassic23-plan.md`. Current waves:

| Wave | Theme | Status |
|------|-------|--------|
| **F** | Foundation: ~2,500 LOC pure-delete purge + `stage` / `mailbox` / `projection` / `platform.clock` / `platform.rng` kernel primitives + this doc + ADR-001 | in progress |
| **S** | Staged sync — the wedge-extinction wave. 12 milestones (`progress.kv` → 8 stages → `chain_sup` → `zcl_diff_with_legacy_staged` → cutover). Wedge class extinct when the diff is zero for 30 days. | next |
| **M** | Claude as participant — consent log + `wallet_agent` / `inbox_agent` / `market_agent` MCP controllers + wallet UI consent panel. | queued |
| **Z** | ZNAM as decentralized identity — standalone resolver lib, `zcl-resolve` CLI, `.well-known/zcl-name` over onion, formal DID spec. | queued |
| **T** | Deterministic simulator — single-thread actor scheduler, FS/net/clock injection, first scenario ports the current wedge as a regression test, first TLA+ spec. | queued |
| **R** | Release engineering — `flake.nix`, signed tarballs with cosign, first real CI workflow. | queued |

See the plan for per-milestone DoD, files-to-touch, and the "where I left off" markers.

---

## What this architecture buys us

1. **Wedges unreachable by construction.** Chain progress is a number on disk; a stage either advances or names a typed blocker. No silent stalls in the state space.
2. **Bugs become seeds.** Deterministic simulator means every failure reduces to a 64-bit seed + event trace; reproduce, fix, prove with a property test.
3. **Claude as co-pilot, not observer.** 200+ MCP tools + consent log + agent controllers turn the node into a first-class AI operator surface.
4. **AI-native operator plane.** No curl, no token waste, no manual SSH; all introspection and control flows through typed MCP tools with replay buffer and Prometheus metrics.
5. **One binary, one onion, one stack.** Sovereign personal computing surface — bank + identity + inbox + market + swap + web host + games — on a $200 SBC behind Tor.
