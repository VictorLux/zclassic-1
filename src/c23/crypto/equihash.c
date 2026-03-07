/* Copyright (c) 2016 Jack Grigg
 * Copyright (c) 2016 The Zcash developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * Equihash proof-of-work verification — pure C23 implementation. */

#include "crypto/equihash.h"
#include <assert.h>
#include <string.h>
#include <stdlib.h>

void equihash_params_init(struct equihash_params *p,
                          unsigned int N, unsigned int K)
{
    p->N = N;
    p->K = K;
    p->indices_per_hash_output = 512 / N;
    p->hash_output = p->indices_per_hash_output * N / 8;
    p->collision_bit_length = N / (K + 1);
    p->collision_byte_length = (p->collision_bit_length + 7) / 8;
    p->hash_length = (K + 1) * p->collision_byte_length;
    p->final_full_width = 2 * p->collision_byte_length +
                          sizeof(eh_index) * ((size_t)1 << K);
    p->solution_width = ((size_t)1 << K) * (p->collision_bit_length + 1) / 8;
}

int equihash_initialise_state(const struct equihash_params *p,
                              struct blake2b_ctx *state)
{
    uint32_t le_N = p->N;
    uint32_t le_K = p->K;
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    le_N = __builtin_bswap32(le_N);
    le_K = __builtin_bswap32(le_K);
#endif

    uint8_t personalization[BLAKE2B_PERSONALBYTES] = {0};
    memcpy(personalization, "ZcashPoW", 8);
    memcpy(personalization + 8, &le_N, 4);
    memcpy(personalization + 12, &le_K, 4);

    return blake2b_init_salt_personal(state,
                                      p->hash_output,
                                      NULL, 0,
                                      NULL,
                                      personalization);
}

static void generate_hash(const struct blake2b_ctx *base_state,
                           eh_index g,
                           unsigned char *hash, size_t hash_len)
{
    struct blake2b_ctx state = *base_state;
    eh_index lei = g;
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    lei = __builtin_bswap32(lei);
#endif
    blake2b_update(&state, (const unsigned char *)&lei, sizeof(eh_index));
    blake2b_final(&state, hash, hash_len);
}

void eh_expand_array(const unsigned char *in, size_t in_len,
                     unsigned char *out, size_t out_len,
                     size_t bit_len, size_t byte_pad)
{
    assert(bit_len >= 8);
    assert(8 * sizeof(uint32_t) >= 7 + bit_len);

    size_t out_width = (bit_len + 7) / 8 + byte_pad;
    assert(out_len == 8 * out_width * in_len / bit_len);

    uint32_t bit_len_mask = ((uint32_t)1 << bit_len) - 1;

    size_t acc_bits = 0;
    uint32_t acc_value = 0;

    size_t j = 0;
    for (size_t i = 0; i < in_len; i++) {
        acc_value = (acc_value << 8) | in[i];
        acc_bits += 8;

        if (acc_bits >= bit_len) {
            acc_bits -= bit_len;
            for (size_t x = 0; x < byte_pad; x++)
                out[j + x] = 0;
            for (size_t x = byte_pad; x < out_width; x++) {
                out[j + x] = (unsigned char)(
                    (acc_value >> (acc_bits + (8 * (out_width - x - 1)))) &
                    ((bit_len_mask >> (8 * (out_width - x - 1))) & 0xFF));
            }
            j += out_width;
        }
    }
}

void eh_compress_array(const unsigned char *in, size_t in_len,
                       unsigned char *out, size_t out_len,
                       size_t bit_len, size_t byte_pad)
{
    assert(bit_len >= 8);
    assert(8 * sizeof(uint32_t) >= 7 + bit_len);

    size_t in_width = (bit_len + 7) / 8 + byte_pad;
    assert(out_len == bit_len * in_len / (8 * in_width));

    uint32_t bit_len_mask = ((uint32_t)1 << bit_len) - 1;

    size_t acc_bits = 0;
    uint32_t acc_value = 0;

    size_t j = 0;
    for (size_t i = 0; i < out_len; i++) {
        if (acc_bits < 8) {
            acc_value = acc_value << bit_len;
            for (size_t x = byte_pad; x < in_width; x++) {
                acc_value = acc_value |
                    ((in[j + x] &
                      ((bit_len_mask >> (8 * (in_width - x - 1))) & 0xFF))
                     << (8 * (in_width - x - 1)));
            }
            j += in_width;
            acc_bits += bit_len;
        }
        acc_bits -= 8;
        out[i] = (unsigned char)((acc_value >> acc_bits) & 0xFF);
    }
}

