# HANDOFF — read this first

**Restart command:** type **`continue zclassic23 development`** (3 words). That's all you need.

State at handoff: `origin/main`, working tree clean, one branch, build + lint + 256-group test suite all green.

---

## The one thing that's left: cutover stabilization + cleanup

The node is in late-stage migration from a **legacy** consensus pipeline (`connect_tip`, whose
tip is `chain_active`) to an **event-sourced** one (the Wave-S reducer, whose tip
derives from an append-only `log_head`). All 8 reducer stages already run in **shadow**
and produce the same answers as legacy on every block. **B5 and B7 are done; remaining work
is live preflight verification plus B8 cleanup.** Everything else is built.

```
B5  make log_head the tip ...... done (2-FUNCTION accessor flip: active_chain_tip + active_chain_height)
B7  flip authoritative with auto-revert guard ...... done (cutover with `cutovermode` + `cutover_no_forward_progress`)
B8  live preflight verification + delete legacy (~3,900 LOC) .. exact checklist already written
```

- B5 audit: `docs/work/b5-chain-active-readers.md`
- B8 inventory: `docs/work/b8-deletion-inventory.md`
- Architecture: `docs/FRAMEWORK.md` · Checklist: `docs/REFACTOR_STATUS.md`

## Do NOT
1. **Do not flip casually.** It touches the live chain. Green tests ≠ a healthy live node
   (a past cutover shipped green and wedged the chain). Flip on a **clean datadir**, with
   the canary armed, watching live tip-advance, ready to revert.
2. **Never stop `zclassicd`** (`zclassicd-rhett`). Both nodes run under systemd `--user`
   linger 24/7. Manage via `systemctl`, never manual runs. `-cold-import` is **forbidden**
   (corrupts state). For a torn local chain use the fast rebuild (`rebuild_recent`).
3. **Never weaken a lint gate** to make progress. Baselines (E1=7, E2=16) only go down.

## Two gotchas that will bite you
1. **Stale test binary** — `make` doesn't relink `test_parallel`. Always:
   `touch lib/test/src/test_parallel.c && make test_parallel` before trusting results.
2. **Worktree leaks** — agents using absolute paths can write into main's working tree.
   `git status` main before every commit/cherry-pick; the real work is in the branch,
   so `git restore` any strays.

## Confirm flip-readiness (all observable)
- `zcl_state subsystem=cutover` — stage modes, authoritative_active, conservation, canary — one call.
- `cutoverpreflight` RPC — the `ready` boolean; refuses the flip with a typed blocker if unsafe.
- Proof tests: `test_cutover_tip_parity`, `test_shadow_conservation`, `test_cutover_flip_dryrun`,
  `test_cutover_postflip_reorg`, `test_reducer_stage_fuzz`, `test_projection_replay_invariant`, `test_replay_verify`.

## First 5 minutes
```
make -j$(nproc) && make lint
touch lib/test/src/test_parallel.c && make test_parallel && ./test_parallel   # expect 256/256
zcl_status                          # live node state
zcl_state subsystem=cutover         # flip readiness
```

## MCP
`claude mcp add zcl23 -- zclassic23 -mcp`. ~105 tools. Start with `zcl_status`.
Primitives: `zcl_state`, `zcl_sql` (SELECT-only), `zcl_node_log`. Escape hatch: `zcl_rpc`.
