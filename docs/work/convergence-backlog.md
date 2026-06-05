# Convergence backlog — the zclassic23 way

Source: tree-wide read-only audit (9 agents, 2026-06-05). Ranked by
value-over-risk. Each item is a SAFE convergence action (DRY / DOC / API /
SHAPE). High-risk consensus-sensitive items are quarantined to §12 and need
repro-on-copy before any edit.

**Discipline:** mutating work runs as edit-only workflows on DISJOINT file
sets, then a single build + `make lint` (35 gates) + `test_parallel` (0/N) +
boot-smoke gate, then commit per logical group. Never grow a baseline, never
weaken a gate, never touch `config/src/boot.c`.

## Wave status

| Wave | Items | State |
|------|-------|-------|
| boot-index decompose | `config/src/boot_index.c` → shape-clean units | in flight (`wjb4k7nac`) |
| Convergence Wave 1 | #1+#11, #2, #3, #6, #7+#8, #9 | in flight (`w37tgxfpv`) |
| Convergence Wave 2 | #4, #5, #10 | queued |
| Consensus-sensitive | §12 (×4) | deferred — repro-on-copy each |

## Items

1. **`coins_alloc` latent NULL-deref + lying return** *(API / real bug, low risk)* —
   `lib/coins/src/coins.c`. On OOM it logs, falls through, derefs NULL `vout`,
   and `return true` masks failure from 3 callers
   (`coins.c:65`, `coins_view_sqlite.c:784`, `utxo_projection.c:674`) whose
   `if (!coins_alloc(...))` guards are dead code. Fix: `num_vout=0; return false;`
   before the null-init loop. **→ Wave 1.**
2. **Document `check_block.h` gate booleans** *(DOC, low)* — four must-never-fork
   entry points expose undocumented "disable a safety check" bools. Lift the
   fast-sync prose from `check_block.c`. **→ Wave 1.**
3. **Unify 3 duplicate `bytes32_nonzero` onto `zcl_chainwork_is_zero`** *(DRY, low)* —
   `chain_evidence_authority_service.c:99`, `chain_evidence_snapshot.c:17`,
   `snapshot_manifest.c:19`. Leave `snapshot_offer.c` (own NULL-guard contract). **→ Wave 1.**
4. **Fix fabricated `nonce` / `confirmations` in block-header serializer** *(API, low)* —
   `blockchain_controller_blocks.c:170/162` emits `nonce:0`, `confirmations:1`
   for every block; consumer `api_controller_lookup.c:102` reads nonce as a
   string. Emit real `uint256_get_hex(nNonce)` string + `1+tip-height`. **→ Wave 2.**
5. **Unify `json_extract_int/real` controller wrappers** *(DRY, low)* — two
   byte-identical wrapper pairs re-adapting the shared `zcl_json_extract_*`,
   ~44 call sites, un-prefixed names leaking via internal headers. Add
   `zcl_json_int/real` to `views/format_helpers.h`; fold in the duplicate
   `get_difficulty`/`explorer_get_difficulty`. **→ Wave 2.**
6. **Document 13 of 15 condition headers** *(DOC, low)* — symptom→remedy→witness
   →cadence, mirroring `tip_fork_stale.h`. The condition registry is the live
   self-heal surface. **→ Wave 1.**
7. **One `stage_block_reader_fn` typedef for 4 reducer stages** *(DRY, low)* —
   four identical typedefs + setter bodies onto one `stage_default_block_reader`;
   alias to keep public names. **→ Wave 1.**
8. **Delete 3rd `cursor_persisted` copy in reorg path** *(DRY, low)* —
   `utxo_apply_delta_reorg.c:73` → `stage_cursor_persisted`. Call site is before
   `BEGIN IMMEDIATE` (no double-lock, verified). **→ Wave 1.**
