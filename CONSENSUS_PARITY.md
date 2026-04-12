# Consensus Parity Audit — zclassic23 (C23) vs zclassicd (C++)

**Date:** 2026-04-12
**C23 node height:** 2,014,988
**C++ node height:** syncing (restarted, loading chainstate)
**Auditor:** AGENT2

---

## Methodology

Compare `getblockhash` at 10 heights spanning the full chain. Both implementations
set `coins_best_block = block_hash` after `ConnectBlock` / `connect_block`, so
matching block hashes at each height proves matching UTXO-set tip identity.

Additionally verify C23 internal consistency: `getbestblockhash` must equal
`getblockhash(getblockcount)`.

**Tool:** `tools/consensus_parity_audit.sh` — queries both nodes via RPC, compares
hashes, reports MATCH/MISMATCH/SKIP.

---

## Audit Heights

| Height    | C23 Block Hash                                                     | C++ Block Hash | Result |
|-----------|-------------------------------------------------------------------|----------------|--------|
| 1         | `0004b371c02c41c61c189ce04ac147220daf796e9a60ce90cfee31e5a71dda2d` | pending        | --     |
| 100       | `00001071e9da677300cf65b92f954b00184f941e6b51e9e6d5927be0ac277271` | pending        | --     |
| 1,000     | `000000311c66325218753433b49ec25150dc220af017fe1198e33f5ad4f71a51` | pending        | --     |
| 10,000    | `0000000a78beb57fa1c711e3067a721022582fb8f8950d857b6841abd09162aa` | pending        | --     |
| 100,000   | `000000016845a9f945079aa52680def9eafd5ea39c86dfdb5ef5630d18e3e74f` | pending        | --     |
| 500,000   | `00000000068bb87ac6e5b52ad17360ffdaed1ff0161b20a73993ed4de0500756` | pending        | --     |
| 1,000,000 | `00001e9a80eef39dd38e4280fd0e9fa6daba1ec1d2bcc26b4835a2991be21502` | pending        | --     |
| 1,500,000 | `00003026bfd01a2909ad66a43dd231f4ebfd869653fb0797be0f1e7607c5c825` | pending        | --     |
| 2,000,000 | `00000bfde9956d07350de33324802251c51b62a5b0df43d2fb7e240c75bd835b` | pending        | --     |
| 2,014,988 | `000011b06949fd1b0fab3409feacf465a9e08cc9366907f06e523127a5d4f235` | pending        | --     |

## C23 Internal Consistency

| Check                              | Value                                                              | Status     |
|------------------------------------|--------------------------------------------------------------------|------------|
| `getbestblockhash`                 | `000011b06949fd1b0fab3409feacf465a9e08cc9366907f06e523127a5d4f235` | --         |
| `getblockhash(2014988)`            | `000011b06949fd1b0fab3409feacf465a9e08cc9366907f06e523127a5d4f235` | --         |
| coins_best_block == chain tip      | --                                                                 | CONSISTENT |
| verificationprogress               | 1.0                                                                | COMPLETE   |

## Cross-Node Comparison

**Status:** C++ node (`zclassicd-rhett`) restarted 2026-04-12 13:22 UTC. Loading
chainstate database (~4%/min). RPC unavailable during initial load. Will re-run
`tools/consensus_parity_audit.sh` once synced and update this table.

To complete the audit:
```bash
./tools/consensus_parity_audit.sh
```

---

## Notes

- C23 fast-syncs via FlyClient + SHA3 UTXO snapshot; merkle roots for historical
  blocks are zero in the block index (full block data not stored). Block **hashes**
  are validated during fast sync via PoW verification on 50 random samples with
  MMB inclusion proofs (150-bit security).

- The C++ node stores full block data and validates every block during initial sync.
  Once both nodes are at the same height, matching `getblockhash` at every audit
  height proves both implementations accept the same chain.

- `coins_best_block` is set to the block hash after `connect_block` in C23
  (`connect_block.c:413`) and after `ConnectBlock` in C++ (`main.cpp:2700`).
  If both nodes agree on block hashes, they agree on `coins_best_block`.
