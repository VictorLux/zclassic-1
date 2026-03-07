/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_SCRIPT_INTERPRETER_H
#define ZCL_SCRIPT_INTERPRETER_H

#include "script/script_error.h"
#include "script/script.h"
#include "script/script_flags.h"
#include "script/sighashtype.h"
#include "core/uint256.h"
#include "core/amount.h"
#include <stdbool.h>
#include <stdint.h>

#define MAX_STACK_ITEMS 1000
#define MAX_STACK_ELEMENT_SIZE MAX_SCRIPT_ELEMENT_SIZE

struct stack_item {
    unsigned char data[MAX_STACK_ELEMENT_SIZE];
    size_t size;
};

struct script_stack {
    struct stack_item items[MAX_STACK_ITEMS];
    size_t count;
};

static inline void stack_init(struct script_stack *s) { s->count = 0; }

static inline bool stack_push(struct script_stack *s,
                              const unsigned char *data, size_t len)
{
    if (s->count >= MAX_STACK_ITEMS || len > MAX_STACK_ELEMENT_SIZE)
        return false;
    if (len > 0) memcpy(s->items[s->count].data, data, len);
    s->items[s->count].size = len;
    s->count++;
    return true;
}

static inline bool stack_push_empty(struct script_stack *s)
{
    return stack_push(s, NULL, 0);
}

static inline struct stack_item *stack_top(struct script_stack *s, int i)
{
    return &s->items[(int)s->count + i];
}

static inline bool stack_pop(struct script_stack *s)
{
    if (s->count == 0) return false;
    s->count--;
    return true;
}

static inline bool stack_erase_at(struct script_stack *s, size_t idx)
{
    if (idx >= s->count) return false;
    for (size_t i = idx; i < s->count - 1; i++)
        s->items[i] = s->items[i + 1];
    s->count--;
    return true;
}

static inline bool stack_erase_range(struct script_stack *s,
                                     size_t from, size_t to)
{
    if (from > to || to > s->count) return false;
    size_t n = to - from;
    for (size_t i = from; i < s->count - n; i++)
        s->items[i] = s->items[i + n];
    s->count -= n;
    return true;
}

static inline bool stack_insert_at(struct script_stack *s, size_t idx,
                                   const struct stack_item *item)
{
    if (s->count >= MAX_STACK_ITEMS || idx > s->count) return false;
    for (size_t i = s->count; i > idx; i--)
        s->items[i] = s->items[i - 1];
    s->items[idx] = *item;
    s->count++;
    return true;
}

static inline void stack_swap_items(struct stack_item *a, struct stack_item *b)
{
    struct stack_item tmp = *a;
    *a = *b;
    *b = tmp;
}

static inline bool cast_to_bool(const struct stack_item *item)
{
    for (size_t i = 0; i < item->size; i++) {
        if (item->data[i] != 0) {
            if (i == item->size - 1 && item->data[i] == 0x80)
                return false;
            return true;
        }
    }
    return false;
}

struct sig_checker {
    bool (*check_sig)(const struct sig_checker *self,
                      const unsigned char *sig, size_t siglen,
                      const unsigned char *pubkey, size_t pklen,
                      const struct script *script_code,
                      uint32_t consensus_branch_id);
    bool (*check_lock_time)(const struct sig_checker *self, int64_t lock_time);
    bool (*verify_signature)(const struct sig_checker *self,
                             const unsigned char *sig, size_t siglen,
                             const unsigned char *pubkey, size_t pklen,
                             const struct uint256 *sighash);
    void *ctx;
};

bool eval_script(struct script_stack *stack,
                 const struct script *script,
                 unsigned int flags,
                 const struct sig_checker *checker,
                 uint32_t consensus_branch_id,
                 ScriptError *serror);

bool verify_script(const struct script *script_sig,
                   const struct script *script_pub_key,
                   unsigned int flags,
                   const struct sig_checker *checker,
                   uint32_t consensus_branch_id,
                   ScriptError *serror);

#endif
