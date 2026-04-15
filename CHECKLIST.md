# ZClassic23 Known Issues Checklist

## Fixed

- [x] Block download stalling after P2P catchup — FIXED wave 23 (HAVE_DATA in chain selection + state gate + AT_TIP check)
- [x] SIGSEGV in bg_hash_verify — FIXED wave 22b
- [x] SIGSEGV in address backfill — FIXED wave 22b
- [x] Sapling tree lost on SIGKILL (5-min rebuild) — FIXED wave 24 (periodic checkpoint every 1000 blocks)

## Remaining

- [ ] UTXO/chain tip mismatch after LDB import — coins at h=3M, chain at h=2M, connect fails with bad-txns-inputs-missingorspent

## Investigation Results

### bg_validation multi-threaded crash (wave 24 task 2)

Script verification path is **thread-safe**. Investigation found:
- `secp256k1_ctx_verify` (pubkey.c:12): shared global, but libsecp256k1 guarantees thread-safe verification on shared contexts
- Script interpreter (interpreter.c): no static/global mutable state — all stack-local
- Sigcache (sigcache.c:80): properly mutex-protected (`zcl_mutex_lock`)
- Sighash computation (sighash.c): uses only const globals and local state
- `verify_shielded_proofs` runs in main thread only, not in workers
- Stack usage per worker: ~1.5 MB (two `script_stack` + altstack), fits default 8 MB pthread stack

**Minor issues found (non-crash):**
- `static int phgr_warn` (bg_validation_service.c:289): non-atomic increment, technically UB but only affects warning count
- `sig_cache_instance()` lazy init (sigcache.c:85): TOCTOU race, but init happens before workers start

**Conclusion:** The crash is likely NOT in the script interpreter. Possible causes: memory pressure (each worker allocates ~1.5 MB stack), or an issue already fixed by the cs_main locking. Recommend re-testing with 2 workers to see if the cs_main fix resolved it.
