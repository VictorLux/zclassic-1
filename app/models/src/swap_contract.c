/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ActiveRecord model: SwapContract (atomic cross-chain HTLC)
 *
 * Owns the integrity contract for the `zswp_contracts` table. The
 * persistence SQL lives in lib/script/src/htlc.c; this file ensures
 * every write through db_swap_save satisfies invariants before the
 * row hits sqlite. */

#include "models/swap_contract.h"
#include <ctype.h>
#include <string.h>

DEFINE_MODEL_CALLBACKS(swap_contract)

static bool is_lowercase_hex(const char *s)
{
    if (!s) return false;
    for (; *s; ++s)
        if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f')))
            return false;
    return true;
}

bool db_swap_contract_validate(const struct swap_contract *swap,
                               struct ar_errors *errors)
{
    ar_errors_clear(errors);
    if (!swap) {
        ar_errors_add(errors, "swap", "is NULL");
        return false;
    }

    static const uint8_t zero32[32] = {0};

    validates_presence_of(errors, swap, swap_id);
    if (swap->swap_id[0]) {
        validates_custom(errors,
            is_lowercase_hex(swap->swap_id),
            "swap_id", "must be lowercase hex");
    }
    static const enum swap_role valid_roles[] = {
        SWAP_INITIATOR, SWAP_PARTICIPANT
    };
    static const enum swap_state valid_states[] = {
        SWAP_PENDING, SWAP_FUNDED, SWAP_REDEEMED, SWAP_REFUNDED, SWAP_EXPIRED
    };
    static const enum swap_chain valid_chains[] = {
        SWAP_CHAIN_ZCL, SWAP_CHAIN_BTC, SWAP_CHAIN_LTC, SWAP_CHAIN_DOGE
    };
    validates_inclusion_of(errors, swap, role,  valid_roles,  2);
    validates_inclusion_of(errors, swap, state, valid_states, 5);
    validates_inclusion_of(errors, swap, chain, valid_chains, 4);
    validates_custom(errors,
        memcmp(swap->secret_hash, zero32, 32) != 0,
        "secret_hash", "can't be all zero");
    if (swap->has_secret) {
        validates_custom(errors,
            memcmp(swap->secret, zero32, 32) != 0,
            "secret", "can't be all zero when has_secret is true");
    }
    validates_positive(errors, swap, amount);
    validates_not_zero(errors, swap, locktime);
    validates_presence_of(errors, swap, my_address);
    validates_presence_of(errors, swap, counter_address);
    validates_range(errors, swap, redeem_script_len, 1, 256);
    validates_presence_of(errors, swap, p2sh_address);
    validates_non_negative(errors, swap, created_at);

    return !ar_errors_any(errors);
}
