# B5 — chain-tip readers audit & single-accessor routing

**Goal (B5):** make the future B7 flip — log_head-derived tip becomes
definitional, `chain_active` demoted to a derived index — a **one-function
change**. We do that by ensuring every *tip read* in the app layer flows
through the single tip accessor, not through ad-hoc index lookups or raw
struct fields.

**This step does NOT flip authority.** It only routes readers so the orchestrator
can change two accessor bodies at B7 without chasing ~20 call sites.

## TL;DR finding

The accessor chokepoint is **already in place at the function level.** A full
`git grep` of `lib/` + `app/` (non-test) for `chain_active.height`,
`chain_active.chain[]`, `chain_active.capacity` returns **zero** direct
field reads — every reader already goes through one of three public
accessors declared in `lib/validation/include/validation/chainstate.h`:

| accessor | returns | role at B7 |
|----------|---------|------------|
| `active_chain_tip(c)` | tip `block_index*` | **FLIP SEAM** — body becomes log_head-derived |
| `active_chain_height(c)` | tip height (int) | **FLIP SEAM** — body becomes log_head-derived |
| `active_chain_at(c, h)` | block at height `h` | stays a *derived index* over `chain_active` |

So the only B5 risk is a **tip read disguised as an index read**:
`active_chain_at(c, active_chain_height(c))`. Today that is bit-for-bit
identical to `active_chain_tip(c)` (both return `c->chain[c->height]` — see
`lib/validation/src/chainstate.c:233` and `:239`). But after B7, if
`active_chain_at` keeps indexing `chain_active` while `active_chain_tip`
derives from log_head, the two could diverge. Those disguised tip reads are
the sites that must be re-pointed at `active_chain_tip` so they pick up the
flip. There are exactly **two** in safe (read-only) app-layer code.

## Redirects performed (2 sites)

Both are behavior-preserving today (`active_chain_at(c, c->height)` ≡
`active_chain_tip(c)`) and become flip-correct at B7.

| file:line | what it read | redirect |
|-----------|--------------|----------|
| `app/controllers/src/chain_inspect_controller.c:418-420` | tip block via `active_chain_at(c, active_chain_height(c))` to read `hashFinalSaplingRoot` (RPC `getsaplinginfo`) | now `active_chain_tip(c)`; the `int tip` height var is kept for the `tip_height` JSON field |
| `app/controllers/src/rpc_chainstate_guard.c:18` | "is the tip slot populated?" via `active_chain_at(c, tip_height) != NULL` (the RPC readiness guard) | now `active_chain_tip(c) != NULL`; the early `tip_height < 0 → return true` guard is unchanged, so null-handling is identical |

Verification that the redirects are bit-for-bit identical TODAY:
- `active_chain_at(c, h)` returns NULL iff `!c || !c->chain || h<0 || h>c->height`; for `h == c->height` (and `c->height >= 0`) it returns `c->chain[c->height]`.
- `active_chain_tip(c)` returns NULL iff `!c || !c->chain || c->height<0`; otherwise `c->chain[c->height]`.
- In `chain_inspect`, `tip = active_chain_height(c)` so the index arg *is* `c->height` → same pointer.
- In `rpc_chainstate_guard`, the `tip_height < 0` case already returned `true` before the index call, so the remaining call only ever ran with `tip_height == c->height >= 0` → `active_chain_tip(c)` returns the same pointer, same NULL-ness.

## Full reader inventory (app-layer / lib/net / lib/metrics, non-test)

Classification key: **TIP** = reads the tip (height/hash/block) → tracks the
B7 flip automatically because it already uses `active_chain_tip` /
`active_chain_height`. **INDEX** = reads an interior height via
`active_chain_at(c, h)` → intentionally a derived-index read, must NOT change.
**WRITE** = mutates `chain_active` (consensus/repository) → out of scope, B7
surface. **redirected** column: yes / n-a (already through accessor or genuine
index) / no (left, with reason).

