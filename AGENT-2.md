# AGENT-2 — Wallet / Storage / App-Layer / Net / Validation

**Working directory:** `~/zclassic23-2` (separate clone; pushes to `origin/main`).
**Coordinator:** Rhett (`~/zclassic23`), coordinator-only — does not code.
**Sibling:** Agent-3 (`~/zclassic23-3`), in the crypto / sapling / consensus-crypto lane.

See [`AGENT.md`](AGENT.md) for the cross-agent priority table and
every row's full description. This file is Agent-2's executable
checklist plus the lane rules.

---

## Lane — what you may edit

**Full edit access:**
- `lib/wallet/`, `lib/storage/`, `lib/coins/`
- `lib/net/` (except Agent-3's P7.4 tip-watchdog scope in
  `msgprocessor.c` + `download.c`)
- `lib/validation/` (except `sigops.c` + `check_block.c` — Agent-3)
- `lib/script/`, `lib/rpc/`, `lib/event/`
- `lib/util/` (excluding crypto helpers)
- `app/controllers/`, `app/services/`, `app/models/`, `app/views/`
- `tools/mcp/`, `tools/mcp/controllers/`
- `lib/sync/` (new — P16 staged-sync port)
- `lib/test/` — add/modify tests for your changes
- `deploy/zclassic23.service`
- Repo-root hygiene: `.gitignore`, `Makefile`, tracked binaries

**Read-only / off-limits:**
- `lib/crypto/`, `lib/sapling/`, `lib/keys/` — Agent-3
- `lib/validation/src/sigops.c`, `lib/validation/src/check_block.c` — Agent-3
- `lib/core/src/random.c` — Agent-3
- `vendor/` — Agent-3 owns `vendor/tor`; all other vendor dirs are pinned

**STOP + ping Rhett triggers:**
- Any change to on-disk serialization (blocks, UTXOs, wallet keystore)
- Any change to consensus constants (MAX_BLOCK_SIZE, MAX_BLOCK_SIGOPS, MAX_P2SH_SIGOPS)
- Any change to the P2P protocol wire format
- Any change to a Priority Group's scope or acceptance criteria (coordinator owns the plan)

---

## Current status — NOW = P24.18a → 18b → 18c → P24.19 → 20 → 21 → 22 → 23 → 24 → 25 → 26 (sync-robustness wave, 11 rows)

## 🚀 KICKOFF — 2026-04-22 04:23 (from Rhett)

**Run kickoff mcp for zclassic23.**

Your P24.18 RED (`lib/test/src/test_unclean_shutdown_advance.c`, 93 lines) is
preserved on `origin/wip/agent-2-p24.18-red` — no loss risk if kickoff resets
the worktree. Restore after kickoff with:
```
git show origin/wip/agent-2-p24.18-red:lib/test/src/test_unclean_shutdown_advance.c \
  > lib/test/src/test_unclean_shutdown_advance.c
```
Then resume from the ACTION LIST **Step 2** below.

## 📡 MCP is live — use these tools instead of shell whenever possible (2026-04-22 04:30)

Coordinator registered `zcl23` as a Codex MCP (global, `~/.codex/config.toml`).
`.mcp.json` is also in the repo root so Claude Code auto-discovers it.

**To verify** in any Codex session: `codex mcp list` — you should see
```
zcl23  /home/rhett/zclassic23/zclassic23  -mcp  -  -  enabled  Unsupported
```

In Codex, the tools surface as `zcl23.<name>` (or `zcl23__<name>` in some clients).
Full list via `zcl_tools_list`; most useful for YOUR work:

| Task | MCP tool | Shell fallback |
|---|---|---|
| Check node state | `zcl_status` | `./tools/zcl-rpc getblockcount` |
| Peer count + latency | `zcl_peers`, `zcl_peerlatency` | `./tools/zcl-rpc getpeerinfo` |
| Tail events | `zcl_events` | `./tools/zcl-rpc listevents` |
| Sync diagnosis | `zcl_syncstate`, `zcl_validationstatus` | grep node.log |
| Run arbitrary RPC | `zcl_rpc(method=..., params=...)` | `./tools/zcl-rpc <method> <args>` |
| Log tail | `zcl_logtail(lines=N)` | `tail -N ~/.zclassic-c23/node.log` |