9. **`condition_reset_state` primitive + `operator_needed_emitted` leak** *(DRY +
   test-isolation, low)* — only 4 of 12 `_test_reset()` clear the field; 8 leak
   operator-needed state between tests. Centralize in `lib/framework`. **→ Wave 1.**
10. **Table-drive the staged-sync supervisor** *(SHAPE, low but load-bearing)* —
    8 cloned per-stage blocks (~390 LOC) differing only by symbol → desc table +
    one generic register. Live liveness wiring; do after the mechanicals. **→ Wave 2.**
11. **Document `coins.h` lifecycle invariants** *(DOC, low)* — "empty record means
    OOM, not pruned." Rides with #1. **→ Wave 1.**

## §12 — Consensus-sensitive (deferred, repro-on-copy each, do NOT batch)

- **`disconnect_block` unused `state` param** (`connect_block.c`) — route reorg
  failures through `validation_state_*`/`REJECT_FATAL` for symmetry, or drop the
  param. Reorg path.
- **`connect_block` 16× duplicated cleanup** — consolidate the `free(checks)/…/
  block_undo_free` sites into one `goto cleanup:`. Reject reasons + DoS scores
  must stay byte-identical.
- **CCoins avail-mask decode — DIVERGED, do NOT merge** (`coins_db.c:104-131`,
  `chainstate_legacy_reader.c:109-136`) — RESOLVED 2026-06-05: read both. They
  share the CCoins mask format but are deliberately distinct: `coins_db.c` (live
  node.db read) is hardened — fixed `avail_stack[4096]` cap + `nMaskCode > 10000`
  reject — to bound untrusted-row memory; `chainstate_legacy_reader.c` (trusted
  external-chainstate import) grows its buffer unbounded so it never truncates a
  legitimately large record. A naive "extract one helper" would either strip the
  live path's DoS guard (a safety gate — forbidden) or impose truncation on the
  import path. Closed as NOT-a-dedup; both sites now carry a matching
  do-not-merge note. No code change beyond the warnings.
- **`legacy_mirror_sync_request_catchup` dual surface** — invert so the worker
  returns `zcl_result` directly (Law 2); keep `bool` as a thin adapter.
- **`update_coins` silently accepts an empty coins record on OOM**
  (`lib/validation/src/update_coins.c:98`) — `coins_from_transaction` is `void`
  and leaves `num_vout=0` on OOM, but `num_vout=0` is ALSO legitimate for an
  all-OP_RETURN/unspendable tx, so the caller cannot distinguish OOM from a
  valid empty record. The real fix is to give `coins_from_transaction` a status
  return (and propagate it through `update_coins` → `connect_block`); a naive
  `if (num_vout==0) fail` guard would false-reject valid txs. Consensus connect
  path — **repro-on-copy**, scope the `coins_from_transaction` signature change
  carefully. (Surfaced by the Wave-1 `coins_alloc` review; the live NULL-deref
  sibling at `coins_db.c:133` was fixed in Wave 1.)

## Round 4 (2026-06-05) — under-swept subsystems audited + fixed

Read-only audit (7 agents: tools/mcp, app/views, lib/wallet, lib/rpc,
app/models, domain, lib/util) → 22 safe findings, executed as a 6-group
edit→adversarial-review workflow on DISJOINT files. Union gate green
(build 0err / lint 35 / test_parallel 0/371). Landed `cf7bbf05f..525ca1ccc`:

- **Build regression fixed** — `crypto/blake2b.h` had `update*/final` whose `*/`
  closed the doc comment early, spilling text into code; clean build was broken
  (a cached build masked it). (`cf7bbf05f`.)
- **3 MCP NULL-deref crashes** — `onion_health`/`listaddresses`/`profile`
  handlers set `res->error` on malloc-fail but fell through to `snprintf` on the
  NULL body; added the early `return 0`. (`b4b77819d`.)
- **contact.c before_save veto-bypass** — logged the veto then persisted anyway;
  now returns false like every other model. (`b4b77819d`.)
