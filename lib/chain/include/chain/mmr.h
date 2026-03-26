/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Merkle Mountain Range (MMR) — append-only authenticated data structure
 * over block hashes. Enables O(log n) inclusion proofs between power nodes.
 *
 * Uses SHA3-256 with domain separation:
 *   Leaf:     SHA3-256(0x00 || block_hash)
 *   Internal: SHA3-256(0x01 || left || right)
 *   Root:     SHA3-256(0x02 || peak_0 || peak_1 || ... || peak_k)
 */

#ifndef ZCL_CHAIN_MMR_H
#define ZCL_CHAIN_MMR_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define MMR_HASH_SIZE 32
#define MMR_MAX_PEAKS 64  /* supports up to 2^64 leaves */

/* Domain separation tags */
#define MMR_TAG_LEAF     0x00
#define MMR_TAG_INTERNAL 0x01
#define MMR_TAG_ROOT     0x02

struct mmr {
    uint64_t num_leaves;
    uint8_t  peaks[MMR_MAX_PEAKS][MMR_HASH_SIZE];
    uint32_t num_peaks;
};

/* Initialize empty MMR */
void mmr_init(struct mmr *m);

/* Append a block hash as a new leaf. Updates peaks and returns
 * the number of new internal nodes created (for persistence). */
int mmr_append(struct mmr *m, const uint8_t block_hash[32]);

/* Compute the MMR root from current peaks */
void mmr_root(const struct mmr *m, uint8_t out[32]);

/* Hash a leaf (exposed for testing/persistence) */
void mmr_hash_leaf(const uint8_t block_hash[32], uint8_t out[32]);

/* Hash two children into a parent */
void mmr_hash_internal(const uint8_t left[32], const uint8_t right[32],
                       uint8_t out[32]);

/* ── Serialization ─────────────────────────────────────── */

/* Max serialized size: 8 (num_leaves) + 4 (num_peaks) + peaks */
#define MMR_SERIALIZED_MAX (8 + 4 + MMR_MAX_PEAKS * MMR_HASH_SIZE)

size_t mmr_serialize(const struct mmr *m, uint8_t *buf, size_t buflen);
bool mmr_deserialize(struct mmr *m, const uint8_t *buf, size_t len);

/* ── Inclusion proofs ──────────────────────────────────── */

struct mmr_proof {
    uint64_t leaf_index;                    /* 0-based leaf index */
    uint8_t  leaf_hash[MMR_HASH_SIZE];      /* SHA3(0x00 || block_hash) */
    uint8_t  siblings[MMR_MAX_PEAKS][MMR_HASH_SIZE];
    uint32_t num_siblings;
    uint8_t  peak_hashes[MMR_MAX_PEAKS][MMR_HASH_SIZE];
    uint32_t num_peaks;
    uint64_t mmr_size;                      /* num_leaves at proof time */
};

/* Generate inclusion proof for a leaf.
 * Requires all_leaves array of block hashes (or NULL + db for lazy load).
 * Returns true on success. */
bool mmr_prove_from_leaves(const uint8_t (*all_leaves)[32],
                           uint64_t num_leaves,
                           uint64_t leaf_index,
                           struct mmr_proof *proof);

/* Verify a proof against an expected MMR root */
bool mmr_verify(const struct mmr_proof *proof,
                const uint8_t expected_root[32]);

#endif
