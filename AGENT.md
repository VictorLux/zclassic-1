# AGENT

## Current Mission

Make `zclassic23` a reliable, secure, fast full node that can:

- follow `zclassicd` at tip
- bring a fresh node to the real network tip head block quickly
- restart cleanly under systemd user services
- bring up a fresh peer quickly and safely
- use secure fast-sync paths deterministically
- fail closed instead of leaving partial DB state behind

This file is the active work document.
It replaces the old spread of overlapping refactor checklists and logs.

## Canonical Docs

Keep these as the primary repo docs:

- `README.md`
  public entry point, quick start, operator overview
- `ARCHITECTURE.md`
  target runtime shape and long-term boundaries
- `API.md`
  RPC/HTTP surface and operator-facing interfaces
- `AGENT.md`
  current engineering work, current risks, next steps
- `VISION.md`
  optional product/strategy document

Everything else should either be folded into one of the files above or deleted.

## Current Facts

As of `2026-04-04`:

- `zclassic23.service` is now running the current repo-built binary on disk
- `zclassic23` and `zclassicd-rhett` were observed at the same height
- recent hardening passes have made most major DB mutation paths fail closed
- full suite currently passes:
  - `make -j4 test_zcl`
  - `./test_zcl`
  - result: `ALL TESTS PASSED (0 failures)`

## Highest-Priority Runtime Findings

Fresh-node probe results exposed the remaining important runtime bugs:

1. Fresh `zclassic23` probe node immediately used the secure file-service path.
   It downloaded `6.3 GB` in about `10s` over SHA3-secured channels.
2. The probe also received a secure snapshot offer and completed FlyClient/MMB
   verification successfully.
3. The secure snapshot apply then failed locally with:
   `cannot start a transaction within a transaction`
4. The sync state machine also logged:
   `BUG: sync illegal transition headers_download -> snapshot_receive`
5. After that failure, the probe remained stalled at height `587` instead of
   converging to tip.

That means the current blocking issue is not cryptographic verification.
It is orchestration and transaction ownership after verification succeeds.

Latest hardening pass:

- snapshot receive setup is now atomic:
  - `BEGIN` happens before UTXO wipe
  - setup failure rolls back instead of leaving a half-reset UTXO table
- snapshot reset now attempts to roll back any still-open receive transaction
- sync state now enters `snapshot_receive` only when receive actually begins,
  not earlier at offer-accept time
- sync transition table now explicitly allows verified snapshot takeover from:
  - `headers_download`
  - `blocks_download`
  - `connecting_blocks`
- new tests cover:
  - router contract update for offer acceptance
  - sync transition into `snapshot_receive` from header sync
- current verification after this pass:
  - `make -j4 test_zcl`
  - `./test_zcl`
  - result: `ALL TESTS PASSED (0 failures)`

## Active Priorities

### Priority 1: Fix Secure Fast-Sync End To End

Goal:

- a fresh node reaches current tip automatically from an empty datadir

Required outcomes:

- FlyClient/MMB verification passes
- snapshot apply does not hit nested transaction errors
- replay resumes cleanly from the verified snapshot
- sync state transitions are legal and deterministic
- no manual restart or operator intervention is needed

Concrete work:

1. trace transaction ownership in snapshot receive/apply/finalize
2. make transaction ownership single-source
3. remove nested `BEGIN` behavior across DB service wrappers and snapshot code
4. move sync-mode transition authority behind one supervisor path
5. add a fresh-node regression harness for:
   - file bootstrap
   - FlyClient/MMB verification
   - snapshot apply
   - delta replay
   - final convergence to tip

### Priority 2: Clean Up Sync-State Ownership

Goal:

- one owner decides sync mode and allowed transitions

Required outcomes:

- no illegal transition logs
- `headers_download`, `snapshot_receive`, replay, and normal block sync do not
  race to own the state machine
- failures produce explicit degraded-state outputs

Concrete work:

1. define legal state transitions centrally
2. move enter/exit hooks out of incidental message handlers
3. ensure failed snapshot paths unwind back to a valid sync mode
4. add tests for transition legality under mixed fast-sync and normal sync

### Priority 3: Finish Final Audit Of DB Write Paths

Goal:

- no remaining silent partial-success paths in boot/sync/import flows

Current status:

- most high-risk surfaces are already hardened
- remaining work is smaller consistency cleanup

Concrete work:

1. continue sweeping smaller boot/runtime write paths
2. keep write ownership explicit
3. log and fail closed on persistence failures
4. avoid nested transaction assumptions

## Tip Convergence Checklist

Requirement:

- a brand-new node must reach the live network tip quickly after bootstrap,
  not just replay to height `587`

Current live reference:

- `zclassic23` observed at `3066276`
- `zclassicd-rhett` observed at `3066276`

Checklist:

- [x] Main service must run the current repo-built binary
  Evidence: `/proc/<pid>/exe` now matches the current on-disk inode after each
  controlled restart.

