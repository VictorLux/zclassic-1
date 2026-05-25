# Revised cutover plan — "prove offline, decouple from bodies, flip atomically"

> Supersedes the symptom-chasing in `wt-c3-reflip.md`. Result of a 3-agent deep
> review (2026-05-25). The old plan kept stalling; this makes the stalls
> structurally impossible.

## The flaw in the current plan (all three reviews agree)

The cutover **entangles "is the new consensus path correct?" with "is this one
live node well-connected and is its hand-patched datadir intact?"** The shadow
stages are fed ONLY by blocks arriving live at the tip (`shadow_feeder` is called
only from `msg_blocks.c` at the active tip); the historical chain 0→tip-1 is
never replayed through them. So the correctness proof is a *side effect of live
operation* and inherits every defect of the live node's state:

- wedge #1/#2 = a boot-time flip ordering hazard (live-state, not logic),
- peer-floor block = a *connectivity* property gating a *correctness* milestone,
- today's `disk-read-failed` = a torn cold-import datadir, not consensus.

Meanwhile the new path is **already proven bit-clean over 3.1M headers**
(`validate_headers_log`: 3,124,225 ok). The proof keeps getting held hostage by
one node's operational state. Per-stage 24h live soaks (×8) make it a fragile
multi-week serial path any live perturbation resets.

## The revision — four moves, each kills a recurring symptom by construction

### 1. Prove correctness OFFLINE over the full chain  (the keystone)
Build a `replay-proof` driver that replays every block **0→tip** through the new
stage pipeline against a **complete, read-only archive** (the local `zclassicd`'s
data, hardlinked read-only — the same substrate `bench_fresh_sync` already uses),
asserting **0 divergences**: `validate_headers failed_total==0`,
`utxo_apply g_delta_diverged_total==0`, `tip_finalize g_utxo_count_diverged_total==0`,
and a final `diff_with_legacy_shadow [0..tip] == CONVERGED`. Emits one hard
artifact: *"0 divergences across N blocks, commit <sha>, datadir <fingerprint>."*
This is independent of peer count and of any torn live state, and becomes a CI gate.

> **Correction (important):** do NOT use Phase 6 (`make chaos` / seed_tape) for
> this — `tools/sim/chaos.c` uses STUBBED consensus (counter bumps), it cannot
> prove block equivalence. The real primitives already exist:
> `adapters/inbound/src/shadow_feeder.c` (intake), `application/operations/src/
> diff_with_legacy_shadow.c` (byte-exact range diff), the per-stage divergence
> counters, and `validate_headers_stage_set_validator` (injectable). The only new
> piece is a thin driver that feeds 0→tip from disk instead of waiting for P2P.

### 2. Decouple header validation from block BODIES
`validate_headers` must verify Equihash from the **persisted `nSolution`** (it is
in the LevelDB `disk_block_index` entry and the event log) — NOT by reading the
full block from disk. The in-memory index drops `nSolution` to save RAM
(~4GB/3M), so the post-restart fallback reads the body → `disk-read-failed`. Add a
"load nSolution from LevelDB by hash" path and drop the body read. This **unblocks
C-3 with no storage repair** AND is mandatory for the 30s FlyClient/SHA3 cold-sync
vision (a snapshot-synced node has no bodies — header validation must not need them).

### 3. Make `BLOCK_HAVE_DATA` a read-verified invariant; retire cold-import
`BLOCK_HAVE_DATA` may only be set after a **successful read-back** (a
`block_index_set_have_data(bi, datadir)` helper all paths must use). cold-import
currently bulk-copies the flag without ever reading the bytes → it can (and did)
claim data that isn't readable (a recurrence of the documented body-pull
pathology). Add a self-heal Condition `have_data_unreadable` (clear flag → refetch).
Strategically: cold-import is a **verification-free band-aid**; the canonical
bootstrap is **FlyClient + SHA3 snapshot** (hash-verifies every chunk + SHA3-commits
before activation). Fence/retire cold-import; prove the cutover against the same
complete-data substrate cold-sync will trust.

### 4. Flip atomically behind the canary + guard (not per-stage soaks)
Once the offline proof is green, flip the stages in **one guarded session** on a
single healthy, data-complete, well-connected node — each behind the one-block
canary + the `cutover_no_forward_progress` guard (keep both; they cover live edge
cases the offline proof can't: reorgs, concurrent admission, at-tip kill-9 order).
Demote the 24h-per-stage soak to a **post-flip** confidence/deletion gate. Collapses
a multi-week serial chain into one session, with the heavy correctness banked offline.

## Net

Stop making block bodies available so validation can read them; instead **prove
the path correct off the live node, make validation not need bodies, and forbid
the index from lying about having them.** The wedge / peer-block / disk-read
symptoms all stem from one root — correctness entangled with live state — and all
three vanish. Correctness is proven over the full chain on a complete archive;
connectivity (peer floor) still gates the final *live flip*, which is correct.

## Critical files
- new driver → reuses `adapters/inbound/src/shadow_feeder.c`,
  `application/operations/src/diff_with_legacy_shadow.c`, `tools/bench_fresh_sync.c` launch model
- `app/services/src/validate_headers_stage.c` (drop body-read fallback; load nSolution from LevelDB)
- `lib/storage/src/block_index_db.c` (+`block_index_set_have_data` helper, nSolution loader)
- `app/services/src/legacy_bootstrap_importer.c` (read-verify before HAVE_DATA, or fence)
- `app/conditions/src/` (+`have_data_unreadable` self-heal condition)