- **views DRY** — `format_zcl_price` + `zcl_format_zcl_short` folded onto one
  exact-integer `zcl_format_zcl_trimmed(…,min_decimals)`; output proven identical
  over a 200M-sample sweep. (`7aabb943c`.)
- **~14 public headers documented** (rpc/util/wallet/models/views). (`525ca1ccc`.)

Adversarial review EARNED its keep again: the audit's "lying `return true` in
hd_keychain/bip44" finding was WRONG — `LOG_FAIL` already expands to
`return false`, so those were success/unreachable paths. Review declined the
`.c` edits; only the accurate doc comments landed. `domain/` returned 0 findings
(already clean) — the safe axis on these subsystems is now harvested.

### New deferred items (surfaced in round 4, not executed)

- **`wallet_generate_hd_key` HD-counter race** (`lib/wallet/src/wallet.c:217-246`,
  *medium risk*) — the internal/external counter is read and used for BIP44
  derivation OUTSIDE the mutex and only incremented inside it; two concurrent
  callers can derive the same key index → address collision. Real fix: read the
  counter (and ideally derive) under the lock. Wallet-threading change — scope and
  test deliberately (not a one-liner; verify no caller holds the lock already).
- **model hook-init DRY** (3 patterns across ~7 models: manual `_init_hooks` +
  static bool, manual `_callbacks_ready`, and the `DEFINE_MODEL_BEFORE_SAVE_READY`
  macro) — consolidate onto one macro. SHAPE, low value, touches many files +
  risks the model-callback lint scaffold; do as its own wave if at all.
- **`mcp minconf` param-spec triplicate** (`wallet_controller.c:283/321/328`) —
  identical INT spec defined 3×; low value, each spec is arguably tied to its
  route entry. Leave unless a wider MCP param-spec table lands.

## Round 5 (2026-06-05) — app/controllers + service glue audited + fixed

Read-only audit (4 agents: explorer/web, wallet-store, ops-net, service glue) →
15 findings (≈5 self-rejected as already-correct/intentional). Executed the real
ones as a 3-group edit→review workflow on DISJOINT files. Union gate green
(build 0err / lint 35 / test_parallel 0/371). Landed `678bf081c..030f16a0e`:

- **Explorer NULL-deref / empty-body fixes** (`678bf081c`) — `explorer_controller_tx.c`
  in_rows/out_rows were malloc'd then iterated with no NULL check (crash on OOM);
  `serve_tx_rpc`/`serve_block_rpc` returned empty bodies on bad input. Added
  guards returning the existing not-found/invalid views (free-before-return,
  no leak). Read-only display path, non-consensus.
- **Controller glue fixes** (`030f16a0e`) — NULL-safe `sqlite3_errmsg` in
  store_mark_order_paid; LOG_WARN on uninit name-DB (RPC reply unchanged);
  LOG_WARN on seed-product save failure; folded the duplicated chain_params
  base58-prefix fetch in wallet_helpers onto one file-local helper.

Adversarial review again EARNED its keep: the audit's `explorer_controller.c`
`socket()`-failure finding was WRONG — `LOG_ERR` already `return -1`s, so there
is no fall-through; the edit was correctly declined.

### New deferred items (surfaced in round 5, not executed)

- **`chain_evidence` dump JSON duplicated** (`event_controller.c:27-74` ≈
  `diagnostics_registry.c:329-370`, *observability-sensitive*) — both assemble the
  same chain-evidence snapshot into JSON; the registry version is the superset.
  Extract one canonical `chain_evidence_controller_dump_state_json(…, bool full)`.
  Deferred: it is the live diagnostics surface and a test may assert the emitted
  shape — verify the JSON is byte-identical before merging.