- [x] Fresh bootstrap receiver must complete secure file sync quickly
  Evidence: recent probes downloaded `6.3 GB` in `10s` to `15s` over the
  SHA3-secured file-service path.

- [x] Fresh bootstrap receiver must replay to the post-snapshot local tip
  Evidence: recent probes consistently replay to `587` in `6s` to `7s`.

- [x] Peer-state takeover into snapshot receive must be legal during live sync
  Evidence: the earlier `syncing_headers -> snapshot_receiving` peer-state bug
  is fixed and regression-tested.

- [x] Verified snapshot activation must tolerate missing exact tip-index entry
  Evidence: activation now falls back to the highest indexed block at or below
  the offered height; regression-tested.

- [x] Shared SQLite read statements must not poison later `coins_flush`
  Evidence: the earlier `SAVEPOINT coins_flush failed ... SQL statements in
  progress` failure is gone after explicit statement resets; regression-tested.

- [x] Receiver must actually process inbound `zsnapshot` offers after replay
  Evidence:
  - fresh `probe12` logs `completed msg 'zsnapshot'`
  - it enters `negotiating` and generates a FlyClient challenge

- [x] Receiver must complete FlyClient/MMB verification after replay handoff
  Evidence:
  - fresh probes now reach the verified snapshot path reliably enough to begin
    secure receive instead of stalling before offer handling
  Close condition tightened:
  - continue keeping an eye on end-to-end replay handoff after verification

- [x] Receiver must request and consume snapshot data after verification
  Evidence:
  - fresh `probe12` reaches repeated `zsnapdata` and `zsnapend`

- [x] Receiver must advance from replay tip `587` toward live tip after
  snapshot handoff
  Evidence:
  - fresh `probe12` advanced past the old ceiling and reached `800`
  - after the stale-offer guard pass, the same probe continued further to
    `2080`

- [ ] Fresh receiver must converge all the way to current network tip
  Evidence:
  - latest stable reference tip was `3066276`
  - no fresh probe has yet demonstrated complete automatic convergence on the
    current build lineage
  Close when:
  - fresh probe reaches the same height and best block as `zclassic23` and
    `zclassicd-rhett`
  - no local errors or illegal transitions occur during the run

- [ ] Fast-sync path needs a deterministic regression harness
  Evidence:
  - current validation still depends on manual systemd probe runs
  Close when:
  - there is a repeatable automated or scripted fresh-node matrix covering:
    - file sync only
    - file sync plus replay
    - snapshot offer receipt
    - FlyClient/MMB verification
    - snapshot receive/finalize
    - convergence to current tip

## Documentation Cleanup Plan

### Keep And Improve

- `README.md`
  remove stale claims and keep it operator-focused
- `API.md`
  make it the clean source of truth for RPC/HTTP usage, auth, ports, and
  current behavior
- `ARCHITECTURE.md`
  keep long-term runtime boundaries and desired layering
- `AGENT.md`
  keep this current, short, and execution-oriented

### Purge Or Fold

These were useful during exploration but are now redundant:

- `CONSISTENCY_CHECKLIST.md`
- `REFACTOR_CHECKLIST.md`
- `REFACTOR_LOG.md`
- `SYNC_HARDENING_PLAN.md`
- `CLAUDE.md`

Rule:

- if a document is not canonical, current, and actively used, remove it
- if information is still valuable, move it into one canonical doc first

## Repo Cruft Policy

Remove files when they are all of:

- clearly generated or obsolete
- not referenced by build or runtime
- not the active service binary
- not needed for current debugging or deployment

Obvious classes of cruft:

- superseded planning docs
- stale backup binaries like `zclassic23.old`
- one-off temporary probe/service artifacts after investigation is complete

Do not delete casually:

- the active `zclassic23` binary
- files currently used by linger services
- data directories unless the task explicitly calls for it

## Definition Of Done

This refactor is done when:

1. `zclassic23` follows `zclassicd` at tip reliably
2. a fresh third node can securely reach tip without operator help
3. FlyClient/MMB verification and snapshot apply both succeed in one flow
4. restart/shutdown under systemd user services is boring and deterministic
5. no major boot/sync/import path silently leaves partial DB state
6. repo docs are reduced to a small, current, high-signal set

## Latest Progress

- connection ownership is cleaner:
  - `connect_node()` now de-dupes by remote service even for addnode and
    localhost connects
  - this removes the old duplicate localhost socket storm that was polluting
    fresh sync probes and splitting one-shot fast-sync negotiation across
    multiple connections
- secure snapshot receive ownership is tighter:
  - receive mode no longer drops indexes like generic IBD turbo mode
  - receive begin/reset now cleanly owns rollback and mode restore
- fresh bootstrap receivers now defer non-critical background DB work:
  - local snapshot/export builder
  - address backfill
  - store payment processor
