# Convergence backlog — the zclassic23 way

Source: tree-wide read-only audit (9 agents, 2026-06-05). Ranked by
value-over-risk. Each item is a SAFE convergence action (DRY / DOC / API /
SHAPE). High-risk consensus-sensitive items are quarantined to §12 and need
repro-on-copy before any edit.

**Discipline:** mutating work runs as edit-only workflows on DISJOINT file
sets, then a single build + `make lint` (35 gates) + `test_parallel` (0/N) +
boot-smoke gate, then commit per logical group. Never grow a baseline, never
weaken a gate, never touch `config/src/boot.c`.

## Wave status

| Wave | Items | State |
|------|-------|-------|
| boot-index decompose | `config/src/boot_index.c` → shape-clean units | in flight (`wjb4k7nac`) |
| Convergence Wave 1 | #1+#11, #2, #3, #6, #7+#8, #9 | in flight (`w37tgxfpv`) |
| Convergence Wave 2 | #4, #5, #10 | queued |
| Consensus-sensitive | §12 (×4) | deferred — repro-on-copy each |

## Items

1. **`coins_alloc` latent NULL-deref + lying return** *(API / real bug, low risk)* —
   `lib/coins/src/coins.c`. On OOM it logs, falls through, derefs NULL `vout`,
   and `return true` masks failure from 3 callers
   (`coins.c:65`, `coins_view_sqlite.c:784`, `utxo_projection.c:674`) whose
   `if (!coins_alloc(...))` guards are dead code. Fix: `num_vout=0; return false;`
   before the null-init loop. **→ Wave 1.**
2. **Document `check_block.h` gate booleans** *(DOC, low)* — four must-never-fork
   entry points expose undocumented "disable a safety check" bools. Lift the
   fast-sync prose from `check_block.c`. **→ Wave 1.**
3. **Unify 3 duplicate `bytes32_nonzero` onto `zcl_chainwork_is_zero`** *(DRY, low)* —
   `chain_evidence_authority_service.c:99`, `chain_evidence_snapshot.c:17`,
   `snapshot_manifest.c:19`. Leave `snapshot_offer.c` (own NULL-guard contract). **→ Wave 1.**
4. **Fix fabricated `nonce` / `confirmations` in block-header serializer** *(API, low)* —
   `blockchain_controller_blocks.c:170/162` emits `nonce:0`, `confirmations:1`
   for every block; consumer `api_controller_lookup.c:102` reads nonce as a
   string. Emit real `uint256_get_hex(nNonce)` string + `1+tip-height`. **→ Wave 2.**
5. **Unify `json_extract_int/real` controller wrappers** *(DRY, low)* — two
   byte-identical wrapper pairs re-adapting the shared `zcl_json_extract_*`,
   ~44 call sites, un-prefixed names leaking via internal headers. Add
   `zcl_json_int/real` to `views/format_helpers.h`; fold in the duplicate
   `get_difficulty`/`explorer_get_difficulty`. **→ Wave 2.**
6. **Document 13 of 15 condition headers** *(DOC, low)* — symptom→remedy→witness
   →cadence, mirroring `tip_fork_stale.h`. The condition registry is the live
   self-heal surface. **→ Wave 1.**
7. **One `stage_block_reader_fn` typedef for 4 reducer stages** *(DRY, low)* —
   four identical typedefs + setter bodies onto one `stage_default_block_reader`;
   alias to keep public names. **→ Wave 1.**
8. **Delete 3rd `cursor_persisted` copy in reorg path** *(DRY, low)* —
   `utxo_apply_delta_reorg.c:73` → `stage_cursor_persisted`. Call site is before
   `BEGIN IMMEDIATE` (no double-lock, verified). **→ Wave 1.**
9. **`condition_reset_state` primitive + `operator_needed_emitted` leak** *(DRY +
   test-isolation, low)* — only 4 of 12 `_test_reset()` clear the field; 8 leak
   operator-needed state between tests. Centralize in `lib/framework`. **→ Wave 1.**
10. **Table-drive the staged-sync supervisor** *(SHAPE, low but load-bearing)* —
    8 cloned per-stage blocks (~390 LOC) differing only by symbol → desc table +
    one generic register. Live liveness wiring; do after the mechanicals. **→ Wave 2.**
11. **Document `coins.h` lifecycle invariants** *(DOC, low)* — "empty record means
    OOM, not pruned." Rides with #1. **→ Wave 1.**

## §12 — Consensus-sensitive (deferred, repro-on-copy each, do NOT batch)

- **`disconnect_block` unused `state` param** (`connect_block.c`) — route reorg
  failures through `validation_state_*`/`REJECT_FATAL` for symmetry, or drop the
  param. Reorg path.
- **`connect_block` 16× duplicated cleanup** — consolidate the `free(checks)/…/
  block_undo_free` sites into one `goto cleanup:`. Reject reasons + DoS scores
  must stay byte-identical.
- **CCoins avail-mask decode duplicated** (`coins_db.c:104-131`,
  `chainstate_legacy_reader.c:109-136`) — extract one
  `ccoins_decode_avail_mask(...)`. On-disk format parse.
- **`legacy_mirror_sync_request_catchup` dual surface** — invert so the worker
  returns `zcl_result` directly (Law 2); keep `bool` as a thin adapter.
