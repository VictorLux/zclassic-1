# Warm-sync + connect hot-path bottleneck profile (2026-05-29)

Evidence-backed profile of the **warm-start / restart-at-tip** path and the
**per-block connect/validation hot path**, with a ranked SAFE-vs-RISKY
optimization plan. Measurement only — no production behavior changed.

Reference bars (BENCHMARKS_LOG.md is canonical): warm-start 37.7s → 10s target,
cold-sync 180s → 30s target. The 37.7s row was wall-clock `systemctl stop→start`
to first `getblockcount`; this profile uses the binary's own `[boot] <phase> Nms`
markers, which read ~46s total (different start datum — see caveats).

---

## (a) Methodology + exact commands

All timing numbers are **harvested from the live node's own instrumentation**,
which it already emits on every restart and every live-tip connect. The live
node restarts periodically (chain_tip_watchdog), so `~/.zclassic-c23/node.log`
contains many independent samples. **The live datadir and zclassicd were never
written to or stopped** — every command below is a read-only grep of the log, or
a run against a throwaway `/tmp` datadir.

Build (worktree, to confirm the binary builds and to exercise `-bench`):

```
cp -n /home/rhett/github/zclassic23/vendor/lib/*.a vendor/lib/   # gitignored static libs
make -j$(nproc) zclassic23                                        # clean build, 12.3 MB binary
```

Two instrumentation sources already in the binary:

1. **Warm-start phases** — `config/src/boot.c` emits
   `printf("[boot] %-30s %lldms\n", phase, boot_clock_ms()-t_phase)` for
   `wallet_load`, `sqlite_open_migrate`, `block_index_load`, `utxo_import`,
   `sapling_tree_load`, `p2p_services_start`, `total`. Plus two
   `[boot-phase] END <name> Nms` markers (`chain_restore_finalize`,
   `wallet_scan_blocks`) via `boot_phase_begin/end`.

2. **Per-block connect stages** — `lib/validation/src/process_block.c:69`
   `process_block_log_live_stage()` emits
   `connect_tip: h=<h> stage=<s> elapsed_ms=<ms>` for every block with
   `height > 1,000,000` (`process_block_live_height`). Stages: `read_block`,
   `connect_block`, `coins_flush`, `update_tip`.

Harvest commands (read-only against the live log):

```
LOG=~/.zclassic-c23/node.log
# warm-start phase aggregate (n=7 restarts in the current log)
for p in wallet_load sqlite_open_migrate block_index_load utxo_import \
         sapling_tree_load p2p_services_start total; do
  grep "\[boot\] $p " $LOG | grep -oE "[0-9]+ms" | grep -oE "[0-9]+" \
    | sort -n | awk -v p=$p '{a[NR]=$1;s+=$1} END{print p, "n="NR, "median="a[int(NR/2)+1], "max="a[NR], "mean="int(s/NR)}'
done
# boot-phase END markers
grep -oE "\[boot-phase\] END [a-z_]+ [0-9]+ms" $LOG
# per-block connect stages
grep -oE "connect_tip: h=[0-9]+ stage=[a-z_]+ elapsed_ms=[0-9]+" $LOG
```

