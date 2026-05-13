# Next-session handoff — durable fast-sync (P6 body-pull)

**Last session ended 2026-05-13 ~20:55.**
Plan file: `~/.claude/plans/look-i-need-you-delegated-forest.md`.

## State at handoff

### Running node
- Tip: **3,100,205** (stuck, see below).
- zclassicd tip: **3,111,099**. Gap: 10,894 blocks.
- Loopback peer 127.0.0.1:8034: P2P handshake works after the P1 cap-fix
  (zero `Misbehaving +20` events since the new binary deployed). Sometimes
  takes ~5 min to come up after restart due to the connman 10 s PEER_CONNECTING
  reaper racing the message loop — see "Known: loopback handshake race" below.
- K2 (loopback block-body fast lane) is **shipped** in the running binary —
  `dl_peer_stats.is_loopback`, `DL_MAX_IN_FLIGHT_PER_LOOPBACK=512`,
  bumped `max_assign=256` for loopback.

### Why the tip is stuck
```
find_most_work_chain: STUCK at tip h=3100205 (best_header h=3100557, gap=352)
  skipped[failed=0 invalid=7 no_data=217]
```
- **7 blocks** in the 352-block forward window carry `BLOCK_FAILED_VALID`
  from an earlier validation rejection. The current chain selector won't
  use any path that passes through them.
- **217 blocks** have no body data — these are the prefix gap that K2 was
  supposed to close. K2 *is* closing the gap from the loopback peer
  (`blocks_received` keeps climbing on peer id 47/127.0.0.1) but the
  bodies arriving are all *past* the 7-invalid roadblock, so they pile
  up as orphans.

zclassicd accepts every block in this range, so the 7 marked-invalid blocks
are almost certainly **spurious-fail** (transient validator bug, malformed
peer delivery, or stale flag from before the K1/P1 fix). They are NOT
consensus-level invalid.

### Commits ahead of last handoff — all pushed to origin/main
```
51517c07d net: K2 — loopback fast lane for block bodies
09d1aae80 lint: annotate pre-existing stderr diagnostics in msg_headers.c
834ae419f net: clamp outbound getheaders response to 160 for legacy peers
de3c395eb lint: annotate pre-existing sentinels and stderr diagnostics
+ the 5 commits from the previous handoff (already pushed)
```

## Immediate priorities (next session)

### P0 — durable body-pull (the big one, ~3-4 hr)

Goal: make `-importfromlegacy=$HOME/.zclassic` a one-command unsticker for
any future "tip behind zclassicd" condition, regardless of cause.

Today's `phase3_block_ingest` at `app/services/src/local_chain_ingest.c:1435`
pulls **headers** past our tip via `header_probe_pull_range_blocking` but
*stops* at line 1523 with a `break` when `active_chain_at(h)` returns NULL
(meaning the height is past our active tip — body never arrived). The
durable fix:

1. **New file `app/services/src/legacy_body_pull.c`**, mirroring the structure
   of `app/services/src/header_probe_service.c`. Single public entry:
   ```c
   bool legacy_body_pull_range_blocking(struct main_state *ms,
                                        struct coins_view_cache *coins_tip,
                                        const struct chain_params *params,
                                        const char *our_datadir,
                                        int from_height,
                                        int to_height,
                                        int *out_applied);
   ```
   For each height in [from..to]:
   - Walk pprev from `pindex_best_header` collecting block_index pointers
     down to height = from (reuse the `collect_pprev_window` pattern from
     `app/services/src/gap_fill_service.c:47`).
   - For each entry in ascending order:
     - If `bi->nStatus & BLOCK_HAVE_DATA`, skip (we already have it).
     - Else: RPC `getblock <hash> 0` against zclassicd → hex string.
     - Decode hex to `struct block` (use `core_io.c parse_hex` + `block_deserialize`).
     - `write_block_to_disk(&block, &pos, our_datadir, params->pchMessageStart)`
       (signature at `lib/storage/include/storage/disk_block_io.h:38`).
     - Set `bi->nStatus |= BLOCK_HAVE_DATA`; persist via `block_tree_db_write_block_index`.
     - `chain_advance(&vs, ms, coins_tip, bi, NULL, params, our_datadir, "legacy_body_pull")`
       → tip advances by one.
   - Reuse the RPC helpers (`hp_http_rpc_call_dyn`, `hp_parse_zclassic_conf`,
     `hp_base64_encode`) — lift them into `lib/rpc/src/legacy_rpc_client.c`
     so both `header_probe` and `body_pull` share them.

