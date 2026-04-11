# Boot.c Edit Queue

Only AGENT1 edits `config/src/boot.c`. AGENT2 and AGENT3 drop wire-up requests
here. AGENT1 batch-applies the queue once per wave cycle.

## Format

```
- [ ] agentN: short description — location hint (file:line or function name)
      Details: what the edit should do, plus any prerequisites.
```

## Queue

- [ ] agent2: wire `bii_verify()` call site for block_index_integrity
      Location: after block_index.bin load, before any write
      Refuse-to-boot on non-OK verdict unless ZCL_ALLOW_CORRUPT_INDEX=1
- [ ] agent2: wire `wallet_backup_start(&g_wallet_backup_cfg, &g_wallet)` after wallet_init
      Default config: backup_dir=~/wallet_backups, interval=3600, max_versions=168
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
      (Depends on those services landing — wave 5 items #3/#4/#5)
- [ ] agent2: wire `disk_monitor_start(&g_disk_monitor_cfg)` early in boot (wave 5 #7)
      Location: after datadir resolution, before any SQLite opens so the
      refuse-when-critical flag is armed before the first write can happen.
      Default cfg: datadir = resolved datadir path, env overrides for
      ZCL_DISK_WARN_BYTES / ZCL_DISK_REFUSE_BYTES / ZCL_DISK_POLL.
      Read `disk_monitor_is_critical()` in: mempool accept path,
      process_block write path, wallet_backup_run_once (skip backup if critical).
- [ ] agent2: wire `ibd_throttle_start(NULL)` early in boot (wave 6)
      Location: after datadir resolution, before any sync/peer code.
      `NULL` tells the service to read `ZCL_IBD_BLOCKS_PER_SEC` /
      `ZCL_IBD_BURST` from env with defaults 500/50. Pair with
      `ibd_throttle_stop()` in the shutdown path. Hot-path call:
      `ibd_throttle_acquire()` in `process_block.c` immediately
      before the `update_coins`/commit path (just before taking
      the DB write lock on the tip commit). Pass-through when
      not running so operators can disable entirely with no env.
- [ ] agent2: wire `mempool_limits_start(g_mempool, &g_mempool_limits_cfg)` (wave 6 #6)
      Location: after `tx_mempool_init` on the global mempool.
      Default cfg: `mempool_limits_config_defaults(&g_mempool_limits_cfg)`
      which reads ZCL_MEMPOOL_MAX_BYTES / ZCL_MEMPOOL_MAX_TXS /
      ZCL_MEMPOOL_EXPIRY_SECONDS / ZCL_MIN_RELAY_FEE_ZAT /
      ZCL_MEMPOOL_LIMITS_TICK_SEC. Pair with `mempool_limits_stop()`
      in the shutdown path next to the other service teardown calls.
      Registers a post-add hook on tx_mempool, so acceptance-path
      enforcement happens automatically — no call sites to change.

## History

(Empty — first entry will be added when AGENT1 batch-applies the initial queue.)
