# ZClassic23 Known Issues Checklist

## Fixed

- [x] Block download stalling after P2P catchup — FIXED wave 23 (HAVE_DATA in chain selection + state gate + AT_TIP check)
- [x] SIGSEGV in bg_hash_verify — FIXED wave 22b
- [x] SIGSEGV in address backfill — FIXED wave 22b

## Remaining

- [ ] UTXO/chain tip mismatch after LDB import — coins at h=3M, chain at h=2M, connect fails with bad-txns-inputs-missingorspent
