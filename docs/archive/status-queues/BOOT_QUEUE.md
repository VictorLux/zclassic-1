# Boot.c Edit Queue

Only AGENT1 edits `config/src/boot.c`. AGENT2 and AGENT3 drop wire-up requests
here. AGENT1 batch-applies the queue once per wave cycle.

## Format

```
- [ ] agentN: short description — location hint (file:line or function name)
      Details: what the edit should do, plus any prerequisites.
```

## Queue

- [x] agent2: wire `bii_verify()` call site for block_index_integrity
      Location: after block_index.bin load, before any write
      Refuse-to-boot on non-OK verdict unless ZCL_ALLOW_CORRUPT_INDEX=1
      **DONE** in agent1 BOOT_QUEUE batch — wired after nChainTx propagation,
      before LDB UTXO import. Quarantines on mismatch, allows BII_SIDECAR_MISSING.
- [x] agent2: wire `wallet_backup_start(&g_wallet_backup_cfg, &g_wallet)` after wallet_init
      Default config: backup_dir=~/wallet_backups, interval=3600, max_versions=168
      **DONE** in agent1 BOOT_QUEUE batch — wired after wallet load/migration/keygen.
- [x] agent2: replay all 8 node_db_wipe_utxos call sites through recovery_policy
      **DONE** in `e60925314` — new `boot_policy_wipe_utxos(reason)` helper
      at the top of boot.c counts rows, loads policy from env, calls
      `policy_check_utxo_wipe(cap, reason)`, refuses loudly when over cap.
      Every site now carries a grep-able reason: `boot.reimport_utxos_flag`,
      `boot.ldb_import_{legacy,underrun,prepare,failed_retry}`,
      `boot.restore_no_utxos`, `boot.reset_to_genesis`,
      `boot.stale_utxos_at_genesis`. This is the gate that would have
      saved the 1.3M UTXOs on 2026-04-10. `./test_zcl` green.
- [ ] agent2: wire block_index_loader/chain_state_validator/utxo_recovery_service
      (Depends on those services landing — wave 10 items #4/#5/#6)
- [x] agent2: wire `disk_monitor_start(&g_disk_monitor_cfg)` early in boot (wave 5 #7)
      Location: after datadir resolution, before any SQLite opens so the
      refuse-when-critical flag is armed before the first write can happen.
      **DONE** in agent1 BOOT_QUEUE batch — wired after datadir mkdir,
      before ECC init. Config reads env overrides. Paired with
      `disk_monitor_stop()` in shutdown.
- [x] agent2: wire `ibd_throttle_start(NULL)` early in boot (wave 6)
      Location: after datadir resolution, before any sync/peer code.
      **DONE** in agent1 BOOT_QUEUE batch — wired after disk_monitor,
      before ECC init. Reads env with defaults 500/50. Paired with
      `ibd_throttle_stop()` in shutdown.
- [x] agent2: wire `mempool_limits_start(g_mempool, &g_mempool_limits_cfg)` (wave 6 #6)
      Location: after `tx_mempool_init` on the global mempool.
      **DONE** in agent1 BOOT_QUEUE batch — wired in boot_services.c
      immediately after tx_mempool_init, before mempool_load. Config
      reads env with defaults. Paired with `mempool_limits_stop()` in shutdown.

## History

- **2026-04-12** AGENT1 batch-applied 5 queued services: bii_verify (boot.c),
  wallet_backup_start (boot.c), disk_monitor_start (boot.c),
  ibd_throttle_start (boot.c), mempool_limits_start (boot_services.c).
  All 4 `_stop()` calls wired in `shutdown_persist_runtime_state`.
  Remaining: block_index_loader/chain_state_validator/utxo_recovery_service
  (blocked on those services landing in wave 10).