Throwaway-datadir run (confirms phase markers + fixed-cost phases; the source
LevelDB read is from zclassicd's `~/.zclassic`, read-only):

```
timeout 60 ./zclassic23 -datadir=/tmp/zcl-bench-fresh -rpcport=28232 \
  -port=28033 -nobgvalidation -connect=0
```

`./zclassic23 -bench` was also run — see the instrumentation-gap section; it is
a no-op for timing.

---

## (b) Measured phase breakdown

### Warm-start (restart-at-tip), n=7 live restarts, chain h≈3.125M

The binary's own `[boot]` + `[boot-phase]` markers, aggregated. Per-phase values
are each measured from that phase's own start (`boot_clock_ms()`), so they are
disjoint segments. `total` is measured from `t_boot_start` (before the
prologue) to the end of `app_init_services`.

| phase | median | max | what it does |
|---|---:|---:|---|
| (prologue, uninstrumented) | — | — | observability, chain/datadir select, postmortem init, unclean-shutdown detect, disk/IBD guards, crypto+state init (`boot.c` 1197–1223) |
| wallet_load | 28 ms | 29 ms | wallet_init + wallet_sqlite open/canary |
| sqlite_open_migrate | 1,376 ms | 1,412 ms | node.db open + schema migrate + progress.kv |
| block_index_load | 2,375 ms | 2,868 ms | LevelDB block index → in-RAM map (3.128M entries, 296 B/entry, 1267 MB) |
| utxo_import | 0 ms | 0 ms | **warm: no-op** (UTXOs already in node.db; `consensus_snapshot.db` present) |
| sapling_tree_load | 19 ms | 19 ms | load saved Sapling commitment tree |
| chain_restore_finalize | 5,298 ms | 5,416 ms | disk-ancestry rebuild of active chain + post-restore integrity check (full nbits/hole scan over 3.125M heights) |
| wallet_scan_blocks | 4,001 ms | 4,710 ms | wallet block scan / catch-up |
| p2p_services_start | 10,809 ms | 16,647 ms | mempool, P2P listen, RPC server, **Phase-4 projection backfill** (mempool/peers/utxo/block_index/znam/wallet/contacts/onion/hodl, replaying event log to offset≈25.38M), Tor onion start |
| **sum of named phases** | **~23,950 ms** | | |
| **`[boot] total`** | **~43,600–47,300 ms** | 52,188 ms | |
| **UNATTRIBUTED GAP** | **~20,000 ms (~45%)** | | time between the named phase markers — see below |

**The single largest documented finding: ~45% of warm-start wall-time (≈20 s)
is not attributed to any named phase.** The named phases sum to ~24 s; `total`
is ~44 s. The gap lives in the uninstrumented code between phase markers:
the prologue before `wallet_load`, and the large stretch in `boot.c` 2522–3289
between `wallet_scan_blocks` and `p2p_services_start` (a second
`activate_best_chain`, additional restore/anchor logic, tip-vs-UTXO safety
reconciliation). This stretch is **not measured** today.

### Per-block connect hot path (live-tip), height > 1M

Aggregated `connect_tip: stage=… elapsed_ms=…` over the current log. `update_tip`
has fewer samples (it only logs on an actual tip advance, not on every
connect_block call in a multi-block activate loop).

| stage | n | mean | max | what it wraps |
|---|---:|---:|---:|---|
| read_block | 7,452 | 0 ms | 1 ms | read block bytes from disk |
| connect_block | 7,727 | 23 ms | 1,034 ms | **all crypto + UTXO apply**: script/sig verify, JoinSplit Ed25519, Sapling Groth16, merkle, `update_coins` |
| coins_flush | 7,727 | 0 ms | 1 ms | flush coins cache → SQLite (UTXO projection) |
| **update_tip** | **1,183** | **584 ms** | **3,768 ms** | **tip commit via chain_evidence_controller → CSR → LevelDB block-index write + fsync** |

`update_tip` distribution: 50–200 ms ×218, 200–500 ms ×430, 500–1000 ms ×376,
1000+ ms ×159. The 1000+ ms tail is wedge/recovery contention (the log spans the
2026-05-29 tip-fork wedge). **Steady-state floor — the 20 most recent samples —
is 180–370 ms per tip commit.** That is the live-tip throughput ceiling:
~3–5 blocks/s regardless of how cheap validation is.

---

## (c) Top bottlenecks, ranked by measured cost

1. **`update_tip` (tip commit / LevelDB block-index sync), ~200 ms floor, mean
   584 ms per block.** Dominates the per-block connect path by ~25× over
   connect_block. Caps live-tip throughput at ~3–5 blk/s. This **confirms the
   prior finding** (bottleneck is the commit/LevelDB-sync path, NOT crypto) and
   refutes any "crypto-bound" hypothesis: `connect_block` (which contains all
   signature/proof verification) is only 23 ms.

2. **Warm-start unattributed gap, ~20 s (~45% of the 44 s boot).** Largest single
   chunk of restart-to-operational time, and it is invisible to the operator.
   Cannot be optimized until it is measured (see section e).

3. **p2p_services_start, ~11 s (median), up to 16.6 s.** Bundles P2P/RPC/Tor with
   the Phase-4 projection backfill (event-log replay to offset ≈25.4M). The
   backfill replays a large event log on every boot; this is a strong candidate
   for the bulk of the 11 s but is not separately timed.

4. **chain_restore_finalize, ~5.3 s.** Every warm boot does a full disk-ancestry
   rebuild of the active chain + a post-restore integrity scan over all 3.125M
   heights (nbits + hole check). On a clean restart-at-tip this is largely
   redundant re-verification of state that was consistent at shutdown.

5. **wallet_scan_blocks ~4.0 s + sqlite_open_migrate ~1.4 s + block_index_load
   ~2.4 s.** Mid-tier fixed costs. block_index_load builds a 1267 MB in-RAM map
   from LevelDB on every boot.

---

## (d) Optimization proposals — SAFE vs RISKY

### SAFE (no consensus-correctness risk; does not touch what gets committed)

- **S1 — Skip `chain_restore_finalize` full integrity scan on a clean shutdown
  (~5.3 s).** A `.shutdown_clean` marker is already written
  (`write_clean_shutdown_marker`). On clean shutdown the active chain was
  consistent; the full disk-ancestry rebuild + 3.125M-height hole/nbits scan is
  redundant. Gate the heavy rebuild behind "unclean shutdown OR integrity
  hint". SAFE: it is a re-derivation of already-persisted state, not a consensus
  decision; keep the full scan on the unclean path. Expected win: ~5 s.

- **S2 — Parallelize / defer Phase-4 projection backfill out of the boot
  critical path (portion of ~11 s).** Projections (explorer/wallet/hodl/etc.)
  are pure derived read-models, repairable from the event log. Start P2P/RPC
  first, run projection catch-up on a background Job that reports readiness.
  SAFE: projections are explicitly non-authoritative (the `check-projections-pure`
  lint gate already enforces this). Expected win: several seconds off
  time-to-operational; needs S3's instrumentation to quantify.

- **S3 — Instrument the ~20 s gap and the inside of p2p_services_start /
  connect_block (instrumentation only, zero behavior change).** See section e.
  SAFE by construction. This is the prerequisite for #2 and #3.

- **S4 — Lazy / mmap block-index load (~2.4 s).** The 1267 MB map is rebuilt
  fully on every boot. A memory-mapped or lazily-populated index would cut boot
  RAM and time. SAFE if the loaded contents are byte-identical; behind a flag,
  diff against the eager load. Expected win: ~2 s + RSS reduction.

### RISKY (touches connect_tip / the live commit authority — needs proofs)

- **R1 — Batch / async the `update_tip` LevelDB block-index write+fsync (biggest
  single win, ~200–580 ms/block → target <50 ms).** Today each tip advance
  appears to do a synchronous LevelDB write + fsync via the chain_evidence
  controller / CSR. Batching N block-index writes into one fsync (or using a
  group-commit / WAL-style append) would lift live-tip throughput from ~5 blk/s
  toward the connect_block-bound ~40 blk/s. **RISKY:** this is the crash-safe
  tip-advance ordering invariant — `connect_tip.c` documents that coins.db must
  commit before the LevelDB block_index fsync (see the
  `coins_hash_pre_commit` rollback logic and the at-tip kill-9 ordering
  invariant). Any batching must preserve: (i) coins-before-index ordering, and
  (ii) the property that after a kill-9 the tip never points past committed
  coins. Requires: a replay/kill-9 corpus proof + the conservation invariants
  before flipping. Do NOT change without explicit go.

- **R2 — Confirm `update_tip` cost is fsync (not CPU) before R1.** R1 assumes
  fsync-bound; needs the trace split in section e to prove it. If a chunk is the
  per-block integrity-evidence build (`process_block_verified_tip_evidence`)
  rather than fsync, the fix is different. Classify as RISKY because the
  measurement target is inside the authority path.

---

## (e) Optimizations that need new instrumentation first

The biggest levers are blocked on missing counters. Adding these is pure
instrumentation (SAFE) and should land before any tuning:

1. **Split `update_tip` into sub-stages.** `process_block_commit_tip` →
   `chain_evidence_controller_promote_tip` → CSR → LevelDB. Add
   `GetTimeMicros()` brackets around (a) `process_block_verified_tip_evidence`,
   (b) the CSR commit, (c) the LevelDB write, (d) the fsync. Without this we
   cannot prove R1's fsync hypothesis vs an evidence-build cost. **(Confidence
   that update_tip is the bottleneck: HIGH — measured. Confidence on the cause
   being fsync specifically: MEDIUM — inferred from the 200 ms floor and the
   commit path, not yet split.)**