void eh_index_to_array(eh_index i, unsigned char *array)
{
    eh_index bei;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    bei = __builtin_bswap32(i);
#else
    bei = i;
#endif
    memcpy(array, &bei, sizeof(eh_index));
}

eh_index eh_array_to_index(const unsigned char *array)
{
    eh_index bei;
    memcpy(&bei, array, sizeof(eh_index));
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap32(bei);
#else
    return bei;
#endif
}

size_t eh_get_indices_from_minimal(const unsigned char *minimal,
                                   size_t minimal_len,
                                   size_t collision_bit_len,
                                   eh_index *indices_out,
                                   size_t max_indices)
{
    assert(((collision_bit_len + 1) + 7) / 8 <= sizeof(eh_index));
    size_t len_indices = 8 * sizeof(eh_index) * minimal_len /
                         (collision_bit_len + 1);
    size_t byte_pad = sizeof(eh_index) - ((collision_bit_len + 1) + 7) / 8;

    unsigned char *array = malloc(len_indices);
    if (!array) return 0;
    eh_expand_array(minimal, minimal_len,
                    array, len_indices,
                    collision_bit_len + 1, byte_pad);

    size_t count = 0;
    for (size_t i = 0; i < len_indices && count < max_indices;
         i += sizeof(eh_index)) {
        indices_out[count++] = eh_array_to_index(array + i);
    }
    free(array);
    return count;
}

size_t eh_get_minimal_from_indices(const eh_index *indices,
                                   size_t num_indices,
                                   size_t collision_bit_len,
                                   unsigned char *minimal_out,
                                   size_t max_len)
{
    assert(((collision_bit_len + 1) + 7) / 8 <= sizeof(eh_index));
    size_t len_indices = num_indices * sizeof(eh_index);
    size_t min_len = (collision_bit_len + 1) * len_indices /
                     (8 * sizeof(eh_index));
    size_t byte_pad = sizeof(eh_index) - ((collision_bit_len + 1) + 7) / 8;

    if (min_len > max_len) return 0;

    unsigned char *array = malloc(len_indices);
    if (!array) return 0;
    for (size_t i = 0; i < num_indices; i++)
        eh_index_to_array(indices[i], array + i * sizeof(eh_index));

    eh_compress_array(array, len_indices,
                      minimal_out, min_len,
                      collision_bit_len + 1, byte_pad);
    free(array);
    return min_len;
}

/* A row in the verification: hash data + appended indices.
 * We dynamically allocate since final_full_width varies by parameters. */
struct eh_row {
    unsigned char *data;
};

static void eh_row_from_hash(struct eh_row *row,
                              const unsigned char *hash_in,
                              size_t h_in_len,
                              size_t h_len,
                              size_t collision_bit_len,
                              eh_index idx,
                              size_t width)
{
    row->data = calloc(1, width);
    if (!row->data) return;
    eh_expand_array(hash_in, h_in_len, row->data, h_len,
                    collision_bit_len, 0);
    eh_index_to_array(idx, row->data + h_len);
}

static bool eh_row_has_collision(const struct eh_row *a,
                                  const struct eh_row *b,
                                  size_t collision_byte_len)
{
    return memcmp(a->data, b->data, collision_byte_len) == 0;
}

static bool eh_row_indices_before(const struct eh_row *a,
                                   const struct eh_row *b,
                                   size_t hash_len,
                                   size_t len_indices)
{
    return memcmp(a->data + hash_len, b->data + hash_len, len_indices) < 0;
}

static bool eh_row_distinct_indices(const struct eh_row *a,
                                     const struct eh_row *b,
                                     size_t hash_len,
                                     size_t len_indices)
{
    for (size_t i = 0; i < len_indices; i += sizeof(eh_index)) {
        for (size_t j = 0; j < len_indices; j += sizeof(eh_index)) {
            if (memcmp(a->data + hash_len + i,
                       b->data + hash_len + j,
                       sizeof(eh_index)) == 0)
                return false;
        }
    }
    return true;
}

static bool eh_row_is_zero(const struct eh_row *row, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (row->data[i] != 0)
            return false;
    }
    return true;
}

