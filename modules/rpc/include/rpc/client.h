/* Copyright (c) 2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_RPC_CLIENT_H
#define ZCL_RPC_CLIENT_H

#include "json/json.h"
#include <stdbool.h>
#include <stddef.h>

bool rpc_should_convert_param(const char *method, int param_idx);
bool rpc_convert_values(const char *method, const char **str_params,
                        size_t num_params, struct json_value *result);

#endif
