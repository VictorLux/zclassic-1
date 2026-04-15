# Agent 2 Task: Wave 24 — Fix Triple-Wipe Bug (CRITICAL)

## Status
- LDB import WORKS — imports 1.34M UTXOs in 6s, finds correct chain tip at h=3078003
- But then `utxo_recovery_wipe` is called 3 TIMES during boot, destroying the imported UTXOs
- SHA3 verification then sees 0 UTXOs → ERROR
- Agent1 fixed the `coins_best_block` seed bug (skip when chainstate/ exists) — pushed

## THE BUG
Output from `-reimport-utxos` boot:
```
db: wiped UTXO set + coins state           ← wipe 1 (prepare_reimport — OK)
UTXO import: 1340852 rows written in 5992ms ← import succeeds!
db: wiped UTXO set + coins state           ← wipe 2 — DESTROYS IMPORTED DATA
db: wiped UTXO set + coins state           ← wipe 3 — redundant
SHA3 UTXO verification: 0 UTXOs            ← all gone
ERROR: only 0 UTXOs imported
```

## Priority Order
1. **Task 1: Fix triple-wipe in utxo_recovery_import_ldb** — find the 2nd and 3rd wipe calls and remove/guard them. File: `app/services/src/utxo_recovery_service.c`. The wipe BEFORE import is correct (line ~168). The wipes AFTER import are bugs.
2. **Task 2: Fix utxo_recovery_clean_above_tip** — at boot.c:1982, this runs after import. If chain tip was just set to 3M by the import, this should be a no-op. But verify it doesn't wipe anything.
3. **Task 3: Re-enable bg_hash_verify + address backfill** (same as before)

## See AGENT2.md for full context
