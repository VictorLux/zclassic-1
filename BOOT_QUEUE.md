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
- [ ] agent2: replay 7 remaining node_db_wipe_utxos call sites through recovery_policy
      Each site needs a reason string; refuse if policy_check_utxo_wipe returns non-ALLOW
- [ ] agent2: wire block_index_loader/chain_state_validator/utxo_recovery_service
      (Depends on those services landing — wave 5 items #3/#4/#5)

## History

(Empty — first entry will be added when AGENT1 batch-applies the initial queue.)
