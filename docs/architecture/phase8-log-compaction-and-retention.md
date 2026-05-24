# Phase 8 — Event-log compaction & retention

**Status:** PLAN (draft 2026-05-24)
**Phase:** 8 (after Phase 4e lands block bodies in the event log)
**Depends on:** Phase 4 complete (4e in particular — block bodies migrated
into the log). Pairs with the existing SHA3 UTXO snapshot + FlyClient
cold-sync machinery.

> The event log is append-only by design. Append-only is not the same as
> *keep-everything-forever*. Phase 8 is how the log stops growing without
> bound once it has become the single source of truth.

---

## Why this exists

Phase 4 makes one append-only `event.log` the source of truth, with N
rebuildable projections derived from it. Phase 4e then appends **all
block bodies** into that log (~6.3 GB on a synced node — see
[`phase4-storage-unification.md`](./phase4-storage-unification.md) § 4e).
After 4e the event log effectively *is* the entire blockchain plus the
full history of every UTXO add/spend, mempool admit/remove, peer
observation, etc. — and it only grows.

The current primitive
([`lib/storage/include/storage/event_log.h`](../../lib/storage/include/storage/event_log.h))
has exactly one form of truncation: **torn-write recovery** (backward
scan from EOF, drop a partial trailing event). That is a durability
mechanism, not a retention mechanism. There is no compaction, rotation,
or pruning anywhere in Phases 4–7. Phase 8 fills that gap.

**Key distinction — two things people conflate:**

- **Projections are already disposable.** `utxo_projection`,
  `block_index_projection`, etc. can be rebuilt from the log at any time.
  They are *not* the growth problem.
- **The log itself grows unbounded.** That is the only thing Phase 8
  addresses.

---

## What is safe to prune (three classes)

1. **Spent-UTXO churn.** An `EV_UTXO_ADD` later cancelled by a matching
   `EV_UTXO_SPEND` is dead weight for any projection rebuilt going
   forward. This is the classic log-structured **compaction** target:
   the net UTXO set at height H is far smaller than the sum of all adds
   and spends that produced it.
2. **Block bodies below a finality / snapshot height.** Immutable
   history. Not required to keep projections live going forward.
   Prunable for non-archive nodes; **archive nodes opt to keep them**
   (a runtime mode, see Retention policy below).
3. **Ephemeral events.** `EV_TX_ADMIT_MEMPOOL` / remove,
   `EV_HODL_SNAPSHOT`, and similar — no value once superseded by a
   confirmed block or a later snapshot.

Consensus-relevant immutable facts that anyone may need to re-verify
(block headers, the canonical chain of work) are **never** pruned below
the snapshot horizon without an explicit archive opt-out.

---

## Mechanism: checkpoint event + segmentation

Two design constraints come straight from the Phase 4a primitive:

- The wire format is **frozen** (`event_log.h` threading note → Phase 7a
  io_uring). Phase 8 must not change the per-event encoding.
- The fsync-sentinel backward-scan invariant assumes **one contiguous
  file**. You therefore **cannot delete from the middle** of a single
  log. Pruning must operate at a coarser granularity.

### Checkpoint event

Introduce an `EV_CHECKPOINT` event: "projection state as of height H is
X, with SHA3 = …". Once a checkpoint is durable, every event strictly
before H that is not consensus-immutable-history becomes droppable,
because a fresh reader can start from the checkpoint instead of replaying
from genesis.

This is the **same primitive as cold-sync, viewed from the other end**:
cold-sync *bootstraps* a node from a SHA3 UTXO snapshot + FlyClient;
Phase 8 *prunes* a node down to the same kind of snapshot. They should
share the snapshot serialization and the SHA3 commitment code, not
reimplement it.

### Segmentation (recommended) vs. compaction-rewrite

- **Segmentation (recommended).** Split the log into ordered segment
  files (`event.000001.log`, `event.000002.log`, …). A segment is sealed
  at a checkpoint boundary. Pruning = drop whole sealed segments below
  the retention horizon. Keeps the frozen wire format and the
  per-file sentinel recovery story intact; deletion is an atomic
  `unlink`, never an in-place rewrite.
- **Compaction-rewrite (alternative).** Stream-rewrite live events into
  a fresh log, then atomically swap. Simpler conceptually but doubles
  peak disk during compaction and has a more delicate crash window.

Recommend **segmentation** as the default; compaction-rewrite can be a
later optimization for reclaiming churn *within* a retained segment.

---

## Retention policy (runtime modes)

- **Archive** — keep everything. No pruning. Default for explorer /
  seed nodes.
- **Default** — retain block bodies + churn back to the most recent
  durable checkpoint horizon (e.g. last snapshot + a finality margin);
  keep all headers and chain-of-work forever.
- **Pruned** — retain only the minimum to serve the node's own wallet
  and validate the tip; rely on peers for deep history.

Mode is an operator flag; switching from a more-retentive to a
less-retentive mode triggers a one-shot prune pass at next checkpoint.

---

## Sub-phases (sketch — to be split into per-PR work docs)

- **8a — `EV_CHECKPOINT` event + checkpoint emitter.** Reuses the SHA3
  UTXO snapshot/commitment code. Shadow-only at first (emit, verify, do
  not prune).
- **8b — Segmented log.** Teach the event_log primitive to roll
  segments at checkpoint boundaries; readers stream across segments
  transparently. Recovery scan runs per-segment.
- **8c — Prune pass + retention modes.** Drop sealed segments below the
  retention horizon per the operator's mode. Diff-tool verification that
  a pruned node + peers can still rebuild every projection.
- **8d — Cold-sync convergence.** Unify the prune-to-snapshot and
  cold-sync-from-snapshot paths onto one snapshot serialization.

Each sub-phase gets a `docs/work/wt-phase8-*.md` assignment when queued.

---

## What this does NOT do

- Does NOT change the per-event wire format (frozen since 4a).
- Does NOT change consensus rules. Retained data is byte-identical.
- Does NOT prune headers or chain-of-work below the snapshot horizon
  unless an operator explicitly runs in `pruned` mode.

---

## Status

**PLAN (draft)** — gated on Phase 4e (block bodies in the log). Not yet
split into per-PR work docs. This document captures the design decision
and the segmentation-vs-rewrite tradeoff so the gap is recorded before
4e makes the log the sole source of truth.
