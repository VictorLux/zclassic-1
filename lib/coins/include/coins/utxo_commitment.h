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

#endif
