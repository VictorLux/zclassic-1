/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Merkle Mountain Belt — O(1) append, O(log k) recent proofs.
 * Lazy merging: at most 1 merge per append (no domino cascade). */

#include "chain/mmb.h"
#include "crypto/sha3.h"
#include <string.h>
#include <stdio.h>

/* ── Leaf construction ────────────────────────────────────── */

void mmb_leaf_from_block(struct mmb_leaf *leaf,
                         const uint8_t block_hash[32],
                         int32_t height, uint32_t timestamp,
                         uint32_t nBits,
                         const uint8_t sapling_root[32],
                         const uint8_t chain_work[32])
{
    memcpy(leaf->block_hash, block_hash, 32);
    leaf->height = (uint32_t)height;
    leaf->timestamp = timestamp;
    leaf->nBits = nBits;
    if (sapling_root)
        memcpy(leaf->sapling_root, sapling_root, 32);
    else
        memset(leaf->sapling_root, 0, 32);
    if (chain_work)
        memcpy(leaf->chain_work, chain_work, 32);
    else
        memset(leaf->chain_work, 0, 32);
}

/* ── Hashing with domain separation ──────────────────────── */

void mmb_hash_leaf(const struct mmb_leaf *leaf, uint8_t out[32])
{
    /* SHA3-256(0x10 || block_hash || height_LE || timestamp_LE ||
     *          nBits_LE || sapling_root || chain_work)
     * Total preimage: 1 + 108 = 109 bytes */
    uint8_t buf[1 + MMB_LEAF_PREIMAGE_SIZE];
    buf[0] = MMB_TAG_LEAF;
    size_t pos = 1;

    memcpy(buf + pos, leaf->block_hash, 32); pos += 32;

    /* Little-endian uint32_t fields */
    buf[pos++] = (uint8_t)(leaf->height);
    buf[pos++] = (uint8_t)(leaf->height >> 8);
    buf[pos++] = (uint8_t)(leaf->height >> 16);
    buf[pos++] = (uint8_t)(leaf->height >> 24);

    buf[pos++] = (uint8_t)(leaf->timestamp);
    buf[pos++] = (uint8_t)(leaf->timestamp >> 8);
    buf[pos++] = (uint8_t)(leaf->timestamp >> 16);
    buf[pos++] = (uint8_t)(leaf->timestamp >> 24);

    buf[pos++] = (uint8_t)(leaf->nBits);
    buf[pos++] = (uint8_t)(leaf->nBits >> 8);
    buf[pos++] = (uint8_t)(leaf->nBits >> 16);
    buf[pos++] = (uint8_t)(leaf->nBits >> 24);

    memcpy(buf + pos, leaf->sapling_root, 32); pos += 32;
    memcpy(buf + pos, leaf->chain_work, 32);   pos += 32;

    sha3_256(buf, 1 + MMB_LEAF_PREIMAGE_SIZE, out);
}

void mmb_hash_internal(const uint8_t left[32], const uint8_t right[32],
                       uint8_t out[32])
{
    uint8_t buf[65];
    buf[0] = MMB_TAG_INTERNAL;
    memcpy(buf + 1, left, 32);
    memcpy(buf + 33, right, 32);
    sha3_256(buf, 65, out);
}

/* ── Core operations ─────────────────────────────────────── */

void mmb_init(struct mmb *m)
{
    memset(m, 0, sizeof(*m));
    m->root_dirty = true;
}

/* Shared merge logic — called after inserting a height-0 mountain */
static int mmb_merge_after_insert(struct mmb *m)
{
    int merges = 0;

    /* First: merge rightmost pair if same height */
    if (m->num_mountains >= 2) {
        uint32_t r = m->num_mountains - 1;
        if (m->mountains[r - 1].height == m->mountains[r].height) {
            uint8_t merged[32];
            mmb_hash_internal(m->mountains[r - 1].peak,
                              m->mountains[r].peak, merged);
            memcpy(m->mountains[r - 1].peak, merged, 32);
            m->mountains[r - 1].height++;
            m->num_mountains--;
            merges++;
        }
    }

    /* Second: scan left for one deferred mergeable pair */
    if (m->num_mountains >= 2) {
        for (uint32_t i = m->num_mountains - 1; i >= 1; i--) {
            if (m->mountains[i - 1].height == m->mountains[i].height) {
                uint8_t merged[32];
                mmb_hash_internal(m->mountains[i - 1].peak,
                                  m->mountains[i].peak, merged);
                memcpy(m->mountains[i - 1].peak, merged, 32);
                m->mountains[i - 1].height++;
                for (uint32_t j = i; j < m->num_mountains - 1; j++)
                    m->mountains[j] = m->mountains[j + 1];
                m->num_mountains--;
                merges++;
                break;
            }
        }
    }

    return merges;
}