- **`rpc_call` ≡ `api_rpc_call`** (`explorer_controller.c:254` / `api_controller.c:130`)
  — identical HTTP+RPC+base64 client, differ only in timeout(5s/10s)/log-prefix.
  Merge onto one fn with a timeout/context param. Medium value, touches raw socket
  code — do as its own focused change with a request/response equivalence check.
- **`store_url_decode` vs `url_decode`** (`store_controller.c:326` /
  `wallet_view_helpers.c:550`) — the store version validates hex nibbles, the
  wallet one does not; merging onto the stricter one is a (safe but real) behavior
  change. Low value; only if a shared `lib/util` URL-decode lands.
- **base64 alphabet hardcoded ×3** + **31-site `chain_params_base58_prefix`
  fetch** — both low-value broad sweeps; pick up opportunistically inside a
  relevant wave, not standalone.

**Safe-axis status:** the non-consensus subsystems (tools/mcp, app/views,
lib/wallet, lib/rpc, app/models, domain, lib/util, app/controllers, service glue)
are now harvested — audits return mostly self-rejected noise. Remaining real work
is the deferred §12 consensus-sensitive items, the boot shutdown TU (HIGH-RISK,
SIGTERM proof, do ALONE), the flyclient/MMB extraction, and the owner-gated
peer-scoring enum. Do NOT keep fanning safe audits over the swept subsystems.

## Round 6 (2026-06-05) — never-swept small libs audited + fixed

Read-only audit (4 agents: znam/zslp, metrics/health/event, bloom/core/keys/encoding,
storage/policy/mining) → 17 findings (10 safe, 7 consensus-sensitive). Safe ones
executed as a 4-group edit→review workflow; union gate green (build 0 / lint 35 /
test_parallel 0/371). Landed `2bca9c825`, `594dba893`, `fba7f1f1a`:

- **znam DRY** — dropped `app/models` `is_valid_znam_name` dup, call canonical
  `znam_validate_name`. **event.c DRY** — `payload_is_text` + `format_payload_escaped`
  (byte-identical) + log observer-table-full. **mining** — LOG_FAIL on the
  `mine_block_pow` NULL guard; 6 `printf`→`LogPrintf` in gen.c.
- **~docs** for bloom/core/keys/metrics/znam/zslp public headers (reviewer fixed a
  `decode_secret` doc that wrongly claimed scalar-range validation).
- **Build regression fixed** (`fba7f1f1a`) — `zclassic-cli` failed to link
  (`EncodeBase64` undefined) since the base64 unification; `make all`/`make deploy`
  were broken. Added `utilstrencodings.c` to `CLI_SRCS`.

Adversarial review again earned its keep (caught the missed 6th gen.c printf + the
decode_secret doc lie). 

### Round-6 deferred (consensus/crypto-sensitive — NOT executed)

- **`lib/keys` asserts on the BIP32/secp256k1 path** (`pubkey.c:93-95,130`,
  `key.c:35,40,56,109,114`) — flagged as "disabled in Release". **VERIFIED NOT A
  LIVE BUG**: the production CFLAGS (Makefile:62) define **no `-DNDEBUG`**, so
  `assert()` is ACTIVE (fail-fast abort on violation). Converting to graceful
  error-returns is optional hardening *only if* `-DNDEBUG` is ever added — low
  priority, and a behavior change (abort→return) on a crypto path, so treat as
  consensus-sensitive if ever done.
- **znam/zslp builders return 0 without logging** (`znam.c:147-241`, `slp.c:170-292`)
  — on-chain OP_RETURN encode path; add `log_macros.h` + context. Consensus-adjacent
  (parse/serialize), defer with care.
- **mining PoW BLAKE2b-state duplicated** (`gen.c:38-48` vs `miner.c:223-253`) —
  consensus PoW format built two ways; extract one canonical builder. Repro/verify
  the personalization bytes are identical. Node isn't mining, low urgency.