### Conditions (`app/conditions/`)
| file:line | class | reads | redirected | note |
|-----------|-------|-------|------------|------|
| block_failed_mask_at_tip.c:59 | TIP | `active_chain_height` | n-a | already accessor |
| cutover_canary_complete.c:25,32 | TIP | `active_chain_height` | n-a | already accessor |
| cutover_no_forward_progress.c:57,104 | TIP | `active_chain_height` | n-a | already accessor |
| have_data_unreadable.c:44,90 | TIP | `active_chain_height` | n-a | already accessor |
| local_header_refill_needed.c:37 | TIP | `active_chain_tip` | n-a | already accessor |
| peer_floor_violated.c:70,165 | TIP | `active_chain_height` | n-a | already accessor |
| snapshot_offer_ready.c:48 | TIP | `active_chain_height` | n-a | already accessor |
| sync_state_stuck.c:43 | TIP | `active_chain_height` | n-a | already accessor |
| sync_violation_lag.c:42,107 | TIP | `active_chain_height` | n-a | already accessor |
| tip_wedged_resnapshot.c:126,216 | TIP | `active_chain_height` | n-a | already accessor |

### Controllers (`app/controllers/`)
| file:line | class | reads | redirected | note |
|-----------|-------|-------|------------|------|
| api_controller_node.c:381 | TIP | `active_chain_tip` | n-a | already accessor |
| blockchain_controller.c:106,181 | TIP | `active_chain_height` | n-a | already accessor |
| blockchain_controller.c:114,189 | INDEX | `active_chain_at(c,h)` loop | n-a | genuine index |
| blockchain_controller_admin.c:50,52,95,350 | TIP | `active_chain_height` | n-a | already accessor |
| blockchain_controller_admin.c:138 | INDEX | `active_chain_at(c,h)` | n-a | genuine index |
| blockchain_controller_blocks.c:43 | TIP | `active_chain_height` | n-a | already accessor |
| blockchain_controller_blocks.c:57,84,118 | TIP | `active_chain_tip` | n-a | already accessor |
| blockchain_controller_blocks.c:141 | INDEX | `active_chain_at(c,height)` | n-a | genuine index |
| blockchain_controller_chain.c:66,336,346 | TIP | `active_chain_tip` | n-a | already accessor |
| blockchain_controller_chain.c:345,412,477,521,619 | TIP | `active_chain_height` | n-a | already accessor |
| blockchain_controller_mmr.c:147 | TIP | `active_chain_height` | n-a | already accessor |
| blockchain_controller_mmr.c:188 | WRITE-ADJACENT | passes `&chain_active` to `sapling_tree_rebuild` | no | rebuild walks chain by index; not a tip read |
| blockchain_controller_mmr.c:214 | TIP | `active_chain_tip` | n-a | already accessor |
| chain_inspect_controller.c:96,182,418,456,545 | TIP | `active_chain_height` | n-a | already accessor |
| **chain_inspect_controller.c:418-420** | **TIP (disguised)** | `active_chain_at(c, active_chain_height(c))` | **YES** | → `active_chain_tip(c)` |
| chain_inspect_controller.c:111,203,397,473,564 | INDEX | `active_chain_at(c,h)` | n-a | genuine index |
| diagnostics_registry.c:157,237 | INDEX | `active_chain_at(c,h)` | n-a | genuine index |
| explorer_controller.c:241,369 | TIP | `active_chain_height` | n-a | already accessor |
| explorer_controller_block.c:173,195 | TIP | `active_chain_height` | n-a | already accessor |
| explorer_controller_block.c:175 | INDEX | `active_chain_at(c,h)` | n-a | genuine index |
| explorer_controller_dashboard.c:167,250 | TIP | `active_chain_height` | n-a | already accessor |
| explorer_controller_dashboard.c:168 | TIP | `active_chain_tip` | n-a | already accessor |
| explorer_controller_dashboard.c:199 | INDEX | `active_chain_at(c,h)` | n-a | genuine index |
| explorer_controller_tx.c:274 | TIP | `active_chain_height` | n-a | already accessor |
| explorer_controller_tx.c:242 | INDEX | `active_chain_at(c,block_height)` | n-a | genuine index |
| health_controller.c:149,563 | TIP | `active_chain_height` | n-a | already accessor |
| health_controller.c:153 | TIP | `active_chain_tip` | n-a | already accessor |
| hodl_controller.c:83,229 | TIP | `active_chain_tip` | n-a | already accessor |
| hodl_controller.c:92,237 | TIP | `active_chain_height` | n-a | already accessor |
| mining_controller.c:65,125,224,324 | TIP | `active_chain_tip` | n-a | already accessor |
| misc_controller.c:66 | TIP | `active_chain_tip` | n-a | already accessor |
| repair_controller.c:171,220 | TIP | `active_chain_tip` | n-a | already accessor |
| repair_controller.c:186,221,248 | TIP | `active_chain_height` | n-a | already accessor |
| repair_controller_utxo.c:270 | TIP | `active_chain_height` | n-a | already accessor |
| **rpc_chainstate_guard.c:18** | **TIP (disguised)** | `active_chain_at(c, tip_height) != NULL` | **YES** | → `active_chain_tip(c) != NULL` |
| rpc_chainstate_guard.c:14,29 | TIP | `active_chain_height` | n-a | already accessor |
| transaction_controller.c:244,303 | TIP | `active_chain_height` | n-a | already accessor |
| transaction_controller.c:114 | INDEX | `active_chain_at(c,entry.height)` | n-a | genuine index |
| transaction_controller_sign.c:263 | TIP | `active_chain_height` | n-a | already accessor |
| wallet_controller.c:210,629 | TIP | `active_chain_height` | n-a | already accessor |
| wallet_controller.c:640 | WRITE-ADJACENT | passes `&chain_active` to `wallet_rescan` | no | rescan walks by index; not a tip read |
| wallet_diagnostic_audit.c:91 | TIP | `active_chain_height` | n-a | already accessor |
| wallet_diagnostic_repair.c:126,458 | TIP | `active_chain_height` | n-a | already accessor |
| wallet_diagnostic_repair.c:131 | WRITE-ADJACENT | passes `&chain_active` to repair | no | walks by index |
| wallet_helpers.c:189 | TIP | `active_chain_height` | n-a | already accessor |
| wallet_rescan_controller.c:99,358 | TIP | `active_chain_height` | n-a | already accessor |
| wallet_rescan_controller.c:106,364 | WRITE-ADJACENT | passes `&chain_active` to `wallet_rescan` | no | walks by index |
| wallet_rescan_controller_witness.c:53 | TIP | `active_chain_height` | n-a | already accessor |
| wallet_rescan_controller_witness.c:82 | INDEX | `active_chain_at(c,h)` loop | n-a | genuine index |
| wallet_rescan_controller_witness.c:188 | INDEX | `active_chain_at(c, safe_tip)` | **no** | `safe_tip = zcl_immutable_height(chain_tip)` — an *interior immutable height below the tip*, NOT the tip. Redirecting would change the returned block → bug. Left as index. |
| wallet_shielded_controller.c:213,435 | TIP | `active_chain_height` | n-a | already accessor |
| wallet_shielded_keys.c:138 | WRITE-ADJACENT | passes `&chain_active` to `wallet_rescan` | no | walks by index |
| wallet_shielded_send.c:146 | TIP | `active_chain_height` | n-a | already accessor |
| sync_controller_catchup.c:477 | TIP | `active_chain_tip(chain)` | n-a | already accessor |
| sync_controller_catchup.c:310,347,398,614 | INDEX | `active_chain_at(chain,h)` | n-a | genuine index |