2. **Wire into `phase3_block_ingest`** at `app/services/src/local_chain_ingest.c:1523`:
   replace the `break` on missing block_index with a call to
   `legacy_body_pull_range_blocking(ms, coins_tip, params, our_datadir,
                                    h, anchor_h_plus_header_tip, &applied)`.
   This makes phase3 robust to partial-state datadirs.

3. **Test**: stop the service, run
   `./zclassic23 -datadir=$HOME/.zclassic-c23 -importfromlegacy=$HOME/.zclassic`,
   wait for ingest to complete, observe tip reaches 3,111,099+, restart
   service normally. Expected ~3-5 min for 11 K-block gap on loopback RPC.

4. **Operator runbook**: add a top-level `tools/zcl-resync-from-legacy.sh`
   wrapper that does stop → import → start. One command for the operator.

### P0.5 — flag-clearing fallback (~30 min)

For the 7 currently-marked-invalid blocks: add a boot-time policy that
clears `BLOCK_FAILED_VALID` from any block whose `nHeight > active_tip`
when the chain is detected as stuck (tip < pindex_best_header by ≥ N).
Source: `app/services/src/chain_restore_service.c:648` already has an
`invalidated_off_chain` counter pattern — extend it.

Combined with P0, this auto-recovers from any "spuriously-marked-invalid"
stall without operator intervention.

### P1 — fix the loopback handshake startup race (~1 hr)

Symptom: on cold boot, the loopback peer sometimes takes ~5 min to reach
PEER_HANDSHAKE_COMPLETE because the connman 10 s PEER_CONNECTING reaper
at `lib/net/src/connman.c:817` fires before `msg_send_messages` pushes
our version. Other times it completes in seconds.

Fix: in connman.c:817 add `net_addr_is_local(&n->addr.svc.addr)` exemption:
```c
int connect_timeout = net_addr_is_local(&n->addr.svc.addr) ? 90 : 10;
if (!n->inbound && n->state == PEER_CONNECTING &&
    n->time_connected > 0 && now_check - n->time_connected > connect_timeout) {
    ...
}
```
Loopback TCP either succeeds instantly or never; the 10 s cutoff was
designed for remote dead SYN sockets and doesn't apply.

### P2 — onward (already in the plan file)
- K2 measurement: with P0+P0.5 unsticking the chain, benchmark loopback
  block throughput. Expected ≥ 50 blocks/sec sustained, possibly bound
  by `connect_tip` validation pipeline (then P3 batched ECDSA + Groth16).
- P3 batched verify if needed.

## Files of interest (P0 implementation)

- `app/services/src/header_probe_service.c` — RPC client to copy from
- `app/services/src/local_chain_ingest.c:1435` `phase3_block_ingest` — insertion point
- `app/services/src/gap_fill_service.c:47` `collect_pprev_window` — pprev walk pattern
- `lib/storage/include/storage/disk_block_io.h:38` `write_block_to_disk`
- `lib/storage/src/block_index_db.c` — persist updated nStatus after setting BLOCK_HAVE_DATA
- `app/services/src/chain_advance.c` — final apply per block

## What was shipped this session
| Commit | Stage |
|---|---|
| `de3c395eb` | P8 lint cleanup (raw-return-ok + obs-ok sentinels) |
| `834ae419f` | **P1: legacy-peer getheaders cap 160** (loopback handshake fix) |
| `09d1aae80` | P1 lint follow-on (msg_headers.c obs-ok) |
| `51517c07d` | **K2: loopback fast lane for block bodies** |

All pushed to `origin/main`.

## Diagnostic findings (won't repeat next session)

- zclassicd's `~/.zclassic/debug.log` is the authoritative source for "why is
  the loopback peer rejecting us." Look for `Misbehaving: 127.0.0.1` events.
- `~/.zclassic-c23/node.log`'s `find_most_work_chain: STUCK at tip ...
  skipped[failed=N invalid=N no_data=N]` summary line is the chain-selection
  diagnosis. `invalid > 0` always means "spurious or real BLOCK_FAILED_VALID
  blocks need attention."
- The `gap-fill queued 1 blocks (window [..])` pattern repeating with no tip
  movement always means: "we asked, peers didn't deliver" — and 90% of the
  time the cause is downstream of the queue (invalid flag, missing parent,
  or peer not actually serving). Investigate via the STUCK line first.

## Deferred (still in plan file, lower priority)

- A2 — 4-way SHA3-256 AVX-512 absorber (~6 hr crypto work)
- D1/D2 — io_uring phase 1 (needs `sudo apt install liburing-dev`)
- T2.3 — tip-zone shadow validate
- N6 — per-block SHA3 tip-zone
- N7 — `-trust-mode=pure|hybrid|full` CLI flag
