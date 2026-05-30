# Loudness + refactor board — "every failure must be loud and located"

_Generated 2026-05-30 from the `loud-errors-and-refactor-audit` workflow (5 read-only
lenses) + direct spot-checks against HEAD `2fe8ece67`. Motivated by the live incident:
zclassic23 SEGV-crash-looped ~hourly (6 core-dumps), each unclean kill scrambling
block-file↔index consistency across ~2,700 blocks (3,126,939–3,129,625) until the tip
wedged at 3,126,938; node halted for forensics._

## Headline: the crash-loudness promise is broken where it mattered — **P0 FIXED 2026-05-30**

The node crashed **6 times today and produced 0 backtraces anywhere** (journald + node.log
both empty). Confirmed causes — and the fix that landed:

- **Correction to the original diagnosis:** the *active* runtime handler is **`event.c:crash_signal_handler`**, not `signal_handler.c`'s — `event_install_crash_handler()` runs right after `signal_handler_install()` in boot and `sigaction()`-overrides all four signals. So the live marker is `FATAL SIGNAL`, not `[fatal-signal]` (the audit grep missed it). BOTH handlers are now fixed.
- ~~no `SA_ONSTACK` / no `sigaltstack()` → stack-overflow SEGV cannot run the handler~~ **FIXED:** both `event_install_crash_handler()` and `signal_handler_install()` now register a 64 KB alternate signal stack + `SA_ONSTACK`. A stack-overflow SEGV now runs the handler.
- ~~backtrace goes only to ephemeral stderr~~ **FIXED:** `signal_handler_set_crash_log($datadir/crash_log.txt)` (wired in `boot.c` once the datadir is known) opens an `O_APPEND|O_CLOEXEC` fd; both handlers mirror the marker + backtrace there and `fsync()` — survives even when systemd's stderr routing loses the buffer. Handler also stamps `pid` + `time`.
- ~~`config/src/boot.c:525` warns-and-continues~~ **FIXED:** install failure is now FATAL (`return false` → boot aborts; never run blind).
- Still external, owner-side: service `StandardError=append` + `core_pattern=|…/apport` discards non-packaged cores. The durable `crash_log.txt` makes this non-blocking for diagnosis (we no longer depend on the core or stderr). `EV_CRASH` is emitted to the in-RAM event ring by the active handler.

Net: a crash now leaves an fsync'd, located backtrace on disk regardless of stderr/core policy.

## Loudness gaps — by file (make every failure loud)

| sev | file | gap | fix | effort |
|---|---|---|---|---|
| ✅ | `lib/event/src/event.c` (active handler) + `lib/util/src/signal_handler.c` | no alt-stack ⇒ stack-overflow SEGV silent; backtrace only to stderr | **DONE:** alt-stack + `SA_ONSTACK` on both installers; durable fsync'd `$datadir/crash_log.txt` (mirrored from both handlers); `pid`+`time` stamped; `EV_CRASH` emitted to event ring | medium |
| ✅ | `config/src/boot.c` | signal-handler install failure warns-and-continues → early crashes silent | **DONE:** install failure now FATAL (`return false` → boot aborts) | trivial |
| 🟠 | `lib/sapling/src/bn254.c:2011` (`ppzksnark_verify`) | **loud-but-useless:** bg-validation re-verifying ancient Sprout proofs floods node.log with `pairing rejected` (1835 of 2000 recent lines = 92%) at 133% CPU, with **no height/txid context**. The chain itself is valid (tip byte-matches network), so these are advisory re-verify failures — either a phgr13 verifier limitation/false-reject or it should be silenced for trusted history | rate-limit/dedup the leaf log line; have the bg-validation caller emit ONE located `EV_PROOF_REJECTED{height,txid}` instead; separately investigate whether the phgr13 verifier false-rejects valid historical Sprout proofs (compare a fixed height vs zclassicd) | small (log) / medium (verifier) |
| 🔴 | `app/models/src/utxo.c:370-496` | **11 `fwrite`, 0 error checks** in snapshot writer; `fclose` unchecked; returns success on silent partial write (disk-full/IO) — *the snapshot-corruption enabler* | check each write/`ferror`/`fclose`; `io_error` flag; `LOG_FAIL` + fail the serialize | small |
| 🔴 | `app/jobs/src/proof_validate_stage.c`, `utxo_apply_stage.c`, `script_validate_stage.c` | reader (block deser) failure → `JOB_IDLE` + bare timestamp; indistinguishable from normal backpressure; no log, no blocker | `LOG_ERR` + register named `BLOCKER_TRANSIENT` + return `JOB_BLOCKED` on sustained reader failure | small |
| 🔴 | `lib/storage/src/coins_view_sqlite.c:360-382` | boot integrity guard **defers** drift >1000 blocks (returns true) — exactly how the 3,497-block drift slipped past auto-heal with no forward recovery | reject ANY >1-block overshoot with a named blocker that halts advance until cleared; document the recovery path | medium |
| 🔴 | `lib/validation/src/connect_tip.c:644-731` | `block_index` fsync **before** coins.db flush; SIGKILL between ⇒ tip ahead of UTXOs; no pre-flush marker; boot detects drift but never repairs | write `block_index_dirty` marker in node.db before index fsync; on boot, if set and mismatched, halt+named-blocker or auto-disconnect | medium |
| 🔴 | `lib/validation/src/update_coins.c` | event_log append + coins.db write not atomic ⇒ crash between = replay double-spend / stale UTXO | wrap append+write in one txn (or one AR save), fsync-ordered | medium |
| 🔴 | `lib/validation/src/process_block.c` | no stage cursor; crash mid-connect leaves undefined state; handler can't name height/stage | atomic stage cursor (VALIDATE→UTXO_APPLY→TIP_FINALIZE) logged before advance; crash hook reads it; `block_in_progress` blocker | medium |
| 🟠 | `lib/storage/src/disk_block_io.c` | 17 unchecked `fclose` (drops buffered-write/fsync errors); pos update not SIGSEGV-atomic | check `fclose`; snapshot `pos->nPos` to local, copy back only after fsync | small |
| 🟠 | `lib/validation/src/process_block_flush_policy.c:236-296` | coins flush failure returns `true` (non-fatal IBD) leaving dirty UTXOs in RAM; no blocker | on repeated/forced flush failure return false + `coins_flush_failed` blocker | small |
| 🟠 | `lib/validation/src/activate_best_chain.c:204,291,318,375` | `connect_tip`/`disconnect_tip` failures unlogged → reorg silently abandoned, tip wedged with no record | `LOG_FAIL` + `EV_TIP_CONNECT/DISCONNECT_FAILED` with the reason | small |
| 🟠 | `app/supervisors/src/staged_sync_supervisor.c` | 8 shadow-stage stalls emit `LOG_WARN` only, no blocker → 30-min stall, no operator escalation | register transient blocker on stall | medium |
| 🟠 | `app/services/src/block_index_loader.c` | raw `sqlite3_*` (15+) with no `ZCL_AR_RAW_SQL` exemption + no error logging | add exemption + `LOG_FAIL` on bind/step, or move behind `block_index_sidecar_port` | medium |
| 🟡 | ~23 sites in `app/models/`, `app/services/` (`database_migrate.c:129`, `block.c:508`, `recovery_policy.c`, `sha3_sidecar_io.c`, …) | `return -1/false` on error with no `LOG_FAIL`/`LOG_ERR` | add one-line `LOG_FAIL` with context | trivial each |
| 🟡 | `lib/util/include/util/safe_alloc.h:45` | `zcl_malloc` failure logs to stderr only, no event ⇒ NULL propagates, SEGVs elsewhere | `event_emitf(EV_MALLOC_FAILED,…)` on failure | small |
| 🟡 | `lib/storage/src/event_log.c` open-scan | partial-event truncation on boot not surfaced (silent history loss) | emit `EV_RECOVERY_TRUNCATED{offset}` when scan truncates | trivial |

