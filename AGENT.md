# AGENT

## Current Mission

Make `zclassic23` a reliable, secure, fast full node that can:

- follow `zclassicd` at tip
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