static void eh_row_xor_merge(struct eh_row *out,
                               const struct eh_row *a,
                               const struct eh_row *b,
                               size_t hash_len,
                               size_t len_indices,
                               size_t collision_byte_len,
                               size_t width)
{
    out->data = calloc(1, width);
    if (!out->data) return;

    size_t trim = collision_byte_len;
    for (size_t i = trim; i < hash_len; i++)
        out->data[i - trim] = a->data[i] ^ b->data[i];

    if (eh_row_indices_before(a, b, hash_len, len_indices)) {
        memcpy(out->data + hash_len - trim,
               a->data + hash_len, len_indices);
        memcpy(out->data + hash_len - trim + len_indices,
               b->data + hash_len, len_indices);
    } else {
        memcpy(out->data + hash_len - trim,
               b->data + hash_len, len_indices);
        memcpy(out->data + hash_len - trim + len_indices,
               a->data + hash_len, len_indices);
    }
}

bool equihash_is_valid_solution(const struct equihash_params *p,
                                const struct blake2b_ctx *base_state,
                                const unsigned char *soln, size_t soln_len)
{
    if (soln_len != p->solution_width)
        return false;

    size_t num_indices = (size_t)1 << p->K;
    eh_index *indices = malloc(num_indices * sizeof(eh_index));
    if (!indices) return false;

    size_t got = eh_get_indices_from_minimal(soln, soln_len,
                                             p->collision_bit_length,
                                             indices, num_indices);
    if (got != num_indices) {
        free(indices);
        return false;
    }

    size_t width = p->final_full_width;
    struct eh_row *X = malloc(num_indices * sizeof(struct eh_row));
    if (!X) { free(indices); return false; }

    unsigned char *tmp_hash = malloc(p->hash_output);
    if (!tmp_hash) { free(indices); free(X); return false; }

    for (size_t i = 0; i < num_indices; i++) {
        generate_hash(base_state,
                      indices[i] / (unsigned int)p->indices_per_hash_output,
                      tmp_hash, p->hash_output);
        size_t offset = (indices[i] % p->indices_per_hash_output) * p->N / 8;
        eh_row_from_hash(&X[i], tmp_hash + offset,
                         p->N / 8, p->hash_length,
                         p->collision_bit_length, indices[i], width);
        if (!X[i].data) {
            for (size_t j = 0; j < i; j++) free(X[j].data);
            free(X); free(indices); free(tmp_hash);
            return false;
        }
    }
    free(tmp_hash);
    free(indices);

    size_t hash_len = p->hash_length;
    size_t len_indices = sizeof(eh_index);
    size_t count = num_indices;

    while (count > 1) {
        struct eh_row *Xc = malloc((count / 2) * sizeof(struct eh_row));
        if (!Xc) {
            for (size_t i = 0; i < count; i++) free(X[i].data);
            free(X);
            return false;
        }

        for (size_t i = 0; i < count; i += 2) {
            if (!eh_row_has_collision(&X[i], &X[i + 1],
                                      p->collision_byte_length)) {
                for (size_t j = 0; j < count; j++) free(X[j].data);
                free(X); free(Xc);
                return false;
            }
            if (eh_row_indices_before(&X[i + 1], &X[i],
                                       hash_len, len_indices)) {
                for (size_t j = 0; j < count; j++) free(X[j].data);
                free(X); free(Xc);
                return false;
            }
            if (!eh_row_distinct_indices(&X[i], &X[i + 1],
                                          hash_len, len_indices)) {
                for (size_t j = 0; j < count; j++) free(X[j].data);
                free(X); free(Xc);
                return false;
            }
            eh_row_xor_merge(&Xc[i / 2], &X[i], &X[i + 1],
                              hash_len, len_indices,
                              p->collision_byte_length, width);
            if (!Xc[i / 2].data) {
                for (size_t j = 0; j < count; j++) free(X[j].data);
                for (size_t j = 0; j < i / 2; j++) free(Xc[j].data);
                free(X); free(Xc);
                return false;
            }
        }

        for (size_t i = 0; i < count; i++) free(X[i].data);
        free(X);
        X = Xc;
        hash_len -= p->collision_byte_length;
        len_indices *= 2;
        count /= 2;
    }

    bool valid = (count == 1) && eh_row_is_zero(&X[0], hash_len);
    free(X[0].data);
    free(X);
    return valid;
}
