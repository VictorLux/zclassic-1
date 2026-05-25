# wt-c3-reflip — re-flip C-3 (validate_headers authoritative) behind the safe-flip guard

## Status

**READY — guard verified GO 2026-05-25.** Prereqs cleared: live node healthy/
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

> Deploy/flip on the live node is operator-gated by Rhett. See
> [`cutover-safety-protocol.md`](./cutover-safety-protocol.md) and
> [`../VISION.md`](../VISION.md) (dependency spine).