- **mining `tx_size=250` hardcoded** (`miner.c:116`) — block-template fill uses a
  fixed size estimate instead of `transaction_serialize_size`; can mis-fill blocks.
  Block-construction path; defer.

**Safe-axis status (updated):** with rounds 2–6, every non-consensus subsystem
(util/rpc/wallet/models/views/mcp/controllers/domain + znam/zslp/metrics/health/
event/bloom/core/keys/encoding/mining-logging) is now swept. Audits return mostly
self-rejected noise or consensus-sensitive items. **The safe parallel axis is
DONE.** Remaining real work is all deferred/gated: §12 + the round-4/5/6 deferred
lists + the boot shutdown TU + flyclient/MMB + peer-scoring enum + the live wedge
(operational/owner-gated — see `[[project_live_wedge_rootcause_2026-06-05]]`).

## Deferred — peer_scoring typed-API adoption (needs enum extension, owner-gated)

Round-3 #1 (adopt typed `peer_scoring_record()` across ~34 raw
`peer_misbehaving(...,N,...)` sites in `lib/net/src/msgprocessor*.c`,
`msg_compact.c`) was ATTEMPTED and REVERTED. Two naive approaches both fail:
- **Map by meaning** → silently changes ban WEIGHTS (raw 20→INVALID_MESSAGE=10
  halved; raw 50→INVALID_BLOCK=100 doubled). A behavior change to a
  security-relevant DoS surface — must not land silently. (Caught by review.)
- **Map by weight (1:1)** → preserves behavior but mis-NAMES: a weight-20
  snapshot *parse* error becomes `PEER_OFFENCE_FLOOD` (the only weight-20 enum),
  which is a worse lie than the raw number.

Root cause: the enum has only 4 weight buckets (INVALID_MESSAGE/UNREQUESTED=10,
FLOOD=20, INVALID_HEADER=50, INVALID_BLOCK=100), with no name for the
snapshot/transport/proof rejection classes the snapshot path uses at weights
20/50/100. **The right fix EXTENDS `peer_scoring.h`** with semantically-accurate
offences at the SAME weights (e.g. `INVALID_SNAPSHOT=20`, a 50-weight
swarm-chunk class, `INVALID_PROOF=100` for flyclient/SHA3/merkle verification),
then maps each site to the enum that is BOTH weight-preserving AND honestly
named. That is enum design on a DoS-policy surface → owner-gated; the
`msg_blocks.c:540` dynamic-`dos` site (graded 1..49) needs a parametric record,
not a constant enum. Until then the raw `peer_misbehaving` calls stay — they do
not misrepresent the category.

## boot_services.c decomposition plan (2513 LOC → target the shutdown TU last)

Seam map (read-only audit `w6755v1wu`). Extract order by risk:

**Wave A (SAFE — independent, non-consensus, not in shutdown body):**
- `boot_sd_watchdog.c` (~110) — owns g_sd_watchdog_id/ctx, zero shared state. *(in flight)*
- `boot_node_utilities.c` (~130) — app_add_node, metrics start/stop, sync-state logger. *(in flight)*
- `boot_bg_verification.c` (~60) — bg-validation/hash-verify start/stop; re-checks finalized history, not the connect path. *(in flight)*

**Wave B (consensus-adjacent but NOT in shutdown body — boot-smoke validates):**
- `boot_runtime_sync_services.c` (~200) — header_probe / legacy_mirror / gap_fill /
  zclassicd_oracle / rolling_anchor start/stop wrappers. Stop ordering is preserved
  via the kernel spec table (they are not in the shutdown sequence body). Move
  byte-identical; boot-smoke on a copy.

**Wave C (needs prep first):**
- `boot_frontend_services.c` (~470, biggest payoff) — BLOCKED: shares the profile
  statics `boot_profile_has_explorer/store/onion` (13 read sites incl. stayers) and
  `boot_configure_frontend_rpc` is called from app_init. First promote the 3 trivial
  profile accessors to a shared header + make boot_configure_frontend_rpc public, THEN
  extract.