### Jobs (`app/jobs/`) — staged reducer; INDEX reads by cursor height
| file:line | class | reads | redirected | note |
|-----------|-------|-------|------------|------|
| body_fetch_stage.c:198 | INDEX | `active_chain_at(c,next_h)` | n-a | cursor index |
| body_persist_stage.c:314 | INDEX | `active_chain_at(c,next_h)` | n-a | cursor index |
| header_admit_stage.c:95,150,521 | INDEX | `active_chain_at(c,h)` | n-a | cursor index |
| header_admit_stage.c:448 | TIP | `active_chain_height` | n-a | already accessor |
| proof_validate_stage.c:468 | INDEX | `active_chain_at(c,next_h)` | n-a | cursor index |
| script_validate_stage.c:379 | INDEX | `active_chain_at(c,next_h)` | n-a | cursor index |
| tip_finalize_stage.c:299,401,402 | INDEX | `active_chain_at(c,row_height+1 / next_h)` | n-a | this stage *writes* log_head; it intentionally reads chain_active by index to record the per-height tip hash (the source the B7 flip will read from). DO NOT touch in B5. |
| utxo_apply_stage.c:487 | INDEX | `active_chain_at(c,next_h)` | n-a | cursor index |
| validate_headers_stage.c:302,402 | INDEX | `active_chain_at(c,h)` | n-a | cursor index |