## Structural refactor backlog — by file (what's left)

| file | LOC | what's left | gate |
|---|---|---|---|
| `config/src/boot_services.c` | 3360 | only 3 of ~18 supervisors extracted (net/chain/staged_sync); mempool/mining/wallet/RPC/Tor/HTTPS still imperative — dissolve into Supervisor declarations | A3 |
| `app/controllers/src/sync_controller_catchup.c` | 1262 | E1 over-ceiling; split into body-fetch backpressure service + queue-depth Condition; port source-selection | B8 |
| `app/services/src/legacy_mirror_sync_service.c` | 1180 | E1 over-ceiling; extract lag-SLO heartbeat → Condition, delete block-apply coordination | C2/B8 |
| `app/controllers/src/legacy_import.c` | 1105 | E1 over-ceiling; collapse into `legacy_bridge_job` + `legacy_poll_job` | C4/B8 |
| `app/controllers/src/sync_controller_import.c` | 995 | E1 over-ceiling; merge into legacy-import consolidation | C4/B8 |
| `app/events/src/` | 0 | Event shape unpopulated — extract Wave-S event subscriptions into one file per event type | B2 |
| `app/services/src/wallet_backup_service.c` | — | direct SQLite, needs `wallet_backup_store_port` | — |
| `app/services/src/zslp_service.c` | — | direct SQLite, needs `zslp_store_port` | — |
| E2 grandfathered (9) | — | bare bool/int → `zcl_result` (chain_evidence_controller, chain_state_repository, chain_tip, utxo_recovery_restore, utxo_recovery_service, …) | A4 |
| `tools/scripts/one_write_path_baseline.txt` | — | 66 dual-write surfaces (two paths to chain state) — the silent-halt root; removed when cutover permanent | E6/B8 |
| B8 apparatus | 9,448 / 32 files | comparison apparatus — delete post-flip-soak only | B8 |
| 4 dormant ports | — | `clock/consensus_log/event_emitter/snapshot_store` — adapters land later (Epoch-I scaffold, NOT cruft) | — |

## Priority

- **P0 — prevents a repeat of today, all shippable without the cutover:** signal_handler alt-stack + `EV_CRASH` + durable crash file; boot.c install-fatal; `utxo.c`+`disk_block_io.c` write-error checks; reducer-stage reader→blocker; boot index/coins divergence repair (`connect_tip` marker).
- **P1 — loudness sweep:** the ~23 `LOG_FAIL` additions; staged_sync stall blockers; `process_block` stage cursor; `update_coins` atomic pair.
- **P2 — structural/cutover-gated:** B8 deletion, supervisor extraction, the 2 ports, A4 `zcl_result` migration.

_The through-line: the loudness *infrastructure* exists (handler, blockers, Conditions, event
log, AR lifecycle) but is incompletely **wired** — and the biggest structural item (the 66
dual-write surfaces) is itself the silent-halt root that the cutover→B8 removes._
