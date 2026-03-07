/* Copyright (c) 2012-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "bloom_c.h"
#include "hash.h"
#include "random.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LN2SQUARED 0.4804530139182014246671025263266649717305529515945455
#define LN2 0.6931471805599453094172321214581765680755001343602552

static unsigned int bloom_hash(const struct bloom_filter *f, unsigned int hash_num,
                               const unsigned char *data, size_t len)
{
    return murmur_hash3(hash_num * 0xFBA4C795 + f->tweak, data, len) %
           (unsigned int)(f->data_size * 8);
}

static bool bloom_filter_init_internal(struct bloom_filter *f, unsigned int num_elements,
                                       double fp_rate, unsigned int tweak,
                                       unsigned char flags, bool constrained)
{
    unsigned int filter_bits;
    if (constrained) {
        unsigned int ideal = (unsigned int)(-1.0 / LN2SQUARED * num_elements * log(fp_rate));
        unsigned int max_bits = MAX_BLOOM_FILTER_SIZE * 8;
        filter_bits = ideal < max_bits ? ideal : max_bits;
    } else {
        filter_bits = (unsigned int)(-1.0 / LN2SQUARED * num_elements * log(fp_rate));
    }

    f->data_size = filter_bits / 8;
    if (f->data_size == 0) f->data_size = 1;
    f->data = calloc(f->data_size, 1);
    if (!f->data) return false;

    f->is_full = false;
    f->is_empty = true;

    if (constrained) {
        unsigned int ideal_funcs = (unsigned int)(f->data_size * 8 / num_elements * LN2);
        f->num_hash_funcs = ideal_funcs < MAX_BLOOM_HASH_FUNCS ? ideal_funcs : MAX_BLOOM_HASH_FUNCS;
    } else {
        f->num_hash_funcs = (unsigned int)(f->data_size * 8 / num_elements * LN2);
    }

    f->tweak = tweak;
    f->flags = flags;
    return true;
}

bool bloom_filter_init(struct bloom_filter *f, unsigned int num_elements,
                       double fp_rate, unsigned int tweak, unsigned char flags)
{
    return bloom_filter_init_internal(f, num_elements, fp_rate, tweak, flags, true);
}

void bloom_filter_free(struct bloom_filter *f)
{
    free(f->data);
    f->data = NULL;
    f->data_size = 0;
}

void bloom_filter_insert(struct bloom_filter *f, const unsigned char *data, size_t len)
{
    if (f->is_full) return;
    for (unsigned int i = 0; i < f->num_hash_funcs; i++) {
        unsigned int idx = bloom_hash(f, i, data, len);
        f->data[idx >> 3] |= (unsigned char)(1 << (7 & idx));
    }
    f->is_empty = false;
}

bool bloom_filter_contains(const struct bloom_filter *f, const unsigned char *data, size_t len)
{
    if (f->is_full) return true;
    if (f->is_empty) return false;
    for (unsigned int i = 0; i < f->num_hash_funcs; i++) {
        unsigned int idx = bloom_hash(f, i, data, len);
        if (!(f->data[idx >> 3] & (1 << (7 & idx))))
            return false;
    }
    return true;
}

void bloom_filter_insert_uint256(struct bloom_filter *f, const struct uint256 *hash)
{
    bloom_filter_insert(f, hash->data, 32);
}

bool bloom_filter_contains_uint256(const struct bloom_filter *f, const struct uint256 *hash)
{
    return bloom_filter_contains(f, hash->data, 32);
}

void bloom_filter_clear(struct bloom_filter *f)
{
    memset(f->data, 0, f->data_size);
    f->is_full = false;
    f->is_empty = true;
}

void bloom_filter_reset(struct bloom_filter *f, unsigned int new_tweak)
{
    bloom_filter_clear(f);
    f->tweak = new_tweak;
}

bool bloom_filter_is_within_size_constraints(const struct bloom_filter *f)
{
    return f->data_size <= MAX_BLOOM_FILTER_SIZE && f->num_hash_funcs <= MAX_BLOOM_HASH_FUNCS;
}

void bloom_filter_update_empty_full(struct bloom_filter *f)
{
    bool full = true;
    bool empty = true;
    for (size_t i = 0; i < f->data_size; i++) {
        full &= (f->data[i] == 0xff);
        empty &= (f->data[i] == 0);
    }
    f->is_full = full;
    f->is_empty = empty;
}

bool rolling_bloom_init(struct rolling_bloom_filter *f, unsigned int num_elements, double fp_rate)
{
    unsigned int tweak = (unsigned int)GetRand(UINT32_MAX);
    if (!bloom_filter_init_internal(&f->b1, num_elements * 2, fp_rate, tweak, BLOOM_UPDATE_NONE, false))
        return false;
    if (!bloom_filter_init_internal(&f->b2, num_elements * 2, fp_rate, tweak, BLOOM_UPDATE_NONE, false)) {
        bloom_filter_free(&f->b1);
        return false;
    }
    f->bloom_size = num_elements * 2;
    f->insertions = 0;
    return true;
}

void rolling_bloom_free(struct rolling_bloom_filter *f)
{
    bloom_filter_free(&f->b1);
    bloom_filter_free(&f->b2);
}

void rolling_bloom_insert(struct rolling_bloom_filter *f, const unsigned char *data, size_t len)
{
    if (f->insertions == 0) {
        bloom_filter_clear(&f->b1);
    } else if (f->insertions == f->bloom_size / 2) {
        bloom_filter_clear(&f->b2);
    }
    bloom_filter_insert(&f->b1, data, len);
    bloom_filter_insert(&f->b2, data, len);
    if (++f->insertions == f->bloom_size)
        f->insertions = 0;
}

bool rolling_bloom_contains(const struct rolling_bloom_filter *f, const unsigned char *data, size_t len)
{
    if (f->insertions < f->bloom_size / 2)
        return bloom_filter_contains(&f->b2, data, len);
    return bloom_filter_contains(&f->b1, data, len);
}

void rolling_bloom_reset(struct rolling_bloom_filter *f)
{
    unsigned int tweak = (unsigned int)GetRand(UINT32_MAX);
    bloom_filter_reset(&f->b1, tweak);
    bloom_filter_reset(&f->b2, tweak);
    f->insertions = 0;
}