### Services (`app/services/`)
| file:line | class | reads | redirected | note |
|-----------|-------|-------|------------|------|
| bg_hash_verification_service.c:70,162 | TIP | `active_chain_height` | n-a | already accessor |
| bg_hash_verification_service.c:106 | INDEX | `active_chain_at(c,h)` | n-a | genuine index |
| bg_validation_service.c:316,339,503 | TIP | `active_chain_height` | n-a | already accessor |
| bg_validation_service.c:343,527,528 | INDEX | `active_chain_at(c,h/0/1)` | n-a | genuine index |
| block_index_integrity.c:454,528 | TIP | `active_chain_height` | n-a | already accessor |
| block_index_loader.c:589 | TIP | `active_chain_height` | n-a | already accessor |
| block_pruning_service.c:120,373 | TIP | `active_chain_height` | n-a | already accessor |
| block_pruning_service.c:134,186,208 | INDEX | `active_chain_at(c,h)` | n-a | genuine index |
| block_sync_service.c:210 | TIP | `active_chain_height` | n-a | already accessor |
| block_sync_service.c:247,393 | TIP | `active_chain_tip` | n-a | already accessor |
| chain_activation_controller.c:340 | TIP | `active_chain_height` | n-a | already accessor |
| chain_activation_controller.c:430 | TIP | `active_chain_tip` | n-a | already accessor |
| chain_advance_coordinator.c:1137 | TIP | `active_chain_height` | n-a | already accessor |
| chain_evidence_controller.c:123-124,492-493,596-597 | TIP+WRITE | `active_chain_tip/height` via `csr->chain_active` ptr | no | this is the chain_state_repository authority path (promote/rollback). WRITE-side; B7 surface — left for orchestrator. |
| chain_restore_disk_repair.c:86,414 | WRITE | `&chain_active` repair | no | restore/repair path; rebuilds the index |
| chain_restore_executor.c:37 | TIP | `active_chain_height` | n-a | already accessor |
| chain_restore_integrity.c:27,78,113 | TIP | `active_chain_tip/height` | n-a | already accessor |
| chain_restore_integrity.c:82,98 | INDEX | `active_chain_at(c,h / h-1)` | n-a | genuine index |
| **chain_restore_integrity.c:116** | **PARITY-INVARIANT** | `active_chain_at(c, tip_height) == active_chain_tip(c)` | **no** | DELIBERATE cross-check that the index slot at tip_height equals the accessor tip. Redirecting either side makes it tautological and destroys its diagnostic value. **Keep both sides distinct.** After B7 this becomes a live index-vs-log_head parity assertion — exactly what we want it to keep checking. |
| chain_restore_repair.c:* | WRITE | rebuilds `chain_active` slots | no | restore/repair path |
| chain_state_repository.c:* | WRITE | the authority repo: `active_chain_set_tip`, reads `csr->chain_active` | no | **B7 flip surface** (CONSENSUS/authority) — left for orchestrator |
| chain_state_validator.c:41 | TIP | `active_chain_tip` | n-a | already accessor |
| chain_tip.c:114,117,131 | WRITE | `active_chain_set_tip` | no | tip mutation; B7 surface |
| chain_tip_watchdog.c:207 | TIP | `active_chain_height` | n-a | already accessor |
| gap_fill_service.c:82 | TIP | `active_chain_height` | n-a | already accessor |
| header_probe.c:140,254 | TIP | `active_chain_height` | n-a | already accessor |
| legacy_bootstrap_importer.c:1107,1216,1332,1607 | TIP | `active_chain_height` | n-a | already accessor (bootstrap import) |
| legacy_bootstrap_importer.c:1339 | WRITE-ADJACENT | passes `&chain_active` to `wallet_rescan` | no | walks by index |
| legacy_mirror_sync_service.c:322 | INDEX | `active_chain_at(c,height)` | n-a | genuine index |
| legacy_mirror_sync_service.c:324 | TIP | `active_chain_tip` (fallback) | n-a | already accessor |
| legacy_mirror_sync_service.c:410 | TIP | `active_chain_height` | n-a | already accessor |
| node_health_service.c:214 | TIP | `active_chain_tip` | n-a | already accessor |
| quorum_oracle_service.c:166 | INDEX | `active_chain_at(c,height)` | n-a | genuine index (oracle by height) |
| rolling_anchor_service.c:357 | INDEX | `active_chain_at(c,h)` | n-a | genuine index |
| rolling_anchor_service.c:418 | TIP | `active_chain_height` | n-a | already accessor |
| snapshot_apply.c:218 | TIP | `active_chain_height` | n-a | already accessor |
| snapshot_verify.c:346 | INDEX | `active_chain_at(c,h)` | n-a | genuine index |
| utxo_recovery_service.c:58,431,804 | TIP | `active_chain_height` | n-a | already accessor |
| utxo_recovery_service.c:957,984 | TIP | `active_chain_tip` | n-a | already accessor |
| zclassicd_oracle_service.c:129 | INDEX | `active_chain_at(c,height)` | n-a | genuine index |
| zclassicd_oracle_service.c:142 | TIP | `active_chain_height` | n-a | already accessor |

