# wt-c3-reflip — re-flip C-3 (validate_headers authoritative) behind the safe-flip guard

## Status

**IN PROGRESS (wt3) — guard verified GO 2026-05-25.** Prereqs cleared: live node healthy/
advancing; safe-flip guard sound (detects in 180s, reverts live, pages a human).
This is the master lever — the first authoritative flip of the ONE path. Claim by
marking IN PROGRESS at the top.

## The critical lesson (why C-3 halted last time)

The first C-3 (`ad34efb65`) set `HEADER_ADMIT_MODE_AUTHORITATIVE` /
`VALIDATE_HEADERS_MODE_AUTHORITATIVE` **at boot**. Legacy `accept_block_header()`
runs first on P2P/RPC ingress, and in AUTHORITATIVE mode it refuses to create new
block_index entries without pre-existing stage records → post-import headers
couldn't advance → silent stall. Reverted by `6e0f6a82c` (defaults back to
SHADOW). **So: flip at RUNTIME on a healthy at-tip node, never via boot defaults.**

## The guard (your safety net — verified)

`app/conditions/src/cutover_no_forward_progress.c`: when a stage is authoritative
AND `sync_monitor_tip_advance_age() > 180s` AND peers are ahead → CRITICAL. Remedy
`header_admit_set_mode(SHADOW)` + `validate_headers_set_mode(SHADOW)` via
`atomic_store` (live, no restart), then `EV_OPERATOR_NEEDED` → alert sinks +
`zcl_status` health=DEGRADED + sd_notify. Worst case: 3-minute stall, auto-revert,
you get paged. Bounded blast radius.

## Procedure

1. **Pre-flip gate (must all hold):**
   - `zcl_status`: node healthy, at tip (gap 0), `tip_advance_age` low.
   - Shadow parity: `zcl_diff_staged_header_admit` and the validate_headers shadow
     counters show **0 divergence** vs legacy across recent heights (the
     authoritative path must already match legacy in shadow before you trust it).
2. **Flip at runtime** (not boot): set header_admit + validate_headers to
   AUTHORITATIVE via the runtime mode setter (expose an MCP/RPC if not present —
   do NOT change the boot defaults).
3. **Watch (soak):** tip advances past the flip height for a sustained run;
   `zcl_status` never goes DEGRADED; the guard never fires; shadow diff stays 0.
4. **If the guard fires:** it already reverted to SHADOW + paged. Read the shadow
   diff to find the divergence height; fix; do not re-flip until parity is 0 again.

## Acceptance

**Sustained LIVE forward progress past the cutover height** (not a unit test) +
shadow diff 0 + guard never fired during the soak. Then: delete the legacy
validate_headers fallback (`wt-phase2-cutover-c3-final-delete.md`), and proceed
C-5→C-9 in sequence (each: runtime flip + same guarded soak), each deleting its
legacy stage. C-8 unlocks the `utxo_recovery` dissolve; C-9 unlocks the
`chain_advance_coordinator` + `legacy_mirror_sync` dissolve — the ~12.5K LOC purge.

## LIVE BLOCKER found 2026-05-25 — C-3 needs GENUINE peer connectivity (do NOT weaken the gate)

C-3 was **attempted** on the live node. Result: preflight `ready:false`, blocker
`live_health_not_ready` → `operator_needed: condition=peer_floor_violated`.
Verified (engine, latch, detect all correct — NOT a bug): the node genuinely
reaches only **2 outbound peers** (`51.178.179.75` + local `zclassicd`); the 8
configured peers are down, and the live inbound nodes don't accept outbound on
:8033. `PEER_FLOOR_MIN_HEALTHY=3`, so `peer_floor_violated` correctly stays active.
Parity itself is clean (`header_admit_diff CONVERGED 10000/10000`, `validate_headers`
0-fail).

**The gate is RIGHT — do NOT relax it, do NOT count the local node toward the
floor.** A consensus-authority flip needs ≥3 *diverse* healthy outbound peers for
real anti-eclipse safety; lowering that bar (or counting your own node) defeats
the purpose and is not the best/correct design. The correct fix is to make the
node **genuinely well-connected**: working DNS seeds / addr discovery / reachable
peers so it sustains ≥3 diverse outbound. Until the node *actually* meets the
floor, C-3 correctly does not fire. **Robust connectivity is the work — not a
lowered bar.** (See memory: always the best, never weaken a bar.)

## REAL BLOCKER (CORRECTED 2026-05-25) — missing block BODIES, not a consensus divergence

I earlier wrote this was "408 main-chain consensus divergences / the new path is
wrong." **That was wrong — I concluded before reading the `fail_reason`.** Read
`validate_headers_log` directly (progress.kv, python3 sqlite3 read-only):

- `ok=1`: **3,124,225** rows. `ok=0`: **419** rows, ALL `fail_reason='disk-read-failed'`.
- Failures are a CONTIGUOUS range **3,124,225 → tip** — exactly the post-cold-import
  region. Heights 0→3,124,224 (cold-imported, `blk*.dat` hardlinked) ALL pass.

So the validation **logic is fine** (3.1M headers verified clean). The failures are
a **data-availability gap from the cold-import** used to cure the halt: the node
holds the HEADERS for the post-import range but not readable block BODIES, so
`check_equihash_solution` can't read the block → `disk-read-failed` (same root as
the earlier `read_block_undo: cannot open rev*.dat`).

**Pinned to the storage layer (2026-05-25):** the bytes are NOT missing —
`blk00050.dat` exists (380639 B) and the index offsets are valid (h=3124225
nFile=50 nDataPos=8 BLOCK_HAVE_DATA; h=3124640 nDataPos=370196). But
`read_block_from_disk_index_pread` (validate_headers_stage.c) returns
`disk-read-failed` for the whole file-50 range. So it's a **read/index
inconsistency on the post-cold-import file**, not missing data and not consensus
logic. blk00049 and below are cold-import hardlinks; blk00050 is the node's own
post-anchor file. The cold-import marked the post-anchor range BLOCK_HAVE_DATA in
a state `read_block_from_disk_index_pread` can't read.

**Fix (storage layer — workers' area):** make validate_headers' block read see/
read `blk00050.dat` correctly (use the open-on-demand `bmr` reader, or repair the
nDataPos/file state the cold-import wrote), → `failed_total→0` → cutover gate
clears. **Honest root note:** the cold-import used to cure the halt was a band-aid
that left this gap; the clean fix is either a body-repair for the post-anchor
range or a rebuild that produces readable block data throughout. NOT a consensus bug.

> Deploy/flip on the live node is operator-gated by Rhett. See
> [`cutover-safety-protocol.md`](./cutover-safety-protocol.md) and
> [`../VISION.md`](../VISION.md) (dependency spine).
