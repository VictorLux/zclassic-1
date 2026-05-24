/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_VALIDATION_UPDATE_COINS_H
#define ZCL_VALIDATION_UPDATE_COINS_H

#include "coins/coins_view.h"
#include "coins/undo.h"
#include "primitives/transaction.h"

#include <stdint.h>

/* Returns false on UTXO corruption (missing inputs, invalid values).
 * Caller MUST check return and reject the block on failure. */
bool update_coins_with_undo(const struct transaction *tx,
                            struct coins_view_cache *inputs,
                            struct tx_undo *txundo,
                            int nHeight);

void update_coins(const struct transaction *tx,
                  struct coins_view_cache *inputs,
                  int nHeight);

/* Phase 4b shadow-emission counters. Reset on process restart. The
 * `total` covers successful EV_UTXO_ADD + EV_UTXO_SPEND emissions; the
 * `fail` total only counts failures when shadow mode is actually
 * enabled (utxo_projection_event_log() non-NULL). Operators compare
 * these against the projection-side counters in zcl_state
 * subsystem=utxo_projection to detect drift between emit and consume. */
uint64_t update_coins_event_emit_total(void);
uint64_t update_coins_event_emit_fail_total(void);

#endif
