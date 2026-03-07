/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_SIGOPS_C_H
#define ZCL_SIGOPS_C_H

#include "primitives/transaction_c.h"
#include <stdint.h>

uint64_t get_legacy_sig_op_count(const struct transaction *tx, uint32_t flags);

#endif