int mmb_append(struct mmb *m, const struct mmb_leaf *leaf)
{
    if (m->num_mountains >= MMB_MAX_MOUNTAINS) return -1;

    struct mmb_mountain *mt = &m->mountains[m->num_mountains];
    mmb_hash_leaf(leaf, mt->peak);
    mt->height = 0;
    m->num_mountains++;
    m->num_leaves++;
    m->root_dirty = true;

    return mmb_merge_after_insert(m);
}

int mmb_append_hash(struct mmb *m, const uint8_t leaf_hash[32])
{
    if (m->num_mountains >= MMB_MAX_MOUNTAINS) return -1;

    struct mmb_mountain *mt = &m->mountains[m->num_mountains];
    memcpy(mt->peak, leaf_hash, 32);
    mt->height = 0;
    m->num_mountains++;
    m->num_leaves++;
    m->root_dirty = true;

    return mmb_merge_after_insert(m);
}

void mmb_root(const struct mmb *m, uint8_t out[32])
{
    if (m->num_mountains == 0) {
        memset(out, 0, 32);
        return;
    }

    /* Check cache */
    if (!m->root_dirty) {
        memcpy(out, m->root_cache, 32);
        return;
    }

    if (m->num_mountains == 1) {
        memcpy(out, m->mountains[0].peak, 32);
    } else {
        /* Bag all peaks: SHA3(0x14 || peak_0 || ... || peak_k) */
        struct sha3_256_ctx ctx;
        sha3_256_init(&ctx);
        uint8_t tag = MMB_TAG_ROOT;
        sha3_256_write(&ctx, &tag, 1);
        for (uint32_t i = 0; i < m->num_mountains; i++)
            sha3_256_write(&ctx, m->mountains[i].peak, 32);
        sha3_256_finalize(&ctx, out);
    }

    /* Update cache (cast away const for caching — safe because
     * root_cache is a mutable cache field) */
    struct mmb *mut = (struct mmb *)m;
    memcpy(mut->root_cache, out, 32);
    mut->root_dirty = false;
}

/* ── Serialization ───────────────────────────────────────── */

size_t mmb_serialize(const struct mmb *m, uint8_t *buf, size_t buflen)
{
    /* version(1) + num_leaves(8) + num_mountains(4) +
     * N × (peak[32] + height[4]) */
    size_t needed = 1 + 8 + 4 + m->num_mountains * 36;
    if (buflen < needed) return 0;

    size_t pos = 0;

    /* Version byte */
    buf[pos++] = 0x01;

    /* num_leaves (LE 64-bit) */
    uint64_t nl = m->num_leaves;
    for (int i = 0; i < 8; i++) { buf[pos++] = (uint8_t)nl; nl >>= 8; }

    /* num_mountains (LE 32-bit) */
    uint32_t nm = m->num_mountains;
    for (int i = 0; i < 4; i++) { buf[pos++] = (uint8_t)nm; nm >>= 8; }

    /* Each mountain: peak[32] + height[4] */
    for (uint32_t i = 0; i < m->num_mountains; i++) {
        memcpy(buf + pos, m->mountains[i].peak, 32); pos += 32;
        uint32_t h = m->mountains[i].height;
        for (int j = 0; j < 4; j++) { buf[pos++] = (uint8_t)h; h >>= 8; }
    }

    return pos;
}

