/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_DB_MODEL_SWAP_CONTRACT_H
#define ZCL_DB_MODEL_SWAP_CONTRACT_H

#include "models/database.h"
#include "models/activerecord.h"
#include "script/htlc.h"
#include <stdbool.h>

/* ActiveRecord model: SwapContract (atomic cross-chain HTLC)
 *
 * Record type is `struct swap_contract` from script/htlc.h. Validator
 * + callbacks live in app/models/src/swap_contract.c.
 *
 * This is cross-chain money — the validator is intentionally strict.
 * A malformed swap_contract that reaches the DB risks orphaning funds
 * on the counter-chain when the local node can't reconstruct the
 * redeem path.
 *
 * Validation (db_swap_contract_validate):
 *   - swap_id:           non-empty, hex chars only
 *   - role:              SWAP_INITIATOR or SWAP_PARTICIPANT
 *   - state:             SWAP_PENDING..SWAP_EXPIRED
 *   - chain:             SWAP_CHAIN_ZCL..SWAP_CHAIN_DOGE
 *   - secret_hash:       non-zero
 *   - secret:            non-zero if has_secret set
 *   - amount:            positive (no zero-value swaps)
 *   - locktime:          non-zero (must have a refund deadline)
 *   - my_address:        non-empty
 *   - counter_address:   non-empty
 *   - redeem_script_len: in [1, 256]
 *   - p2sh_address:      non-empty
 *   - created_at:        non-negative
 */

struct ar_callbacks *db_swap_contract_callbacks(void);
bool db_swap_contract_validate(const struct swap_contract *swap,
                               struct ar_errors *errors);

#endif