- **`update_coins` silently accepts an empty coins record on OOM**
  (`lib/validation/src/update_coins.c:98`) — `coins_from_transaction` is `void`
  and leaves `num_vout=0` on OOM, but `num_vout=0` is ALSO legitimate for an
  all-OP_RETURN/unspendable tx, so the caller cannot distinguish OOM from a
  valid empty record. The real fix is to give `coins_from_transaction` a status
  return (and propagate it through `update_coins` → `connect_block`); a naive
  `if (num_vout==0) fail` guard would false-reject valid txs. Consensus connect
  path — **repro-on-copy**, scope the `coins_from_transaction` signature change
  carefully. (Surfaced by the Wave-1 `coins_alloc` review; the live NULL-deref
  sibling at `coins_db.c:133` was fixed in Wave 1.)

## Deferred — peer_scoring typed-API adoption (needs enum extension, owner-gated)

Round-3 #1 (adopt typed `peer_scoring_record()` across ~34 raw
`peer_misbehaving(...,N,...)` sites in `lib/net/src/msgprocessor*.c`,
`msg_compact.c`) was ATTEMPTED and REVERTED. Two naive approaches both fail:
- **Map by meaning** → silently changes ban WEIGHTS (raw 20→INVALID_MESSAGE=10
  halved; raw 50→INVALID_BLOCK=100 doubled). A behavior change to a
  security-relevant DoS surface — must not land silently. (Caught by review.)
- **Map by weight (1:1)** → preserves behavior but mis-NAMES: a weight-20
  snapshot *parse* error becomes `PEER_OFFENCE_FLOOD` (the only weight-20 enum),
  which is a worse lie than the raw number.

Root cause: the enum has only 4 weight buckets (INVALID_MESSAGE/UNREQUESTED=10,
FLOOD=20, INVALID_HEADER=50, INVALID_BLOCK=100), with no name for the
snapshot/transport/proof rejection classes the snapshot path uses at weights
20/50/100. **The right fix EXTENDS `peer_scoring.h`** with semantically-accurate
offences at the SAME weights (e.g. `INVALID_SNAPSHOT=20`, a 50-weight
swarm-chunk class, `INVALID_PROOF=100` for flyclient/SHA3/merkle verification),
then maps each site to the enum that is BOTH weight-preserving AND honestly
named. That is enum design on a DoS-policy surface → owner-gated; the
`msg_blocks.c:540` dynamic-`dos` site (graded 1..49) needs a parametric record,
not a constant enum. Until then the raw `peer_misbehaving` calls stay — they do
not misrepresent the category.

## boot_services.c decomposition plan (2513 LOC → target the shutdown TU last)

Seam map (read-only audit `w6755v1wu`). Extract order by risk:

**Wave A (SAFE — independent, non-consensus, not in shutdown body):**
- `boot_sd_watchdog.c` (~110) — owns g_sd_watchdog_id/ctx, zero shared state. *(in flight)*
- `boot_node_utilities.c` (~130) — app_add_node, metrics start/stop, sync-state logger. *(in flight)*
- `boot_bg_verification.c` (~60) — bg-validation/hash-verify start/stop; re-checks finalized history, not the connect path. *(in flight)*

**Wave B (consensus-adjacent but NOT in shutdown body — boot-smoke validates):**
- `boot_runtime_sync_services.c` (~200) — header_probe / legacy_mirror / gap_fill /
  zclassicd_oracle / rolling_anchor start/stop wrappers. Stop ordering is preserved
  via the kernel spec table (they are not in the shutdown sequence body). Move
  byte-identical; boot-smoke on a copy.

**Wave C (needs prep first):**
- `boot_frontend_services.c` (~470, biggest payoff) — BLOCKED: shares the profile
  statics `boot_profile_has_explorer/store/onion` (13 read sites incl. stayers) and
  `boot_configure_frontend_rpc` is called from app_init. First promote the 3 trivial
  profile accessors to a shared header + make boot_configure_frontend_rpc public, THEN
  extract.
- `boot_flyclient_mmb.c` (~175, consensus) — BLOCKED: `g_mmb_leaf_store` is an extern
  global shared with boot_snapshot_offer.c + read in app_init's MMB-build block; a lint
  gate asserts msgprocessor_snapshot.c does NOT reference it. Extract only together with
  the MMB-build block, and update the gate's expected-owner file.

**HIGH-RISK (do ALONE, with a real SIGTERM stop/restart proof on a datadir COPY — boot-smoke CANNOT validate this):**
- The shutdown section (boot_services.c lines ~2168-2407): shutdown_stop_frontend_services,
  shutdown_persist_fast_restart_state, shutdown_flush_coins_to_sqlite,
  shutdown_quiesce_network_and_flush_coins, shutdown_persist_runtime_state,
  shutdown_release_owned_resources, app_shutdown_svc. The ordering invariant lives here:
  coins.db COMMIT (emergency flush + quiesce) MUST precede block_index fsync / flat-file
  save / block_tree+node_db close. See [[feedback_at_tip_kill9_ordering_invariant]].
- `boot_catchup_job.c` (~35) — small but `boot_join_catchup_service` is called from
  shutdown_persist_runtime_state, so it touches the SIGTERM teardown. Low payoff, not
  worth the shutdown risk until the shutdown TU is being done anyway.

## Dropped / opportunistic (pick up inside the relevant wave, not standalone)

`gap_fill_service` stale doc + descent swap; `stage_helpers.h` roster comment;
`msgprocessor` seen-ring unification; `sapling_keys` LE32 swap;
`zcashconsensus.h` / coins-decode docs. The `sprout_viewing_key_to_address`
placeholder: doc-only edit is safe; defer the `pk_enc = scalarmult_base(sk_enc)`
wiring as a separately-scoped change (derivation semantics).