- `boot_flyclient_mmb.c` (~175, consensus) — BLOCKED: `g_mmb_leaf_store` is an extern
  global shared with boot_snapshot_offer.c + read in app_init's MMB-build block; a lint
  gate asserts msgprocessor_snapshot.c does NOT reference it. Extract only together with
  the MMB-build block, and update the gate's expected-owner file.

**HIGH-RISK (do ALONE, with a real SIGTERM stop/restart proof on a datadir COPY — boot-smoke CANNOT validate this):**
- The shutdown section (boot_services.c lines ~2168-2407): shutdown_stop_frontend_services,
  shutdown_persist_fast_restart_state, shutdown_flush_coins_to_sqlite,
  shutdown_quiesce_network_and_flush_coins, shutdown_persist_runtime_state,
  shutdown_release_owned_resources, app_shutdown_svc. The ordering invariant lives here:
  coins.db COMMIT (emergency flush + quiesce) MUST precede block_index fsync / flat-file
  save / block_tree+node_db close. See [[feedback_at_tip_kill9_ordering_invariant]].
- `boot_catchup_job.c` (~35) — small but `boot_join_catchup_service` is called from
  shutdown_persist_runtime_state, so it touches the SIGTERM teardown. Low payoff, not
  worth the shutdown risk until the shutdown TU is being done anyway.

## Dropped / opportunistic (pick up inside the relevant wave, not standalone)

`gap_fill_service` stale doc + descent swap; `stage_helpers.h` roster comment;
`msgprocessor` seen-ring unification; `sapling_keys` LE32 swap;
`zcashconsensus.h` / coins-decode docs. The `sprout_viewing_key_to_address`
placeholder: doc-only edit is safe; defer the `pk_enc = scalarmult_base(sk_enc)`
wiring as a separately-scoped change (derivation semantics).

## Round 8 (2026-06-05) — lib/net feature transports (UNswept until now)
8 read-only audit agents over the P2P feature/infra surface → 22 findings, 16 real
after vetting (rejected peer_lifecycle:507 — two same-condition ifs feed two
intentionally-distinct counters, a metrics behavior-change not a bug). Fixed in 4
commits (`4d60fc49d..436252ddb`): 5 public-fn NULL-deref guards (zmsg/protocol inv),
3 unchecked byte_stream writes (pong reply, getaddr) that could send a NULL/malformed
frame on OOM, 2 unchecked sqlite3_prepare_v2 in onion directory handlers (stepping
NULL), a torn onion-fetch record (body_len set on malloc fail), 5 ignored
fs_send_frame transmission errors, header docs + a dead fs_send_chunk decl removed.

## Round 9 (2026-06-05) — wide app/ + lib/ non-consensus sweep
16 read-only audit clusters → 47 findings; vetted, fixed in 7 commits
(`2867eb1ee..83ce1dece`):
- **Projection event-skip CLASS BUG** (highest value): wallet/mempool/peers/contacts
  projection catch_up advances last_consumed_offset in-flight but on COMMIT/ROLLBACK
  failure returned without restoring it from persisted meta → next catch_up skips the
  failed events. Fixed to match the correct utxo_projection sibling.
- 1-byte stack overflow (chain_inspect saplingtreeinfo hbuf[hlen]), favicon NULL
  datadir, unbounded strchr→memchr in tx vout parse, uninit health struct ×2,
  unchecked fread/fwrite in file manifest, GLib NULL+nonzero-size UB in wallet_gui,
  no-current-row sqlite read, truncated-blob z-addr encode, store.c const-cast-away,
  2 rpc NULL guards, https ssl/plain read-line fold, ~24 documented wallet/storage APIs.
- DEFERRED: wallet_key.c:562 "incomplete script data loading" — labeled DRY but may
  change wallet spend-path data loading; not batched, needs its own look.
