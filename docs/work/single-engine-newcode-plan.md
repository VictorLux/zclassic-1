# Single-engine new code — the feed-before-delete plan

The legacy `coins.db`/connect engine cannot simply be deleted: it is the **sole**
producer of three things the surviving Wave-S reducer consumes but does not yet
produce. Deleting it first leaves the stages spinning `JOB_IDLE` on an empty map.
Design source: [`single-engine-newcode-design.json`](./single-engine-newcode-design.json).

## The cycle (why order matters)

All 8 stages READ the in-memory `map_block_index` via `active_chain_at()` and gate
on `nStatus` bits. Today the **only** producers are legacy delete-targets:

- `add_to_block_index()` (`process_block_core.c:213`) — creates in-mem entries (header scalars + `pprev` + `nChainWork`).
- `connect_tip.c:158/642` — sole setter of `BLOCK_VALID_SCRIPTS`; `:156/180` sets `BLOCK_HAVE_DATA`.
- `block_index_db.c:297` — sole emitter of `EV_BLOCK_HEADER`, the feed for `block_index_projection`.

Delete the legacy engine and you remove the map producer, the nStatus setters, AND
the projection feed at once. Resolution: **production must precede consumption at
every height** — seed the map at boot from the projection (warm) or fast_sync
(cold); keep `accept_block_header → add_to_block_index` as the runtime producer
(relocate it out of the delete set); make the stages themselves set the nStatus
bits + emit `EV_BLOCK_HEADER`.

## Missing primitives (must be written, not just rewired)

1. `utxo_projection_setinfo(p,&txs,&txouts,&total)` — for `gettxoutsetinfo`. (`utxo_projection.c`)
2. `utxo_projection_seed_from_snapshot(p,staging_db)` — cold-start fast_sync seed (clone of `seed_from_legacy`).
3. `load_block_index_from_projection()` / `boot_rebuild_from_log()` — warm cold-start.
4. **Relocate** `add_to_block_index()` out of `process_block_core.c` (e.g. into `accept_block_header.c`) so the runtime producer survives the delete.
5. `script_validate_stage` nStatus setter + `EV_BLOCK_HEADER` re-emit — sets `BLOCK_VALID_SCRIPTS` (which `tip_finalize.preconditions_ok` HARD-gates on). **Omitted by the first design pass; without it the tip never advances post-delete.**

## Status (2026-05-31): the ADDITIVE phase (steps 1–5) is DONE + on `main`, green.

Steps 1–5 landed (commits `bfc2e302f`, `daecdaec4`) + a DRY follow-up (`8fcf45f19`,
the shared `block_index_emit_header_event`). The reducer now produces everything
the legacy engine produces — block_index entries (relocated producer), headers,
`HAVE_DATA`, `VALID_SCRIPTS` — running ALONGSIDE the still-present legacy engine.
Build + `make lint` clean + `test_parallel` 0/275 at each step. Remaining: steps
6–10 (the cutover + delete), gated on the cold/restart/read proof on a datadir copy.

## Ordered steps (build checkpoint after each; verify on a datadir COPY)

| # | File(s) | Does | Risk | Status |
|---|---|---|---|---|
| 1 | `utxo_projection.c` | add `setinfo` + `seed_from_snapshot` (pure additions) | none | ✅ done |
| 2 | `accept_block_header.c` | relocate `add_to_block_index` here (producer survives delete) | low | ✅ done |
| 3 | `header_admit_stage.c` | emit `EV_BLOCK_HEADER` when authoritative (2nd emitter alongside legacy) | low — idempotent | ✅ done |
| 4 | `body_persist_stage.c` | set `BLOCK_HAVE_DATA` + re-emit header | low | ✅ done |
| 5 | `script_validate_stage.c` | set `BLOCK_VALID_SCRIPTS` + re-emit header | low | ✅ done |
| 6 | `block_index_loader.c` + `boot.c` | `boot_rebuild_from_log()`: catch_up → fold projection → map + pprev + nChainWork → seed tip from cursor | medium | ✅ done |
| 7 | `snapshot_apply.c` + `utxo_projection.c` | cold-start: seed projection from snapshot + anchor header + cursor stamp | medium | ✅ done |
| 8 | `boot.c` + `utxo_projection.h` | point `coins_tip` read view at `coins_view_projection` | medium | ✅ done |
| 9 | `blockchain_controller_chain.c` | `gettxoutsetinfo`/commitment read off the projection | medium | ✅ done |
| 10 | (legacy connect path + importers) | **DELETE the legacy engine** | high | ⛔ BLOCKED |

## Step 10 finding (2026-05-31): the reducer must DRIVE ingestion before the delete

Step 10 stopped without editing — correctly. The legacy connect path
(`connect_tip` / `activate_best_chain` / `accept_block` / `process_block_core` /
`process_new_block`) is **still the live, unconditional block-ingest engine**, not
dead code shadowed by the reducer. It is called from ~20+ live sites: P2P delivery
(`msg_blocks.c:324`, `msg_compact.c`, `msg_headers.c`, `download.c`), mining
(`miner.c`, `mining_controller.c` submitblock), reorg/repair (`process_block_*`,
`chain_activation_controller`, `gap_fill_service`), and boot. The reducer stages
today RUN ALONGSIDE it (they emit `EV_BLOCK_HEADER` + set nStatus + feed the
projection) but they do **not** drive block connection — `connect_tip` still does.

So deleting the legacy path is NOT a mechanical removal. The remaining real work:
**route block ingestion through the Job reducer** — `process_new_block` / the P2P
+ mining + submitblock entry points must apply blocks via the reducer pipeline
(header_admit → … → tip_finalize) instead of `connect_tip`/`activate_best_chain`.
Only once the reducer is the actual ingest engine does the legacy connect path
become dead and deletable. This is the next focused effort (its own design pass).

## End-to-end proof (on a COPY, `~/.zclassic` renamed away)

- **A Cold:** wipe + fast_sync to anchor; `strace` shows zero `$HOME/.zclassic` access; `entry_count≥1`; `zcl_utxocommitment` == snapshot SHA3.
- **B Advance:** P2P past anchor; `zcl_status` height climbs (proves the cycle is cut + step 5 — frozen height = step 5 missing).
- **C Warm restart:** `kill -9` + reboot; height restored from log alone (±WAL rewind); no genesis re-validation storm (proves nStatus persisted).
- **D Read:** `gettxoutsetinfo` txouts == `utxo_projection_count`, matches pre-delete baseline; `zcl_utxo_projection_diff==0`; `getblock` body still served from `disk_block_io`.
- **Gate:** A–D green + `make lint` clean + `test_parallel` `N passed, 0 failed`.