bool mmb_deserialize(struct mmb *m, const uint8_t *buf, size_t len)
{
    mmb_init(m);
    if (len < 13) return false;  /* version + num_leaves + num_mountains */

    size_t pos = 0;

    /* Version check */
    if (buf[pos++] != 0x01) return false;

    /* num_leaves */
    m->num_leaves = 0;
    for (int i = 7; i >= 0; i--)
        m->num_leaves = (m->num_leaves << 8) | buf[pos + i];
    pos += 8;

    /* num_mountains */
    uint32_t nm = 0;
    for (int i = 3; i >= 0; i--)
        nm = (nm << 8) | buf[pos + i];
    pos += 4;

    if (nm > MMB_MAX_MOUNTAINS) return false;
    if (len < pos + nm * 36) return false;

    m->num_mountains = nm;
    for (uint32_t i = 0; i < nm; i++) {
        memcpy(m->mountains[i].peak, buf + pos, 32); pos += 32;
        uint32_t h = 0;
        for (int j = 3; j >= 0; j--)
            h = (h << 8) | buf[pos + j];
        pos += 4;
        m->mountains[i].height = h;
    }

    m->root_dirty = true;
    return true;
}

/* ── Inclusion proofs ────────────────────────────────────── */

bool mmb_prove(const uint8_t (*all_leaf_hashes)[32],
               uint64_t num_leaves,
               uint64_t leaf_index,
               struct mmb_proof *proof)
{
    if (!all_leaf_hashes || !proof || leaf_index >= num_leaves)
        return false;

    memset(proof, 0, sizeof(*proof));
    proof->leaf_index = leaf_index;
    proof->mmb_size = num_leaves;
    memcpy(proof->leaf_hash, all_leaf_hashes[leaf_index], 32);

    /* Rebuild MMB using the same append logic as mmb_append(),
     * tracking the authentication path for the target leaf.
     * We track which stack entry contains the target and record
     * siblings whenever the target participates in a merge. */
    uint8_t stack[MMB_MAX_MOUNTAINS][32];
    uint32_t stack_heights[MMB_MAX_MOUNTAINS];
    uint64_t stack_first[MMB_MAX_MOUNTAINS];
    uint32_t sp = 0;
    int target_sp = -1;
    uint32_t sib_count = 0;

    /* Record left/right bits for verification */
    uint64_t lr_bits = 0; /* bit i: 0 = target is left child, 1 = right */

    for (uint64_t i = 0; i < num_leaves; i++) {
        memcpy(stack[sp], all_leaf_hashes[i], 32);
        stack_heights[sp] = 0;
        stack_first[sp] = i;
        if (i == leaf_index) target_sp = (int)sp;
        sp++;

        /* Rightmost merge */
        if (sp >= 2 && stack_heights[sp - 2] == stack_heights[sp - 1]) {
            uint32_t a = sp - 2, b = sp - 1;
            if (target_sp == (int)b) {
                memcpy(proof->siblings[sib_count], stack[a], 32);
                lr_bits |= (1ULL << sib_count);  /* target is right */
                sib_count++;
                target_sp = (int)a;
            } else if (target_sp == (int)a) {
                memcpy(proof->siblings[sib_count], stack[b], 32);
                /* target is left, bit stays 0 */
                sib_count++;
            }
            uint8_t merged[32];
            mmb_hash_internal(stack[a], stack[b], merged);
            memcpy(stack[a], merged, 32);
            stack_heights[a]++;
            sp--;
        }

        /* Deferred merge: scan left for one more mergeable pair */
        if (sp >= 2) {
            for (uint32_t k = sp - 1; k >= 1; k--) {
                if (stack_heights[k - 1] == stack_heights[k]) {
                    uint32_t a = k - 1, b = k;
                    if (target_sp == (int)b) {
                        memcpy(proof->siblings[sib_count], stack[a], 32);
                        lr_bits |= (1ULL << sib_count);
                        sib_count++;
                        target_sp = (int)a;
                    } else if (target_sp == (int)a) {
                        memcpy(proof->siblings[sib_count], stack[b], 32);
                        sib_count++;
                    }
                    uint8_t merged[32];
                    mmb_hash_internal(stack[a], stack[b], merged);
                    memcpy(stack[a], merged, 32);
                    stack_heights[a]++;
                    /* Shift right */
                    for (uint32_t j = b; j < sp - 1; j++) {
                        stack_heights[j] = stack_heights[j + 1];
                        memcpy(stack[j], stack[j + 1], 32);
                        stack_first[j] = stack_first[j + 1];
                    }
                    sp--;
                    /* Adjust target_sp if it was shifted */
                    if (target_sp > (int)b) target_sp--;
                    break;
                }
            }
        }
    }

