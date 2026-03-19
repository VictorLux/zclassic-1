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

#endif
