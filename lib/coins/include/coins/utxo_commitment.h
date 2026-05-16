/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Incremental UTXO set commitment using XOR-hash accumulator.
 * Each UTXO is hashed to 32 bytes via SHA256(txid || vout || value || height).
 * The accumulator is the XOR of all UTXO hashes.
 * Add and remove are the same operation (XOR is self-inverse). */

#ifndef ZCL_UTXO_COMMITMENT_H
#define ZCL_UTXO_COMMITMENT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct utxo_commitment {
    uint8_t accumulator[32]; /* XOR of SHA256(utxo) for all UTXOs */
    uint64_t count;          /* number of UTXOs in the set */
};

/* Initialize to empty set */
void utxo_commitment_init(struct utxo_commitment *uc);

/* Add a UTXO to the commitment (XOR in its hash) */
void utxo_commitment_add(struct utxo_commitment *uc,
                          const uint8_t txid[32], uint32_t vout,
                          int64_t value, int32_t height);

/* Remove a UTXO from the commitment (XOR out its hash — same op) */
void utxo_commitment_remove(struct utxo_commitment *uc,
                             const uint8_t txid[32], uint32_t vout,
                             int64_t value, int32_t height);

/* Merge two commitments (XOR accumulators, add counts) */
void utxo_commitment_merge(struct utxo_commitment *dst,
                            const struct utxo_commitment *src);

/* Serialize: 32-byte accumulator + 8-byte count = 40 bytes */
#define UTXO_COMMITMENT_SERIALIZED_SIZE 40

void utxo_commitment_serialize(const struct utxo_commitment *uc,
                                uint8_t buf[UTXO_COMMITMENT_SERIALIZED_SIZE]);

bool utxo_commitment_deserialize(struct utxo_commitment *uc,
                                  const uint8_t *buf, size_t len);

/* Compare two commitments for equality */
bool utxo_commitment_equal(const struct utxo_commitment *a,
                            const struct utxo_commitment *b);

/* Skip commitment tracking during bulk operations (reindex).
 * When true, add/remove are no-ops for performance. */
extern _Atomic bool g_utxo_commitment_skip;

/* ── Checkpoint verification ──────────────────────────────── */

/* Recompute commitment from the full UTXO set in SQLite.
 * This is O(n) and should only be called at startup or periodically. */
struct sqlite3;
bool utxo_commitment_verify_db(struct sqlite3 *db,
                                const struct utxo_commitment *expected);

/* Compute commitment from SQLite UTXO set (result in out). */
void utxo_commitment_compute_db(struct sqlite3 *db,
                                 struct utxo_commitment *out);

/* Save commitment checkpoint to node_state table. */
bool utxo_commitment_save_checkpoint(struct sqlite3 *db,
                                      const struct utxo_commitment *uc);

/* Load commitment checkpoint from node_state table.
 * Returns false if no checkpoint stored. */
bool utxo_commitment_load_checkpoint(struct sqlite3 *db,
                                      struct utxo_commitment *uc);

/* ── SHA3-256 full-set commitment ────────────────────────── */

/* Deterministic SHA3-256 hash over the canonically ordered UTXO set.
 * Streams all UTXOs in (txid, vout) order into a single SHA3 context.
 * Each UTXO serialized as: txid(32) || vout_le(4) || value_le(8) ||
 *   script_len_le(4) || script(var) || height_le(4) || is_coinbase(1).
 * More secure than XOR accumulator but O(n). */
void utxo_commitment_sha3_compute(struct sqlite3 *db, uint8_t out[32],
                                   uint64_t *utxo_count);
void utxo_commitment_sha3_compute_table(struct sqlite3 *db,
                                        const char *table,
                                        uint8_t out[32],
                                        uint64_t *utxo_count);

/* Save/load SHA3 commitment to node_state (key='utxo_sha3'). */
bool utxo_commitment_sha3_save(struct sqlite3 *db, const uint8_t hash[32],
                                int32_t height, uint64_t count);
bool utxo_commitment_sha3_load(struct sqlite3 *db, uint8_t hash[32],
                                int32_t *height, uint64_t *count);

/* ── Full data integrity hash ────────────────────────────── */

/* SHA3-256 over ALL consensus-critical data in canonical order:
 * blocks, transactions, tx_inputs, tx_outputs, utxos,
 * sapling_nullifiers, sapling_outputs, sapling_spends,
 * sprout_nullifiers, joinsplits, zslp_tokens, zslp_transfers.
 * Each table hashed separately, then combined into a master hash.
 * Returns per-table hashes in the detail struct for diagnostics. */
struct data_integrity_detail {
    uint8_t blocks[32];
    uint8_t transactions[32];
    uint8_t tx_inputs[32];
    uint8_t tx_outputs[32];
    uint8_t utxos[32];
    uint8_t sapling_nullifiers[32];
    uint8_t sapling_outputs[32];
    uint8_t sapling_spends[32];
    uint8_t sprout_nullifiers[32];
    uint8_t joinsplits[32];
    uint8_t zslp_tokens[32];
    uint8_t zslp_transfers[32];
    uint8_t master[32];         /* SHA3-256 of all above concatenated */
};

void data_integrity_compute(struct sqlite3 *db,
                            struct data_integrity_detail *out);

#endif