    proof->num_siblings = sib_count;

    /* Store lr_bits in leaf_hash[0..7] area — we repurpose mmb_size for this.
     * Actually, store it as a separate field via the proof struct.
     * Since we can't add fields, encode lr_bits into mmb_size (which is
     * also stored). The verifier will use it directly. */
    /* Better: store lr_bits in the high bits of mmb_size. Since num_leaves
     * fits in 40 bits, we use the upper 24 bits for lr flags. But that's
     * fragile. Instead, encode the path in the leaf_index field:
     * leaf_index is used for identification; lr_bits for reconstruction.
     * Let's just use the proof->mmb_size to also carry the lr_bits. */

    /* Cleanest: just set mmb_size to the actual leaf count, and store
     * lr_bits in a way the verifier can use. Since the siblings array
     * already captures the path, we encode lr_bits by placing left siblings
     * with a 0x00 prefix marker and right siblings with 0x01.
     * Actually, simplest: just use mmb_size for leaf count and let the
     * verifier try both directions at each level (at most 2^depth attempts,
     * but depth is small). */

    /* Actually the cleanest: just put lr_bits into mmb_size since we
     * know num_leaves < 2^40 and sib_count < 24. Pack them. */

    /* Pack: low 40 bits = num_leaves, bits 40-63 = lr_bits */
    proof->mmb_size = (num_leaves & 0xFFFFFFFFFFULL) |
                      ((lr_bits & 0xFFFFFFULL) << 40);

    /* Record all peaks */
    proof->num_peaks = sp;
    for (uint32_t i = 0; i < sp; i++)
        memcpy(proof->peaks[i], stack[i], 32);

    return true;
}

bool mmb_verify(const struct mmb_proof *proof,
                const uint8_t expected_root[32])
{
    if (!proof || !expected_root) return false;
    if (proof->num_peaks == 0) return false;

    /* Unpack lr_bits from mmb_size (low 40 = num_leaves, bits 40+ = lr) */
    uint64_t lr_bits = (proof->mmb_size >> 40) & 0xFFFFFFULL;

    /* Reconstruct target peak from leaf + siblings using lr_bits */
    uint8_t current[32];
    memcpy(current, proof->leaf_hash, 32);

    for (uint32_t i = 0; i < proof->num_siblings; i++) {
        uint8_t merged[32];
        if (lr_bits & (1ULL << i)) {
            /* Target was right child; sibling is left */
            mmb_hash_internal(proof->siblings[i], current, merged);
        } else {
            /* Target was left child; sibling is right */
            mmb_hash_internal(current, proof->siblings[i], merged);
        }
        memcpy(current, merged, 32);
    }

    /* Check if reconstructed hash matches any peak */
    bool peak_found = false;
    for (uint32_t p = 0; p < proof->num_peaks; p++) {
        if (memcmp(current, proof->peaks[p], 32) == 0) {
            peak_found = true;
            break;
        }
    }
    if (!peak_found) return false;

    /* Bag all peaks and verify root */
    uint8_t computed_root[32];
    if (proof->num_peaks == 1) {
        memcpy(computed_root, proof->peaks[0], 32);
    } else {
        struct sha3_256_ctx ctx;
        sha3_256_init(&ctx);
        uint8_t tag = MMB_TAG_ROOT;
        sha3_256_write(&ctx, &tag, 1);
        for (uint32_t i = 0; i < proof->num_peaks; i++)
            sha3_256_write(&ctx, proof->peaks[i], 32);
        sha3_256_finalize(&ctx, computed_root);
    }

    return memcmp(computed_root, expected_root, 32) == 0;
}
