# wt-consolidate-import-paths — 3 legacy importers → 1 (independent DRY)

## Status

**UNCLAIMED.** Independent of the cutover and the P0 halt — touches only the
import/bootstrap modules, none of the live chain-advance path. Claim by marking
**IN PROGRESS** at the top.

## Why (the cruft)

Three bootstrap pathways from a sibling zclassicd duplicate the same LevelDB
reader + SHA3 spot-check + cursor-stamping logic:

| File | LOC | Mode |
|---|---|---|
| `app/services/src/legacy_cold_import.c` | ~? | full state-only import (`-cold-import`), zclassicd stopped |
| `app/services/src/legacy_direct_import.c` | ~? | direct LevelDB+mmap (`-fastimport`), zclassicd stopped |
| `app/services/src/legacy_oneshot_import.c` | ~? | ldb-snapshot, no-stop (`-legacy-attach`) |

Plus `tools/rebuild_recent.c` (the live-safe io_uring whole-chain rebuild) shares
the same `blocks_index_legacy_reader` / `chainstate_legacy_reader` / SHA3 helpers.
~2,400 LOC across these with heavy copy-paste.

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