### Supervisors (`app/supervisors/`)
| file:line | class | reads | redirected | note |
|-----------|-------|-------|------------|------|
| chain_supervisor.c:45,81 | TIP | `active_chain_height` | n-a | already accessor |

### lib/net (read-only tip reads for P2P)
| file:line | class | reads | redirected | note |
|-----------|-------|-------|------------|------|
| msg_blocks.c:101,382,388,462 | TIP/INDEX | `&chain_active` ptr, `active_chain_tip`, `msg_blocks_should_mark_seen` | n-a | tip reads go through `active_chain_tip(chain)` at :123; :120 reads `active_chain_at(c,0)` (genesis, index) |
| msg_headers.c:71,332,423,746,899 | TIP | `active_chain_tip` | n-a | already accessor |
| msg_headers.c:185 | INDEX | `active_chain_at(c,0)` | n-a | genesis index |
| msg_headers.c:222,330,472,561,738,744,747,807,817,864 | TIP | `active_chain_height` / passes `&chain_active` to locator builders | n-a | locator/height reads via accessor |
| msg_tx.c:104,200,201,225 | TIP | `active_chain_height` / `active_chain_tip` | n-a | already accessor |
| msg_version.c:215 | TIP | `active_chain_height` | n-a | already accessor |
| msgprocessor.c:761,1114,1115,1216,1217 | TIP | `active_chain_height` / `active_chain_tip` | n-a | already accessor |
| msgprocessor_snapshot.c:708,1466 | TIP | `active_chain_height` | n-a | already accessor |
| msgprocessor_snapshot.c:714,744,1053,1092,1172 | INDEX/builder | passes `&chain_active` to manifest builders | n-a | index walks |
| fast_sync.c:1486,1524 | INDEX | `active_chain_at(c,h)` | n-a | genuine index |

### lib/metrics, lib/mining
| file:line | class | reads | redirected | note |
|-----------|-------|-------|------------|------|
| metrics.c:217,338 | TIP | `active_chain_tip` | n-a | already accessor |
| mining/gen.c:122,146 | TIP | `active_chain_tip` | n-a | already accessor (block template prev) |
| mining/miner.c:69 | TIP | `active_chain_tip` | n-a | already accessor |