2. **Instrument the warm-start gap.** Wrap the prologue (boot.c 1197–1223) and
   the post-`wallet_scan_blocks` stretch (boot.c 2522–3289) in `boot_phase`
   markers, or assert `sum(phases) == total` and emit the residual as a named
   `boot_unattributed` phase. ~45% of boot is currently dark.

3. **Split `p2p_services_start`.** Separate markers for projection-backfill vs
   P2P/RPC listen vs Tor start, so S2's win is quantifiable.

4. **Sub-time `connect_block`.** It lumps script verify + JoinSplit + Sapling
   Groth16 + UTXO apply into one 23 ms number. Per-component timing would
   confirm crypto is not the ceiling (currently inferred from the 23 ms total
   vs the 200 ms+ update_tip, which is already decisive at the block level).

---

## Confidence: measured vs estimated

- **MEASURED (high confidence):** every warm-start phase number (n=7 restarts,
  binary's own `boot_clock_ms` markers); every per-block connect stage number
  (n=7,452–7,727 samples; update_tip n=1,183); the ~20 s warm-start gap
  (= total − sum of named, arithmetic on measured values); the fact that the
  `-bench`/`zcl_benchmark` harness emits only `pending` placeholders + an
  optional read-only `/proc` RSS sample and does **not** time cold/warm start.
- **MEASURED but caveated:** `update_tip` mean (584 ms) is inflated by the
  current log spanning the 2026-05-29 wedge (1000+ ms tail ×159). The
  **steady-state floor of ~180–370 ms** (20 most recent samples) is the honest
  live ceiling; use that, not the mean.
- **INFERRED (medium confidence):** that update_tip's cost is LevelDB fsync
  (vs evidence-build) — consistent with the code path and the ~200 ms floor, but
  not yet split (needs instrumentation #1). That the ~11 s p2p_services_start is
  dominated by projection backfill — supported by the `[phase4] … caught up to
  offset=25384402` log lines, but not separately timed.
- **NOT MEASURED:** a true clean warm-restart total in isolation (the live
  datadir cannot be safely snapshotted while the node writes it; copying a 14 GB
  live-written LevelDB risks a torn read). The 37.7 s BENCHMARKS_LOG row and the
  ~44 s `[boot] total` here use different start datums and should not be
  compared directly.

## Safety confirmation

The live node datadir (`~/.zclassic-c23`) and zclassicd's datadir
(`~/.zclassic`) were never written to. The live `zclassic23` service and
`zclassicd-rhett` were never stopped or restarted. All timing data was harvested
read-only from `~/.zclassic-c23/node.log`. The only state-mutating run used a
throwaway `/tmp/zcl-bench-fresh` datadir (reading zclassicd's LevelDB read-only).
No `-cold-import` was run.