**⚠ UNSAFE against live node** (will be safe after P24.14 canary and P24.18
land): `zcl_syncdiag` (was crashing the node — P24.11 fixed; verify before
using), `zcl_walletaudit`, `zcl_listunspent`, `zcl_z_listunspent`,
`zcl_rescanblockchain` (P24.14 fixed these on main but live node at
h=3,078,014 still has pre-P24.14 binary if you haven't redeployed since
coordinator's last deploy).

**Coord tools (zcl_coord_*) are P25 scope — NOT built yet.** P25.1 is your
row to build them. When P25 lands, you'll use `zcl_coord_mail_inbox` on
kickoff instead of re-reading this file.

---

## ⚡ ACTION LIST — Agent-2 — 2026-04-22 04:00

**The live node is stalled at h=3,078,014. You unstick it by landing
P24.18 with the sticky-recovery approach you started. Do these exact
steps in order.**

### Step 1 — pull + verify your RED exists

```
cd ~/zclassic23-2
git fetch origin && git checkout main && git reset --hard origin/main
ls -l lib/test/src/test_unclean_shutdown_advance.c   # should show 93 lines, untracked
```

If the file is missing, restore from side branch:
```
git show origin/wip/agent-2-p24.18-red:lib/test/src/test_unclean_shutdown_advance.c \
  > lib/test/src/test_unclean_shutdown_advance.c
```

### Step 2 — commit RED first (before GREEN)

Add it to the test registry then commit:
```
# lib/test/include/test/test_helpers.h — add forward decl:
#   int test_unclean_shutdown_advance(void);
# lib/test/src/test.c — add call site under the test runner
git add lib/test/src/test_unclean_shutdown_advance.c \
        lib/test/include/test/test_helpers.h \
        lib/test/src/test.c
git commit -m "test/P24.18: RED for sapling-persist 3-fail threshold + rebuild"
git push origin main
```

The test will FAIL to compile until you land Step 3 — that's expected
RED shape.

### Step 3 — write GREEN (4 edits)

1. **`lib/event/include/event/event.h`** — add new enum value:
   ```
   EV_SAPLING_PERSIST_FAIL,   /* P24.18: sapling tree persist failure */
   ```

2. **`lib/validation/include/validation/process_block.h`** — add test-only
   declarations:
   ```c
   #ifdef ZCL_TESTING
   void process_block_test_fail_next_sapling_persists(int n);
   bool process_block_test_persist_sapling_tree(bool force);
   extern _Atomic bool g_sapling_tree_rebuilding;
   #endif
   ```

3. **`lib/validation/src/process_block.c`** — at/around line 293-304,
   replace the single fprintf-and-continue with the 3-fail counter:
   ```c
   static _Atomic int g_sapling_persist_fail_count = 0;
   static _Atomic int g_sapling_persist_test_force_fail = 0;
   _Atomic bool g_sapling_tree_rebuilding = false;

   /* In flush_coins, replace:
      if (!node_db_state_set(ndb, "sapling_tree", ts.data, ts.size))
          fprintf(stderr, "flush_coins: sapling_tree persist failed\n");
      with: */
   bool persist_ok;
   int force_fail = atomic_fetch_sub(&g_sapling_persist_test_force_fail, 1);
   if (force_fail > 0) {
       persist_ok = false;
   } else {
       persist_ok = node_db_state_set(ndb, "sapling_tree", ts.data, ts.size);
       atomic_store(&g_sapling_persist_test_force_fail, 0);
   }
   if (!persist_ok) {
       int fails = atomic_fetch_add(&g_sapling_persist_fail_count, 1) + 1;
       event_emitf(EV_SAPLING_PERSIST_FAIL, 0, "fails=%d", fails);
       if (fails >= 3) {
           atomic_store(&g_sapling_tree_rebuilding, true);
           atomic_store(&g_sapling_persist_fail_count, 0);
           ok = false;   /* propagate to connect_tip */
       }
   } else {
       atomic_store(&g_sapling_persist_fail_count, 0);
   }
   ```

4. **Same file**, add test-only entry points (guarded by `#ifdef ZCL_TESTING`):
   ```c
   #ifdef ZCL_TESTING
   void process_block_test_fail_next_sapling_persists(int n) {
       atomic_store(&g_sapling_persist_test_force_fail, n);
   }
   bool process_block_test_persist_sapling_tree(bool force) {
       (void)force;
       /* Call into the same persist path flush_coins uses — extract helper
        * static bool sapling_tree_persist_once(void) if needed. */
       /* Must update g_sapling_persist_fail_count / rebuild flag as side-effect. */
       return sapling_tree_persist_once();
   }
   #endif
   ```

### Step 4 — build, test, commit GREEN

```
make -j$(nproc) test_zcl && ./test_zcl 2>&1 | grep -E "unclean_shutdown|ALL TESTS|SOME TESTS" | tail -5
```

If green, commit + push:
```
git add lib/event/include/event/event.h \
        lib/validation/include/validation/process_block.h \
        lib/validation/src/process_block.c
git commit -m "validation/process_block: sapling persist 3-fail threshold + rebuild (P24.18 GREEN)"
git push origin main
```

### Step 5 — deploy to live node and watch tip advance

```
cd ~/zclassic23-2
make deploy   # will trigger 13-min Sapling rebuild (P24.19 will fix that later)
# After ~13-15 min, check tip:
./tools/zcl-rpc getblockcount
# Expect: advancing past 3,078,014 toward ~3,086,400
```

If tip advances: rotate NOW to **P24.19** in AGENT-2.md and push
`agents: P24.18 landed, tip advancing — rotate to P24.19`.

If tip does NOT advance: you need P24.18b after all (end-height loop
in sync_controller.c:1506). File the evidence from `~/.zclassic-c23/node.log`.

### If stuck

Your Codex log showed activity through 03:27 but you haven't pushed.
If you're stuck on any of Steps 3's sub-edits, push what you have to
`wip/agent-2-p24.18-in-progress` so coordinator can help. The node
stays stalled until this lands.

---

## ⚡ (SUPERSEDED) 2026-04-22 03:30 — you're on P24.18c-shape; that's fine

---

## 🎯 IMPORTANT 2026-04-22 02:30 — P24.18 SPLIT into 3 narrower rows (SUPERSEDED by above note)

**Your NOW is NOW P24.18a** — NOT the monolithic P24.18. Coordinator
split it because the 3-part fix was too cross-cutting to land as one
commit. Work the split sequentially:

- **P24.18a (NOW):** repro + diagnostic. Build a deterministic RED test
  in `lib/test/src/test_stall_repro.c` that reproduces the live-node
  stall on a fixture. ADD diagnostic printf in `flush_coins` to print
  sqlite errmsg when `node_db_state_set("sapling_tree", ...)` fails.
  Deploy THIS row alone. Read `node.log` to see the actual failure
  mode. **Coordinator's hypothesis in P24.18b may be wrong — P24.18a
  verifies first.**
- **P24.18b:** end-height fix (loop until rebuild is stable). Land
  only if P24.18a confirms the end-height is actually the bug.
- **P24.18c:** error propagation + sticky recovery (event emit, 3-fail
  threshold, rebuild-on-demand).

**Why this split:** you've been on P24.18 for ~2 hours with nothing
pushed. The row is too cross-module for one commit. P24.18a is ~100
lines and pushable in <1 hour. It gives us EVIDENCE of what's actually
broken, which may be different from coordinator's hypothesis. That
evidence lets us land 18b with confidence, or redirect if the real
bug is elsewhere.

**Keep the RED-first discipline:** every row = RED commit + GREEN
commit. Even P24.18a's diagnostic printf is a "GREEN" after the RED
test harness.

## 🎯 COORDINATOR MANDATE 2026-04-22 02:00 — "STICKY, STRONG, ROBUST" (wave extended +3)

Rhett re-affirmed the mandate after P13.1 landed. Wave is now 7 rows
deep — the original cascade fixes PLUS three structural hardening rows
that prevent the cascade class from recurring. **Work IN ORDER.**

1. **~~P13.1~~ ✅** landed 96c8d32c6 — addnode-drain fallback (HIGH).
2. **P24.18** CRIT — connect_tip silent-fail + Sapling rebuild end-height. **Live node stuck at h=3,078,014 right now because of this.** 3-part fix: rebuild to current tip, propagate persist errors, sticky recovery on mismatch.
3. **P24.19** CRIT — clean-shutdown marker + WAL checkpoint on SIGTERM. Kills 13-min unclean-boot cycle so every deploy becomes cheap (≤30s restart).
4. **P24.20** HIGH — connect_tip/flush_coins error propagation to `zcl_events` / `zcl_kpi`. Silent failures become observable failures.
5. **P24.21** HIGH — UTXO anchor roll-FORWARD on boot (re-validate missing blocks) instead of roll-BACKWARD.
6. **P24.22** HIGH (NEW 2026-04-22 02:00) — Boot-time chain-consistency invariant assertion. Four authoritative values (`A=active_chain, B=coins_best_block, C=max(blocks.height), D=sapling_rebuild_height`); assert `A==B && A<=C && D<=A` on boot. Violation → boot-readonly mode + `EV_BOOT_INVARIANT_VIOLATION`. Today's node boots with all four values wrong and tries to advance anyway.
7. **P24.23** HIGH (NEW) — Validated state snapshots every 1K blocks. Boot becomes "mmap most recent snapshot + replay ≤1K-block delta." Eliminates 13-min Sapling rebuild. (P12.1's flat-file Sapling checkpoint is single-value every 10K; this is full-state, every 1K, SHA3-checksummed.)
8. **P24.24** CRIT (NEW) — Transactional chain advance. Wrap `connect_tip` body in single SQLite transaction so block_index + coins + sapling_tree commits are atomic. P24.18 is one instance of this class; P24.24 is the structural fix so no future regression can slip the same mistake.

### Why this order

1. **P24.18 first (NOW)** — THE stall. Live node advances past h=3,078,014 only when this lands. Every downstream row needs working foreground sync to canary.
2. **P24.19 second** — without clean-shutdown, every deploy re-triggers P24.18's cascade. P24.19 makes the fix durable across deploys.
3. **P24.20 third** — observability. Rows after this depend on SEEING sync-error signals in `zcl_events` / `zcl_kpi`.
4. **P24.21 fourth** — roll-forward. Chain tip never moves backward on boot.
5. **P24.22 fifth** — boot invariant. P24.21 moves tip correctly; P24.22 asserts tip-consistency is impossible to violate silently (booting refuses rather than silently-corrupting).
6. **P24.23 sixth** — snapshots. Requires invariant check (P24.22) to trust snapshot contents. Deploy cost drops to seconds.
7. **P24.24 seventh** — transactional advance. Generalization of P24.18; structural fix preventing a whole class of future regressions.

### Expected KPI lift (pillar-by-pillar — full extended wave)

| Pillar | Before wave | After wave | Reason |
|---|---|---|---|
| Correctness | 2 | 9 | Tip advances reliably; atomic commit prevents partial-advance corruption; boot invariant catches silent divergence. |
| Robustness | 6 | 9 | No more unclean-boot rebuild; snapshot boot; transactional advance. |
| Operability | 5 | 9 | `make deploy` cost drops from ~14 min to <30s. |
| Observability | 7 | 9 | All sync errors fire events + KPI; boot invariant fires events. |
| Test discipline | 7 | 9 | Crash-injection test (P24.24), boot-invariant test (P24.22), snapshot test (P24.23) add new test classes. |

**Total expected jump: ~55 → ~82 points.** This extended wave covers more than half the gap from current KPI to 100.

### Operational notes

- Do NOT try to bootstrap the live node manually to clear the stall. Coordinator is not doing that intentionally — running the legacy-bootstrap escape hatch would hide the bug and defer the fix. Your P24.18 fix must be the thing that unstucks the live node. Canary is "deploy → watch tip advance."
- ~/zclassic23-2 (your worktree) already has P24.14 merged (9c1794086). `git pull` after kickoff-reset to pick up all coordinator changes.
- Agent-3 is NOT on this wave — they're on the MVP drain (P11.5 store e2e, P11.8 parity diff). Don't coordinate with them on these rows.

### Rotation instructions

When you finish each row, BEFORE rotating NOW:
1. Deploy via `make deploy` if the row affects the live node (P24.18+).
2. Canary: confirm the row's acceptance criteria (listed per-row in AGENT.md).
3. Update your `## Current status` header to `NOW = <next row>`.
4. Commit the AGENT-2.md rotation along with the row's RED+GREEN commits.

---

## (historical) NOW = P13.1 (P24.14 LANDED 2026-04-21 21:00, pending deploy)

**P24.13 done b466740d2 [test:1.0 7c540ddfb]** — landed by coordinator
while Agent-2's independent in-progress implementation (local commit
d6de2bb01) had been kickoff-reset. If your local clone still shows
d6de2bb01 unpushed, do `git pull --rebase` — your commit will be
dropped as a patch-identical duplicate of b466740d2, OR produce a
small conflict you can resolve by taking the origin version. Then
advance to P24.11 below.

Live-canary post-deploy (2026-04-21 05:54-05:58):
- header_gap closed from 3,862 → 0 in 81s
- block height advanced 3,081,601 → 3,082,953+ (catching up ~3 blocks/sec)
- zero `bad-diffbits` rejections in `zcl_events`
- `chain.headers >= chain.blocks` invariant holds (closes P24.5 side-effect)

**P14.6 done 5994bc3b1 [test:1.0 de30f389d]** — extract the inline
`connect_tip` propagation into a testable helper
`process_block_propagate_failed_child` and gate it with two cheap
early returns: SKIP_PARENT_FAILED (when `pindex_root->pprev` already
carries `BLOCK_FAILED_MASK` — the prior propagation already covered
this subtree) and SKIP_RATE_LIMITED (a static 10-second per-process
window on the full block_map walk). At the live tip each walk is
~24 MB scratch + O(N log N) qsort across ~3M entries; pre-fix the
2026-04-19 BIP30 flap re-fired ~5×/s for hours, driving RSS from
5.9 GB to the cgroup high-water in 2h51m. Post-fix, the worst the
flap can do is one walk per 10 s. Tests in
`lib/test/src/test_p14_6_failed_child_cap.c` assert both guards
against a 5-entry fixture block_map. **Side-effect: closes P12.2**
(BLOCK_FAILED_CHILD GC).

**P24.11 done ffad7cf7d [test:1.0 ab1e88a1b]** — centralized JSON-RPC
response envelope in `rpc_http_test_build_response_envelope`, routed
`handle_client` through the same helper as the RED test. Agent-2 had
these on a local branch when coordinator rotated your NOW; coordinator
rebased onto origin/main (ab1e88a1b + ffad7cf7d) and pushed to preserve
your work. If you kickoff-reset before pull, the same patches are now
upstream — just `git pull` and move on to P24.14.

**P24.14 done 9c1794086** — squash-merged to main 2026-04-21 ~21:00. New
chokepoint `rpc_require_chainstate_lookup_ready()` in
`app/controllers/src/rpc_chainstate_guard.{c,h}`; all 16 callsites across
5 controllers (transaction, chain_inspect, wallet_diagnostic,
wallet_rescan, repair, wallet) now guard before hitting the cache.
`lib/test/src/test_rpc_safety.c` (198 lines) exercises the guard paths
with a fixture-based harness. Lint gate `check_coins_lookup_nullcheck.sh`
catches future regressions. `make -j && ./test_zcl` green. Side branch
`codex/p24-14-chainstate-guard` also pushed.

**P13.1 done 96c8d32c6** — addnode-drain fallback. Prioritizes `-addnode` list over addrman when tried bucket exhausted; RED `test_connman_addnode_fallback.c` (preserved via side-branch 7140999a8) fully incorporated into GREEN merge. Coordinator deploying now. **Agent-2 rotate to P24.18 (next below).**

**P13.1 HIGH — single-peer sync regression (HISTORICAL CONTEXT, kept for later rows referencing it).**

Evidence (live canary post-P24.11 deploy 2026-04-21 20:25): node came back
up with only **2 peers** (both inbound magicbean-legacy), zero outbound
despite 10 `-addnode` entries in the systemd ExecStart:
```
-addnode=127.0.0.1:8034 -addnode=140.174.189.17 -addnode=140.174.189.3
-addnode=37.187.76.79 -addnode=162.55.92.62 -addnode=157.90.223.151
-addnode=157.173.195.203 -addnode=85.239.232.93 -addnode=154.38.178.121
-addnode=51.178.179.75
```
`addrman_select` was spamming "exhausted tried bucket search after 200k
iterations" in node.log 2026-04-21 20:25. Block-download throughput
crashed to 0 blocks/sec (blocks_download state + header_gap=0 but
verified_height stuck at 3,079,470 for 10+ min).

**Fix shape (evidence-first):**
- (a) Reproduce the addrman exhaustion: does `-addnode` populate the
  `tried` bucket, or just `new`? If addnode entries aren't tried-promoted
  on successful handshake, `select_tried` will spin on the 200k guard.
- (b) Audit `lib/net/src/addrman.c:539` (`addrman_select` 200k exhaustion
  path) — the guard itself is correct; the issue is the caller's
  recovery. Right now we log and return NULL; connman probably retries
  the same bucket.
- (c) Fallback: when `addrman_select` exhausts, fall back to the
  `-addnode` list directly instead of waiting for addrman to populate.

**RED test shape** — `lib/test/src/test_connman_addnode_fallback.c` (new):
- Construct main_state + empty addrman.
- Register 10 `-addnode` entries.
- Call `connman_pick_next_outbound_target()` 10 times.
- Assert all 10 are dialed before any addrman lookup.
- Pre-fix: addrman returns NULL, connman stalls.

**⚠️ RESUME HINT (coordinator 2026-04-22 01:00):** You already wrote 76
lines of this RED test in a prior session — it survives kickoff-reset
(untracked files are preserved by `git reset --hard`). Check for
`~/zclassic23-2/lib/test/src/test_connman_addnode_fallback.c` BEFORE
starting from scratch. The file references:
- 5-arg `connman_pick_next_outbound_target(cm, cursor, pick, source, addnode_index)`
- `enum connman_outbound_target_source` with `CONNMAN_TARGET_NONE` and `CONNMAN_TARGET_ADDNODE`
- `cm.addnodes[]`, `cm.num_addnodes`, `cm.next_addnode_cursor` fields
- `connman_record_addnode_attempt(cm, index, ok)`

These are all the GREEN APIs you must implement in `lib/net/include/net/connman.h` + `lib/net/src/connman.c`. If the file exists, push it first as `test/P13.1: RED for addnode-drain fallback` so it survives any future kickoff, THEN implement GREEN. The 5-arg signature is right; preserve it.

**Acceptance (live canary):**
- Post-deploy, node reaches **≥ 8 outbound peers** within 60s (each
  `-addnode` target should successfully connect).
- `zcl_events` shows zero `addrman_select` exhaustion warnings.
- `blocks/sec` > 100 during post-handshake block download.

**After P13.1 lands:** P14.4, P14.5, P14.15, P14.16 (P14 drain) →
P13.2/P13.3/P13.5/P13.6/P13.7 → P12.3/P12.3.1/P12.5-8 → P7/P8 drain →
P15-P23.

---

### Historical — P24.13 sync-stall evidence (FIXED by b466740d2)

Evidence from live-node `zcl_events` scan 2026-04-21 02:25 (pre-fix):

```
sync.headers_rejected  peer=0   header[0] 000009f9...  reason=bad-diffbits
sync.headers_rejected  peer=0   header[1] 0000126d...  reason=bad-prevblk
sync.headers_rejected  peer=0   header[2] 000002f4...  reason=bad-prevblk
sync.headers_rejected  peer=0   all 160 headers rejected
```

Every one of 12 peers sending the same 160-header batch (all at h=3,085,141), every few seconds, **all rejected**. The inverted `header_height (3,081,408) < height (3,081,601)` is not a cosmetic issue — it is the MECHANISM by which sync stalls:

1. FlyClient UTXO snapshot placed us at h=3,081,601
2. Chain-restore backfilled block_index up to h=3,081,408
3. 193 blocks (3,081,409–3,081,601) have NO block_index entries, or have default/zero `nBits`
4. Peer sends header at h=3,081,602
5. `check_block.c:241` calls `GetNextWorkRequired(pindex_prev)` to compute expected nBits
6. `GetNextWorkRequired`'s 17-block averaging window walks back from 3,081,601 → hits the 193-block gap → cannot complete → returns `nProofOfWorkLimit` (weakest allowed, per comment at `check_block.c:231-238`)
7. Peer's actual nBits (the real network difficulty) ≠ `nProofOfWorkLimit` (weakest) → **`bad-diffbits` REJECT**
8. Loop forever

The comment at `check_block.c:236-239` explicitly calls out this failure mode: *"Callers that legitimately accept headers without local-window validation (fast-sync snapshot tail, MMB-proved headers) MUST bypass this function entirely — see process_block.c's `skip_contextual` gate."* **The `skip_contextual` gate is not firing for post-FlyClient-snapshot headers_download.** That's the bug.

**Fix shape (3 options, pick one or combine):**

- **(a)** Extend `skip_contextual` in `process_block.c` to cover headers whose prev-block height is within `tip_height - 17` to `tip_height` when block_index is non-contiguous. Cheapest fix.
- **(b)** Backfill block_index entries 3,081,409..3,081,601 during chain-restore (expand `chain_restore_backfill_nbits_from_disk` to walk past the block_index end, using whatever source has the real nBits — likely `disk_block_io` or zclassicd parity). Correctness-complete but requires data source.
- **(c)** Dedicated `header_backfill_phase` between fast-sync and headers_download that explicitly requests the 193-block range from a peer, verifies against FlyClient MMB roots, writes to block_index. Clean but more code.

**Best is (a) short-term + (c) long-term.**

**RED test first** (`lib/test/src/test_chain.c` new case "post-snapshot headers_download accepts valid mainnet headers"):
- Build fake chain-restore state with tip_h=100, block_index populated 0..83, missing 84..100 (similar 17-block inversion).
- Feed a batch of 10 headers starting at h=101 with real difficulty transitions.
- Assert tip advances to 110 without any `bad-diffbits` reject.
- Pre-fix: all 10 headers rejected.

**Acceptance (live canary):**
- Post-fix deploy, tip advances from 3,081,601 toward 3,085,141+ within 5 minutes.
- `zcl_events` shows zero `sync.headers_rejected` with `reason=bad-diffbits`.
- `chain.headers >= chain.blocks` invariant holds (closes P24.5 side-effect).

**File path pointers:**
- `lib/validation/src/check_block.c:224-251` — the reject site
- **`lib/validation/src/process_block.c:902-911`** — the `skip_contextual` gate (see PROGRESS HINT below)
- `app/services/src/chain_restore_service.c:433` — `chain_restore_backfill_nbits_from_disk` (existing backfill infra for option (b))
- `lib/test/src/test_chain_restore_service.c:757,816` — existing RED test pattern for backfill (copy this shape for P24.13 RED)
- `app/services/src/utxo_recovery_service.c:475` — already has a comment acknowledging this class of bug

**RESUME-HERE DESIGN (added 2026-04-21 05:30 — preserving Agent-2's in-flight fix shape after a kickoff reset wiped 161 lines of WIP):**

Agent-2 had the right approach drafted before the reset. If you're
reading this fresh, implement exactly this — it's Option (a) from
the hints below, done cleanly as two functions:

```c
/* lib/validation/src/process_block.c — new helpers */

/* Returns true iff the pprev chain from pindex_prev can be walked
 * backward `pow_window` steps with contiguous nHeight values
 * (each pprev->nHeight + 1 == cursor->nHeight).  Used to detect
 * the post-FlyClient-snapshot tail where block_index entries
 * 3,081,409..tip exist but have no pprev linkage.
 */
static bool process_block_pow_window_complete(
    const struct block_index *pindex_prev,
    int pow_window)
{
    const struct block_index *cursor = pindex_prev;
    if (!cursor || pow_window <= 0) return true;
    for (int i = 0; i < pow_window; i++) {
        if (!cursor->pprev) return false;
        if (cursor->nHeight != cursor->pprev->nHeight + 1) return false;
        cursor = cursor->pprev;
    }
    return true;
}

/* Exported gate — replaces the inline bool skip_contextual expression
 * at process_block.c:904-905.  Skips contextual_check_block_header when:
 *   - tip > 100000 AND pindex_prev is >1000 behind tip (old IBD case), OR
 *   - the 17-block GetNextWorkRequired window cannot be walked
 *     contiguously back from pindex_prev (P24.13 inverted-tail case).
 * consensus->pow_averaging_window is 17 for mainnet. */
bool process_block_should_skip_contextual_header(
    const struct main_state *ms,
    const struct block_index *pindex_prev,
    const struct consensus_params *consensus);
```

In `lib/validation/include/validation/process_block.h` add the extern
declaration for `process_block_should_skip_contextual_header` (+10 lines).

At `process_block.c:902-906` replace the inline boolean with a call to
the new exported function.

**RED test** (`lib/test/src/test_chain.c` new case, ~82 lines):
Build an in-memory block_index tree where `tip` is at height 100,
`pindex_prev` is at height 100 (tip itself), but `pindex_prev->pprev`
chain only extends back 5 blocks contiguously before breaking
(simulating the 193-block inversion in miniature). Call the gate:
- With `pow_window = 17` → expect TRUE (skip contextual, can't walk window)
- With `pow_window = 3` → expect FALSE (window fits inside contiguous region)
- Also test: pindex_prev == NULL → FALSE (safety fallback)

**Lint gate wiring** (`lib/test/src/test_make_lint_gates.c`, +18 lines):
Make sure the new symbol `process_block_should_skip_contextual_header`
passes the `check_nodiscard` + `check_raw_sqlite` lints (it's pure C,
no sqlite, so this is just ensuring the symbol is declared properly).

**Commit shape (2 commits):**
1. RED: `test/chain: RED for P24.13 skip_contextual inverted-tail gate`
   — just the new test case, imports from process_block.h that
   don't exist yet, expect a link error (or FAIL compilation gate).
2. GREEN: `validation/process_block: skip contextual when PoW window
   can't walk (P24.13 GREEN)` — the two helpers + header + call-site swap.

After push, coordinator will `make deploy`. Expected live-canary:
- Tip advances from 3,081,601 toward 3,085,271+ within 5 min
- `zcl_events` shows zero `sync.headers_rejected reason=bad-diffbits`
- `chain.headers >= chain.blocks` holds (closes P24.5 side-effect)

---

**PROGRESS HINT (added 2026-04-21 05:08 by coordinator — if you were stuck, start here):**

The exact gate that is failing to fire is at `process_block.c:902-906`:
```c
int tip_h = active_chain_height(&ms->chain_active);
bool skip_contextual = (tip_h > 100000 && pindex_prev &&
                        pindex_prev->nHeight < tip_h - 1000);
if (pindex_prev && !skip_contextual &&
    !contextual_check_block_header(header, state, params, pindex_prev,
                                    ms->fCheckpointsEnabled))
    LOG_FAIL(...);
```

Plug in the live P24.13 numbers:
- `tip_h = 3,081,601` → `tip_h > 100000` ✓
- New incoming header is at h=3,081,602 → `pindex_prev->nHeight = 3,081,601` (the tip)
- Gate: `3,081,601 < 3,081,601 - 1000 = 3,080,601` → **FALSE**
- Therefore `skip_contextual = FALSE` → contextual_check runs → GetNextWorkRequired's 17-block window can't walk the 193-block inverted region → returns weakest-allowed → peer's real nBits mismatches → reject.

**The `-1000` slack was designed for reorg/scrambled-height recovery, not for tail-append during post-snapshot header download.** Approach options (you choose — this is your lane):

- Tightest fix: widen the gate to also skip_contextual when `pindex_prev->nHeight >= tip_h - 17` (the GetNextWorkRequired averaging window — if pindex_prev is inside or abutting the window and the window can't walk through the inverted tail, we already know contextual-check will spuriously fail). One-line change.
- Phase-flag fix: add a bool `ms->in_post_snapshot_header_backfill` that sync service sets TRUE during the known-inverted phase; gate on it. Cleaner but touches sync service too.
- Backfill fix (option (b) from above): extend `chain_restore_backfill_nbits_from_disk` to walk past the block_index end and populate entries 3,081,409..3,081,601 from `disk_block_io`. Correctness-complete but more code.

Recommended order: (1) tightest fix first to unblock live mainnet sync, (2) backfill fix as a hardening follow-up row (could be a new P24.15), (3) write the RED test against the WIDENED gate so regression is caught if someone narrows it back.

---

## (historical) NOW = P14.6

**P14.3 done 5406beca3 [test:1.0 63016db95]** — `rpc_getsyncdiag` in
`app/controllers/src/health_controller.c` declared `struct json_value
wd;` and `hdr;` without initialization. `json_set_object()` calls
`json_free()` on the struct before re-typing, reading uninitialized
`type`/`num_children`/`children` and calling `free()` on garbage.
Applied `= {0}` + post-push `json_free(&wd)` / `json_free(&hdr)` —
same pattern sibling RPCs use (`chain`, `bgv`, `bgh`, `svc`).
Local live-verify: 10× `getsyncdiag` calls, node alive, well-formed
JSON. **⚠ Only a PARTIAL fix of the MCP tool.** The coordinator
canary on 2026-04-21 02:11 proved that `h_zcl_syncdiag` in
`tools/mcp/controllers/ops_controller.c:491` composites THREE RPCs
(`getsyncdiag` + `downloadstats` + `getpeerinfo`); one of the other
two still SIGABRTs the node on the live path. Filed as **P24.11
CRITICAL** — queued immediately after P14.6.

**P14.14 done 9d71841ba [test:1.0 9f114c251]** — after the bucket-fill,
the rebuild pass walks chain bottom-up and (for every slot whose pprev
is NULL) wires it to chain[h-1], then calls `block_index_build_skip()`
on any slot whose pskip is NULL. Bottom-up ordering lets build_skip
reuse each parent's freshly-built pskip → O(tip_h · log tip_h) total
(~1.7M ops at live tip).

**P14.10 done 8b5443a8d [test:1.0 fd23f77a3]** — atomic
`deferred_pending` counter on the controller; SKIP_ALREADY_RUNNING
increments it, the activator drain-loops (bounded 8 rounds) under the
mutex before transitioning out of CONNECTING.

**P14.13 done a62394130 [test:1.0 b07284439]** — single-pass bucketing
replaces the O(N²) residual-holes branch.

**Canary status (2026-04-21 02:10 post-`make deploy`):** fresh binary
booted clean, peers 3→18 immediately, chain-restore path completes
without hang, `getblockcount=3081601` within seconds. All four
chain-restore CRITICALs (P14.10 + P14.13 + P14.14 + P14.3 RPC-side)
validated on the live node. Tip not advancing yet — still
`headers_download` state with `header_height (3,081,408) < height
(3,081,601)` inversion (filed P24.5). Most likely P14.6 + P13.1 need
to land before tip catches up to legacy (h≈3,085,137).

**P24.11 (CRITICAL): second `zcl_syncdiag` crash path.** P14.3 fixed
`rpc_getsyncdiag` only; the MCP tool `h_zcl_syncdiag` in
`tools/mcp/controllers/ops_controller.c:491` composites THREE RPCs
(`getsyncdiag` + `downloadstats` + `getpeerinfo`), and one of the
other two still SIGABRTs on the live path. Blocks coordinator MCP
diagnostics. **Agent-2 NOW.** RED-first: instrument
`rpc_downloadstats` + `mcp_node_rpc` to isolate which internal RPC
is the crash site; reproduce on a fixture; minimal `= {0}` +
`json_free(&X)` fix following P14.3's pattern; live-verify 10×
`zcl_syncdiag` calls with no abort.

(historical P14.13 description retained below for context)

**P14.13 (CRITICAL): `chain_restore_rebuild_active_chain` is O(N²), boot hangs.**

The residual-holes branch does `tip_h × map_size ≈ 10 trillion` ops
when the anchor's `pprev` is NULL (which is always, post-P14.11 — the
anchor is synthesized at the UTXO layer). Fresh binary pins at ~92%
CPU for >5 min with no RPC; coordinator has to SIGTERM every boot.

**Fix shape (preferred):** single-pass bucketing. Scan `block_map`
ONCE into a temporary `block_index*[tip_h+1]` array bucketed by
height (24 MB alloc at live scale — safe, system has 95 GB RAM).
Then fill `c->chain[h]` from buckets in one O(tip_h) pass.
Alternative: reconnect `anchor->pprev` via
`block_map_find(&ms->map_block_index, &anchor_header.hashPrevBlock)`
before calling rebuild, so the pprev-walk completes in O(tip_h).
**Best is both** — option 2 avoids the alloc on the happy path,
option 1 prevents pathology on any pprev-NULL variant.

**RED test first** (non-negotiable):
- New case `chain_restore_rebuild: completes in <2s at realistic chain depth`
  in `lib/test/src/test_chain_restore_service.c`.
- Build a synthetic block_map with N=100,000 entries; seed the tip with `pprev=NULL`.
- Assert wall-clock <2s; fails pre-fix.

**Acceptance (canary, Rhett's row):**
- Boot reaches `[chain-integrity] post-restore check OK` within 30s of
  `Chain restore: anchor at h=...`.
- RPC up within 60s of service start.
- `getblockcount` returns 3,081,601+ within 90s.
- `getblockhash 3081000` returns a valid hash (P14.12 success signal).
- Zero `bad-diffbits` lines (P14.11 success signal).

**File:** `app/services/src/chain_restore_service.c:322-391`
(`chain_restore_rebuild_active_chain`).

---

## POST-P14 WORK QUEUE — the shining-example roadmap

Filed 2026-04-20 after the coordinator's full review + Erigon / Caplin
architecture study. Work the list **in order** unless you hit a
blocker; when in doubt, ping the coordinator.

Every row has a full description in [`AGENT.md`](AGENT.md). This
section is the executable checklist.

### Phase 0 — Finish the P14 stall wave

- [x] **P14.13** CRITICAL — rebuild_active_chain O(N²) boot hang. **done a62394130 [test:1.0 b07284439]**.
- [x] **P14.10** CRITICAL — deferred-activation queue for `SKIP_ALREADY_RUNNING` from `process_new_block`. **done 8b5443a8d [test:1.0 fd23f77a3]**.
- [x] **P14.3** CRITICAL — `rpc_getsyncdiag` json_free on uninit stack. **done 5406beca3 [test:1.0 63016db95]** (partial — see P24.11).
- [x] **P14.6** CRITICAL — cap `BLOCK_FAILED_CHILD` propagation (OOM amplifier). **done 5994bc3b1 [test:1.0 de30f389d]** (closes P12.2 side-effect).
- [x] **P24.13** CRITICAL — `bad-diffbits` on every post-snapshot header batch. done b466740d2 [test:1.0 7c540ddfb] (coordinator 2026-04-21 05:54; header gap closed 3,862 → 0 in 81s post-deploy).
- [ ] **P24.11** CRITICAL — `rpc_getsyncdiag+0xCB` json_free UAF (real site, corrected from earlier rpc_downloadstats hypothesis via nm symbol resolution 2026-04-21 05:02). **Agent-2 NOW — highest priority.**
- [ ] **P14.4** HIGH — sync FSM flap debounce (279,135 events in hours on prior incident).
- [ ] **P14.5** HIGH — `val.block_connected` must fire on commit, not receipt.

### Phase 1 — Chain-restore extensions (the N1-N3 review findings)

- [x] **P14.14** CRITICAL — populate `block_index.skipList[]` on chain-restore path; `BuildSkip()` in the rebuild pass. **done 9d71841ba [test:1.0 9f114c251]**.
- [ ] **P14.15** HIGH — backfill `nChainTx` + `nSequenceId` alongside `nBits` in `chain_restore_backfill_nbits_from_disk`. Acceptance: every restored entry has `nChainTx != 0`.
- [ ] **P14.16** HIGH — per-entry CRC32 footer on `block_index` flat file. Acceptance: `[height-repair] repaired N` drops to 0 on clean boots; corruption detected at load, not silently mid-sync.

### Phase 2 — Finish P13 sync UX wave

- [ ] **P13.1** CRITICAL — single-peer sync regression (9 `-addnode` peers backing off).
- [ ] **P13.6** HIGH — DNS-seed fallback when addrman empty + addnode fails.
- [ ] **P13.2** HIGH — header tip oscillation (counter goes backwards to 480).
- [ ] **P13.4** HIGH — IBD throughput 5-8× slower than zclassicd.
- [ ] **P13.5** MED — addrman `find_node_by_service` lookup gap.
- [ ] **P13.3** MED — `connect_block_local: failed at height N` spam.

### Phase 3 — Finish P12 hardening wave

- [x] **P12.2** HIGH — BLOCK_FAILED_CHILD GC (closed by P14.6). **done via P14.6 5994bc3b1 [test:1.0 de30f389d]**.
- [ ] **P12.3** HIGH — continuous parity-diff service vs zclassicd.
- [ ] **P12.3.1** HIGH — `zcl_parity_status` MCP tool (gate for MVP #8).
- [ ] **P12.5** MED — audit every `coins_map_erase` call site for P10.1.4-class gaps.
- [ ] **P12.6** MED — structured JSON logging + per-subsystem rate limits.
- [ ] **P12.6.1** MED — `msgprocessor.c` 96-fprintf cleanup.
- [ ] **P12.6.2** MED — `process_block.c` 61-fprintf cleanup.
- [ ] **P12.7** MED — block-index height-repair root-cause.
- [ ] **P12.8** LOW — `zcl_health` RSS trajectory + alarm threshold.
- [ ] **P12.8.1** HIGH — `zcl_mvp_status` MCP tool.
- [ ] **P12.8.2** MED — `zcl_flush_stats` + `zcl_reorg_diff` MCP tools.

### Phase 4 — P7/P8 drain

- [ ] **P7.10** MED — migrate long-running loops to `thread_registry_shutdown_requested()`.
- [ ] **P7.11** MED — `nat.c:241` UPnP recv wall-clock + byte caps.
- [ ] **P8.4** MED — compact-block reconstruction O(n·m) → khash short-txid table.
- [ ] **P13.7** HIGH — `make lint` check-raw-sqlite covers `main.c`; wrap `main.c:507` + `main.c:555` in `AR_BEGIN_SAVE`.

### Phase 5 — P15 Discipline (prerequisite for P16)

*Attribution: Erigon per-subsystem `agents.md` + ruleguard discipline (LGPL-3.0 © Erigon Authors).*

- [ ] **P15.1** HIGH — remove `-Wno-unused-result`; annotate Agent-2 `lib/*/include/` with `[[nodiscard]]`. One PR per subsystem.
- [ ] **P15.2** HIGH — define `struct zcl_result` + `enum zcl_err` in `lib/util/include/util/result.h`. Migrate `app/services/src/*.c` one at a time, starting with `wallet_backup_service.c`.
- [ ] **P15.3** MED — adopt `[[gnu::cleanup(zcl_free_p)]]` for heap pointers; start with wire parsers (`zmsg.c`, `znam.c`, `zslp.c`, `msg_*.c`). `goto fail:` pattern deleted.
- [ ] **P15.5 (A2 portion)** MED — author `lib/{net,validation,storage,wallet,script}/agents.md` + `app/{services,controllers}/agents.md` + `tools/mcp/agents.md`.
- [ ] **P15.6** HIGH — ruleguard grep gates in `tools/scripts/check_*.sh`; wire into `make lint`. Five patterns listed in AGENT.md P15.6.

### Phase 6 — P16 Architecture (the big Erigon port)

*Attribution: Erigon `execution/stagedsync/` + `db/kv/` (LGPL-3.0 © Erigon Authors). Concepts only — no verbatim code ported.*

**Start gate:** P15.1 + P15.2 landed.

- [ ] **P16.1** HIGH — `struct zcl_stage` + runner in `lib/sync/`. Trivial test stage drives Forward → Unwind → Prune.
- [ ] **P16.2** HIGH — port sync FSM to 9-stage shape: `flyclient_snapshot → headers → bodies → sigs → connect_blocks → utxo_flush → sapling_tree → tx_lookup → finish`.
- [ ] **P16.3** MED — per-stage wall-clock timings surfaced via `zcl_status.sync.stage_timings`. Replaces P14.9's dual IBD reporters.
- [ ] **P16.4** MED — per-stage `Cfg` struct pattern applied to all `app/services/*.c`.
- [ ] **P16.5** MED — `docs/NAMING.md` + grep-gate for ambiguous `height`.
- [ ] **P16.6** MED — `lib/storage/include/storage/temporal.h` unified interface.
- [ ] **P16.7** MED — `zcl_stream` iterator with `[[gnu::cleanup]]` auto-finalize. Migrate 140+ `sqlite3_step` sites.

### Phase 7 — P17 Testing support (Agent-2 share)

- [ ] **P17.4** HIGH — ETL framework in `lib/storage/src/etl.c`. First caller: `chain_restore_rebuild_active_chain` (solves P14.13 class permanently). Second: block-index rewrite. *Attribution: Erigon `db/etl/` (LGPL-3.0).*
- [ ] **P17.5 (A2 share)** HIGH — spectest harness wiring from chain-side: corpus loader, RPC driver, state diff logic. Pair with Agent-3. *Attribution: Erigon `cl/spectest/` (LGPL-3.0).*

### Phase 8 — P18 Perf

- [ ] **P18.1** HIGH — cache-aware `block_map` (SoA + `__builtin_prefetch`). Permanently solves P14.13 pathology class.
- [ ] **P18.2** MED — io_uring backend for `disk_block_io.c`. Runtime detection; fallback to `pread`.
- [ ] **P18.3** LOW — PGO pipeline + `tools/scripts/record_pgo.sh`.

### Phase 9 — P19 Attribution (coordinator pairing)

- [ ] **P19.1** HIGH — verify every P15-P18 commit message cites the attribution row in `ATTRIBUTIONS.md`. Add new concept-borrow entries as they surface.

### Phase 10 — P20 Developer MCP (START IMMEDIATELY — does not depend on P14)

**Can run in parallel with P14 drain.** These tools materially reduce
the context-burn for every subsequent agent session — start
shipping rows before P14.13 lands.

- [ ] **P20.1** HIGH — `zcl_codemap` MCP tool: `{subsystem: {files, symbols, deps}}` for `lib/*/` + `app/*/`.
- [ ] **P20.2** HIGH — `zcl_roadmap` MCP tool: structured JSON of AGENT.md rows (feeds from `.ac.yaml` sidecars P22.2).
- [ ] **P20.5** HIGH — `zcl_impact` MCP tool: transitive reverse-dependency graph (uses P22.3 LSP call-hierarchy).
- [ ] **P20.6** HIGH — `zcl_lint_status` MCP tool: cached `make lint` violations with file:line.
- [ ] **P20.7** MED — `zcl_postmortems` MCP tool: structured index of `docs/postmortems/`.
- [ ] **P20.8** (post-P16) — `zcl_stages` MCP tool: staged-sync pipeline graph + last timings.
- [ ] **P20.9** MED — `zcl_build_info` MCP tool: last-built binary SHA + delta-file warning.

### Phase 11 — P21 Oversized-file deconstruction

**Dependency:** Agent-2's controller splits (P21.1-P21.5) benefit from
P16.4 service-shape landing first. P21.6 pairs naturally with P12.6.1.
P21.9 is standalone. P21.10 is absorbed into P16.2 as stage files.

- [ ] **P21.1** — `sync_controller.c` (3,456) → skinny dispatch + services.
- [ ] **P21.2** — `explorer_controller.c` (3,376) → `explorer_{blocks,tx,addr,stats,chart}_controller.c`.
- [ ] **P21.3** — `blockchain_controller.c` (2,992) → split query vs ops.
- [ ] **P21.4** — `wallet_diagnostic_controller.c` (2,474) → move diagnostics to services.
- [ ] **P21.5** — `api_controller.c` (2,362) → `api_{blocks,tx,wallet,...}_controller.c`.
- [ ] **P21.6** — `msgprocessor.c` (3,404) → per-message handlers out (pair with P12.6.1).
- [ ] **P21.9** — `config/src/boot.c` (2,460) → per-subsystem boot-registry.
- [ ] **P21.10** — `process_block.c` (2,375) absorbed into P16.2 stages.

### Phase 12 — P22 AI-native scaffolding (START IMMEDIATELY)

**Parallel with P14 drain.** This is the shining-example discipline
applied to the agent-onboarding surface itself.

- [ ] **P22.1** HIGH — `AGENTS.md` at repo root (portable alias for CLAUDE.md). 50-line index into per-subsystem `agents.md`.
- [ ] **P22.2** HIGH — `.ac.yaml` sidecars per AGENT.md row at `docs/rows/P<id>.ac.yaml` + CI generator from tables. Feeds P20.2.
- [ ] **P22.3** HIGH — **integrate clangd + LSP-MCP bridge** into zclassic23 binary. `zcl_lsp_{definition,references,hover,call_hierarchy,diagnostics}` tools. Spawns clangd subprocess; generates `compile_commands.json` from Makefile. **Do not hand-roll.** *Attribution: `mcp-language-server` (MIT — see ATTRIBUTIONS.md).*
- [ ] **P22.4 (A2 share)** MED — `docs/spec/{net,validation,storage,wallet,script}.md` cold-memory RAG corpus.
- [ ] **P22.5** HIGH — file-size budget lint (no file over 1,000 lines in `lib/` or `app/`). Grandfather existing oversized files via exemption file tracked by P21.* rows.
- [ ] **P22.6** MED — `AGENTS.md` "fresh-session bootstrap" sequence documented.

### Phase 13 — P23 Structural simplification + generative MCP

**Can start P23.1 / P23.2 / P23.3 / P23.4 / P23.9 immediately** — they
have no upstream dependencies inside this queue. P23.5/P23.6/P23.8
block on upstream P16 / P22 rows as noted.

This phase closes out the shining-example roadmap: once it lands, a
new agent can scaffold a fully-compliant MCP tool, service, stage, or
test from a single MCP call, and the repo's structural surface
(`lib/*/` count, boot sequence, Makefile) is audited + right-sized.

- [ ] **P23.1** HIGH — subsystem consolidation audit (28 `lib/*/` → target ≤20). Write `docs/ARCHITECTURE.md` with proposed merges + per-merge impact. Each actual merge lands later as its own row; P23.1 is audit-and-plan only.
- [ ] **P23.2** HIGH — boot-registry (`config/src/boot.c` → `lib/core/src/boot_registry.c` + per-subsystem `*_boot_init.c`). `ZCL_BOOT_INIT(name, fn, deps)` link-time registration. **Close P21.9 when this lands.**
- [ ] **P23.3** MED — Makefile audit + pattern-rule consolidation. Measure before/after clean-build wall-clock. Target ≤ 2 min on this host.
- [ ] **P23.4** HIGH — `zcl_scaffold_mcp_tool(name, category, description)`. One-call generator for: MCP dispatch entry + controller stub + handler + AGENT.md row + `.ac.yaml` sidecar (P22.2) + RED test skeleton. Gateway row for the rest of P23.
- [ ] **P23.5** MED — `zcl_scaffold_service(name, description)` — P16.4-shape `Cfg` struct + RED test skeleton. Blocks on P16.4.
- [ ] **P23.6** MED — `zcl_scaffold_stage(name, forward_desc, unwind_desc)` — Forward/Unwind/Prune triad + P17.6 contract test skeleton. Blocks on P16.1.
- [ ] **P23.8** MED — `zcl_explain(file, line)` — combines clangd AST (P22.3) with relevant `agents.md` + `docs/spec/` via RAG (P22.4). Blocks on P22.3 + P22.4.
- [ ] **P23.9** MED — `zcl_commit_plan(intent)` — reads `git diff` + AGENT.md rows in-progress, returns structured commit message (row ID, attribution, test evidence). Enforces today's manual discipline.

**Agent-3 owns P23.7** (`zcl_scaffold_test_from_row`) — see AGENT-3.md.

### Phase 14 — P24 Coordinator audit wave (2026-04-21)

Filed after the coordinator burned a session on the binary-drift
incident (stale binary ran a full day behind source). Full descriptions
in [`AGENT.md`](AGENT.md) Priority 24.

- [ ] **P24.11** CRITICAL — second `zcl_syncdiag` crash path (composite of `downloadstats` + `getpeerinfo`). **Queued immediately after P14.6** — blocks coordinator diagnostics.
- [ ] **P24.3** HIGH — lint rule banning raw `malloc()` in `lib/` + `app/` (370 sites today, no CI enforcement).
- [ ] **P24.5** HIGH — `chain.headers >= chain.blocks` invariant assert + RED test. Observed violation live (h=3,081,408 vs blocks=3,081,601).
- [ ] **P24.8** HIGH — `zcl_binary_vs_head` MCP tool: `{binary_mtime, head_sha, commits_behind, drift_seconds}`.
- [ ] **P24.10** HIGH — `make ci-crash` nightly gate (start → send tx → kill-9 → restart → assert balance).
- [ ] **P24.7** HIGH — `make deploy` pre-flight: fail if HEAD commit time > binary mtime. Coord-lane or Agent-2.
- [ ] **P24.1** MED — datadir hygiene sweep `.corrupt.*` → `~/zcl-backups/corrupt-sweep-<ts>/` at boot. Coord-lane.
- [ ] **P24.6** MED — `goto fail;` refactor (34 sites in 3 app-layer files). Extends P15.3 scope.
- [ ] **P24.9** MED — oversized-file backlog (11 additional files >1000 lines beyond P21 list).

---

## Execution discipline (non-negotiable)

1. **RED test first.** No fix commits until a failing test is on main. No exceptions post-P10.1 reset.
2. **One row per commit.** Don't batch unrelated fixes. Row ID goes in the commit message.
3. **Update AGENT.md.** Mark `done <SHA> [test:X.X]` when a row lands.
4. **STOP + ping Rhett** on any serialization change, consensus constant, P2P wire format change, or scope/acceptance-criteria change.
5. **Respect lane boundaries.** `lib/crypto/`, `lib/sapling/`, `lib/keys/`, `lib/validation/src/sigops.c`, `lib/validation/src/check_block.c`, `lib/core/src/random.c` are Agent-3. Do not edit.
6. **Keep `make test` green.** Push every row; never amend pushed commits.

Total rows in this queue: **~58** across 13 phases. Work in order;
parallel-safe rows within a phase can land concurrently.