### app/models
| file:line | class | reads | redirected | note |
|-----------|-------|-------|------------|------|
| mmb_leaf_store.c:151-156,177 | INDEX | casts ptr to `const struct active_chain*`, `active_chain_at(c,h)` | n-a | builds MMB leaves by index |

## B5 flip plan (what changes at B7)

At B7 the orchestrator changes **exactly two accessor bodies** in
`lib/validation/src/chainstate.c`:

```c
struct block_index *active_chain_tip(const struct active_chain *c)   // :233
int                 active_chain_height(const struct active_chain *c) // :303
```

Their bodies switch from reading `c->chain[c->height]` / `c->height` to
deriving the tip from the append-only reducer log (`tip_finalize_log`,
written by `app/jobs/src/tip_finalize_stage.c`):

```
derived_tip_height = (max finalized H in tip_finalize_log) + 1
derived_tip_hash   = tip_finalize_log[max finalized H].tip_hash
```

`active_chain_at(c, h)` for `h < tip` stays an index over `chain_active`
(the "derived index" `chain_active` is demoted to).

Because every TIP reader in the inventory above either already calls these
two accessors, or was redirected to `active_chain_tip` in this step, the flip
is a one-function-body change with no caller churn.

### Invariant the flip must hold (the gate already in the suite)

`test_cutover_tip_parity` (`lib/test/src/test_cutover_tip_parity.c`,
registered `cutover_tip_parity`) is the safety gate. It proves, per height
and across a reorg, that:

- **P1** log-derived tip *hash* == `chain_active` best-block hash at that height
- **P2** log-derived tip *height* == `active_chain_height(chain_active)` (no lag/overshoot)
- **P3** at full convergence the finalize cursor lag is exactly **0**
  (= the cutover preflight criterion in
  `app/controllers/src/cutover_controller.c`: `required_cursor = tip + 1`,
  both `header_caught_up`/`validate_caught_up` require `cursor_lag == 0`)

Concretely the flip is safe to enable iff, for every live height,
`active_chain_at(chain_active, h)->phashBlock` (the demoted index) equals the
`tip_finalize_log` hash for the tip at `h`. The persistent in-process version
of this equality is the **PARITY-INVARIANT** already wired at
`app/services/src/chain_restore_integrity.c:116` — which is why we KEEP its
two sides distinct (index vs accessor) instead of redirecting it.

## DO NOT redirect — consensus-internal / B7 flip surface (orchestrator handles)

These mutate `chain_active` or are the authority path. Left untouched in B5:

- `lib/validation/src/connect_tip.c` (:112,331,363,376)
- `lib/validation/src/disconnect_tip.c` (:55) — **also another agent's live edit**
- `lib/validation/src/activate_best_chain.c` (all sites)
- `lib/validation/src/accept_block.c` (:115), `accept_block_header.c` (:240,242)
- `lib/validation/src/process_block_core.c`, `process_block_revalidate.c`, `process_block_self_heal.c`
- `lib/validation/src/chainstate.c` (:315,327 init/free; :233/:303 = the two flip-seam bodies)
- `app/services/src/chain_state_repository.c` (the authority repo: `active_chain_set_tip`, rollback/commit) — **B7 flip surface**
- `app/services/src/chain_evidence_controller.c` (promote/rollback authority)
- `app/services/src/chain_tip.c` (`active_chain_set_tip`)
- `app/services/src/chain_restore_repair.c`, `chain_restore_disk_repair.c` (rebuild the index)
- `lib/coins/*` — **another agent's live edit; off-limits**

## Verification (this branch)

- `make -j$(nproc)` → builds `zclassic23` + `test_zcl` clean
- `make lint` → all checks passed (29 gates)
- `./test_parallel --only=cutover_tip_parity` → PASSED
- `./test_parallel --only=chain_state_repo` → PASSED
- `./test_parallel --only=tip_finalize_stage` → PASSED
- `./test_parallel` (full) → **245/245 groups passed**
