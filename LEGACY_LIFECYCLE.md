# Legacy Lifecycle

Modules and CLI flags prefixed `legacy_` interact with an existing
`zclassicd` (C++ ZClassic) install on the same host. They exist for two
distinct reasons that should not be confused:

1. **Bootstrap** — cold-start a fresh `zclassic23` faster than full IBD
   by reading blocks / index / chainstate from a sibling `zclassicd`.
2. **Drift detection** — at runtime, periodically compare our chain
   state against `zclassicd` so divergence is caught early.

This file is the source of truth for which paths are **active**,
**opt-in**, **deprecated**, or **scheduled for removal**. Without it,
the word "legacy" tends to read as "cruft" — most of this code is in
fact load-bearing.

Cross-references: `CLAUDE.md` (top-level architecture),
`DEFENSIVE_CODING.md` (the rules legacy_* modules must follow).

---

## CLI flag map

| Flag | Module | Status | What it does |
|------|--------|--------|--------------|
| `-cold-import[=DIR]` | `legacy_cold_import.c` | **Active** (recommended cold start) | Hardlinks `blk*.dat`, bulk-copies block_index LevelDB, bulk-imports chainstate at the legacy tip. Empty datadir → tip in ~60s. Skips `process_new_block` entirely. |
| `-fastimport[=DIR]` | `local_chain_ingest_fastimport.c` (calls into `legacy_*` readers) | **Active** | Reads blocks LevelDB + mmaps `blk*.dat`, runs the normal block-ingest path with deferred per-block I/O. Slower than `-cold-import` but exercises the full validation pipeline; auto-triggers a wallet rescan at end. |
| `-importfromlegacy=DIR` | dispatcher in `legacy_import.c` | **Active** | The documented one-liner that picks between `-cold-import` and `-fastimport` based on detected state. Mirrors the user-facing CLI from the 2026-05-13 fast-sync plan. |
| `-nolegacyimport` | (no module — disables) | **Active** | Disable any auto-detection of `~/.zclassic` on boot. Use when you explicitly do not want legacy interaction. Default is to auto-detect. |
| `-legacy-auto-import` | — | **Active** (default-on) | Implicit. When `~/.zclassic` exists and `-nolegacyimport` is not set, the boot path promotes legacy state via `boot_step_legacy_anchor`. |

There is no `-bodypull-from-legacy` flag in the current tree. It was
removed in Wave 9i after the [body-pull pathology](MEMORY.md) was
diagnosed: `legacy_body_pull` pre-populated `block_index` with
`BLOCK_HAVE_DATA` but never activated those blocks, leaving
`find_most_work_chain` stuck. The module's `legacy_body_pull.c` /
`.h` files remain in tree because they still provide the SHA3
spot-check primitives that other paths call.

---

## Module map

### Bootstrap path (`app/services/src/`, `app/controllers/src/`)

| File | Status | Role |
|------|--------|------|
| `legacy_cold_import.c` + `.h` | **Active** | Direct copy/hardlink of legacy state. Triggered by `-cold-import`. Skips `process_new_block`. |
| `legacy_direct_import.c` + `.h` | **Active** | Used by `-fastimport` for the per-block ingest path that mmaps `blk*.dat` and reads block_index from LevelDB directly. |
| `legacy_body_pull.c` + `.h` | **Disabled at boot** (Wave 9i); helpers retained | The boot-time call has been removed (pathology — see memory). SHA3 spotcheck helpers used by other paths remain callable. **Slated for narrower API.** |
| `legacy_mirror_sync_service.c` + `.h` | **Active** | Background drift-detector. Periodically calls `getmirrorstatus` and surfaces lag / divergence via `EV_MIRROR_*` events. Powers `zcl_mirror_status` and `zcl_diff_with_legacy`. |
| `legacy_import.c` (controller) | **Active** | RPC + boot-time dispatcher for `-importfromlegacy`. |

### RPC clients (`lib/rpc/src/`)

| File | Status | Role |
|------|--------|------|
| `legacy_rpc_client.c` + `.h` | **Active** | HTTP/JSON-RPC client for talking to `zclassicd:8232`. Used by mirror sync, `zcl_probe_zclassicd`, and `legacy_chain_oracle`. |
| `legacy_chain_oracle.c` + `.h` | **Active** | Treats `zclassicd` as an external chain oracle (hash at height, getblockcount, etc.). Used by quorum / drift checks. |

### Storage readers (`lib/storage/src/`)

| File | Status | Role |
|------|--------|------|
| `blocks_index_legacy_reader.c` | **Active** | Reads `zclassicd`'s block_index LevelDB into our schema. Used by `-cold-import` and `-fastimport`. |
| `chainstate_legacy_reader.c` | **Active** | Reads `zclassicd`'s chainstate LevelDB (compressed UTXOs) into our `coins_db`. Used by `-cold-import`. |

---

## Removal candidates

Nothing in the table above is scheduled for removal in the near term —
the bootstrap path is still the fastest way to spin up a fresh
`zclassic23` against a working `zclassicd`, and drift detection has
caught real bugs (see the Wave 9 memory entries on CSR rollback +
chain_evidence_controller).

The narrowest cleanup target is the `legacy_body_pull` API: only the
SHA3 spotcheck helpers are still load-bearing; the rest can shrink to
match. A future sub-wave can fold those helpers into
`local_chain_ingest_fastimport.c` and remove `legacy_body_pull.{c,h}`
entirely.

---

## Adding a new legacy_ module

Don't, unless:

1. The behaviour is **strictly tied to interoperability with an
   external `zclassicd`** (bootstrap, drift detection, oracle). General
   "legacy because written earlier" modules belong in their own
   directory or get renamed.
2. The CLI flag goes through `app_context` and respects
   `-nolegacyimport`.
3. The new module has an entry in the tables above before merge.
