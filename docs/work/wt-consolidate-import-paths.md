# wt-consolidate-import-paths — 3 legacy importers → 1 (independent DRY)

## Status

**IN PROGRESS (wt2).** Independent of the cutover and the P0 halt — touches only the
import/bootstrap modules, none of the live chain-advance path. Claim by marking
**IN PROGRESS** at the top.

## Why (the cruft)

Originally, three bootstrap pathways from a sibling zclassicd duplicated the
same LevelDB reader + SHA3 spot-check + cursor-stamping logic:

| File | LOC | Mode |
|---|---|---|
| `app/services/src/legacy_bootstrap_importer.c` (`COLD`) | current | full state-only import (`-cold-import`), zclassicd stopped |
| `app/services/src/legacy_bootstrap_importer.c` (`DIRECT`) | current | direct LevelDB+mmap (`-fastimport`), zclassicd stopped |
| `app/services/src/legacy_bootstrap_importer.c` (`ATTACH`) | current | ldb-snapshot, no-stop (`-legacy-attach`) |

Plus `tools/rebuild_recent.c` (the live-safe io_uring whole-chain rebuild) shares
the same `blocks_index_legacy_reader` / `chainstate_legacy_reader` / SHA3 helpers.
~2,400 LOC across these with heavy copy-paste.

Progress:
- `tools/rebuild_recent.c` now reuses the shared legacy snapshot and height-map
  helpers.
- `legacy_cold_import.c` and `legacy_direct_import.c` were removed; their public
  compatibility wrappers now dispatch through `legacy_bootstrap_importer` modes.
- `legacy_oneshot_import.c` was removed; its public compatibility wrapper now
  dispatches through `LEGACY_BOOTSTRAP_IMPORT_ATTACH`.
- The old `legacy_cold_import.h`, `legacy_direct_import.h`, and
  `legacy_oneshot_import.h` compatibility headers were removed; wrapper
  contracts now live in the canonical `legacy_bootstrap_importer.h`.
- The old wrapper functions (`legacy_cold_import_blocking`,
  `legacy_direct_import_range_blocking`, `legacy_oneshot_import_run`) and their
  adapter-only result structs were removed; boot now calls
  `legacy_bootstrap_import_blocking` directly with `COLD`, `DIRECT`, or `ATTACH`.
- The canonical importer header now exposes only the true cross-module contract:
  the mode-driven importer, the height-map loader used by `rebuild_recent`, and
  attach-stage test accessors. Snapshot, block-source, chainstate, cursor-anchor,
  and block-index-copy helpers are private implementation details again.

Verification:
- `make -j$(nproc) test_zcl`, `make -j$(nproc) zclassic23`, `make lint`, and
  `make -j1 test_parallel && ./test_parallel --jobs=$(nproc)` pass after the
  canonical-header collapse.
- Scratch `-cold-import=/home/rhett/.zclassic` imports successfully in ~47s and
  publishes the pending CSR anchor from legacy chainstate. Current observation:
  legacy `blocks/index` maps to h=3124589, while chainstate `B` anchors h=3124356;
  boot then advances the delta through normal activation. This is outside the
  import-path consolidation itself, but it is the remaining acceptance question
  behind "reaches zclassicd tip".
- Follow-up fix: the shared snapshot importer now records the pending CSR anchor
  height from the chainstate-best branch, not the copied block-index tip. When
  the two differ, the imported UTXO set, anchor hash, and anchor height now name
  the same chain point; higher block-index entries remain available for normal
  activation above the anchor.

## The shape (one canonical importer, pluggable mode)

Collapse the three services into ONE `legacy_bootstrap_importer.{c,h}` with a
mode enum (`COLD` | `DIRECT` | `ATTACH`) selecting the policy differences
(stop-required vs ldb-snapshot; hardlink vs mmap; cursor-stamp or not). Shared
single-copy helpers for: legacy LevelDB height-map load, SHA3 K-window
spot-check, chainstate UTXO bulk-import, cursor stamping. The three CLI flags
keep working — they just route into the one importer with a different mode.
`rebuild_recent` should reuse the same shared readers (not its own copies).

## Acceptance
- Net LOC down (target: ~2,400 → ~700).
- All three flags (`-cold-import`, `-fastimport`, `-legacy-attach`) behave
  identically to before (verify a cold-import to a scratch datadir reaches the
  zclassicd tip).
- `./test_parallel --jobs=$(nproc)` green. One commit per logical step. Push to main.

## Do NOT touch
connect_tip / activate_best_chain / the live chain-advance path, or the
condition layer (orchestrator owns the condition DRY in flight).