- live probe result on the current working tree:
  - FlyClient/MMB verification succeeds
  - `snapshot_receive` starts cleanly
  - the prior `cannot start a transaction within a transaction` failure is gone
  - the prior `schema[37] failed: database is locked` failure is gone
  - `zsnapdata` now streams in repeated bursts after secure handoff
- verified snapshot activation is now more robust:
  - if the offered snapshot-tip hash is not in `map_block_index` yet,
    activation now falls back to the highest local indexed block at or below
    the offered height
  - this mirrors the existing boot-time chainstate fallback pattern instead of
    leaving the node pinned at height `587`
  - a dedicated unit test now covers the fallback activation path
- current verification after this pass:
  - `make -j4 zclassic23`
  - `make -j4 test_zcl`
  - `./test_zcl`
  - result: `ALL TESTS PASSED (0 failures)`
- live probe progress is now materially better:
  - fresh `probe12` no longer dies at `587`
  - it processes `zsnapshot`, generates a FlyClient challenge, receives
    `zsnapdata`, reaches `zsnapend`, and advances to `800`
  - the prior `SHA3 FAILED` after secure snapshot receive pointed to a stale
    offer/cache race on the serving side
  - sender-side serving now binds each peer to the exact snapshot generation
    it was offered, and stale requests/serve sessions are re-offered instead
    of serving bytes for a newer cache generation
  - after that guard, `probe12` continued further to `2080` without the old
    `SHA3 FAILED` / `bad-txns-inputs-missingorspent` signature reappearing in
    the latest journal sweep
- block relay dedupe is now safer during snapshot handoff:
  - `process_block_msg()` now defers block dedupe until after snapshot-sync
    suppression instead of marking a block as "seen" while it is still being
    intentionally ignored
  - this closes a real late-sync hazard where a block first received during
    `snapshot_receive` could be cleared from in-flight, dropped, and then
    ignored forever when re-requested after the handoff
  - a dedicated regression test now proves a snapshot-deferred block is still
    accepted when replayed later
- current verification after the block-dedupe fix:
  - `make -j4 test_zcl`
  - `./test_zcl`
  - result: `ALL TESTS PASSED (0 failures)`
- fresh bootstrap startup is now leaner:
  - a node with no usable local chain data now defers local file-service
    serving, snapshot-offer building, store payment scanning, and address
    backfill during bootstrap receiver mode
  - this removes wasted first-minute work from the secure fast-sync path and
    avoids the old `bind port 18034 failed` / local snapshot export churn on
    brand-new receivers
  - live probe on the rebuilt `zclassic23` binary now shows:
    - `Fresh bootstrap receiver mode: deferring local serve/build work`
    - `File service server deferred during fresh bootstrap receiver mode`
    - `Fast sync offer build deferred during bootstrap receiver mode`
    - `Store payment processor deferred during bootstrap receiver mode`
    - secure `zsnapshot`/FlyClient negotiation still starts normally
- current verification after the bootstrap deferral pass:
  - `make -j4 zclassic23`
  - `make -j4 test_zcl`
  - `./test_zcl`
  - result: `ALL TESTS PASSED (0 failures)`
- duplicate snapshot offers are now gated more cleanly:
  - the message router now ignores fresh `zsnapshot` offers while snapshot
    negotiation/receive/verify already owns the receiver lifecycle
  - this removes the earlier live bug where a later offer could try to drive
    the service back from `receiving` into `negotiating`
  - a direct unit test now covers the ignore policy for duplicate offers
- live probe result after the offer-gate fix:
  - fresh probe still shows normal `zsnapshot` and `zfcproofs` flow
  - secure receive still begins normally
  - the earlier `BUG: snapsync illegal receiving -> negotiating (accepted offer)`
    line no longer appears in the recent probe log
- current remaining blocker:
  - fresh-node convergence to the real live tip is still not finished
  - the remaining work is in the later live sync path after secure handoff,
    not in the earlier offer/transaction/SQLite ownership failures that were
    blocking progress before
    - the remaining stall is now later and narrower: a fresh node can still
      get stranded after secure handoff without reaching the real network tip
    - the next work is continuing live probe iteration from the post-handoff
      block-processing path, not undoing earlier snapshot ownership fixes

## Current Commands

Useful verification commands:

```bash
make -j4 test_zcl
./test_zcl
systemctl --user --no-pager --full status zclassic23.service
systemctl --user --no-pager --full status zclassicd-rhett.service
```

Useful runtime comparison:

```bash
/home/rhett/bin/zclassic-cli -datadir=/home/rhett/.zclassic getblockcount
curl --silent --user "$(cat /home/rhett/.zclassic-c23/.cookie)" \
  --data-binary '{"jsonrpc":"1.0","id":"height","method":"getblockcount","params":[]}' \
  -H 'content-type: text/plain;' http://127.0.0.1:18232/
```
