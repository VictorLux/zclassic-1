/* Copyright (c) 2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef BITCOIN_TIMEDATA_H
#define BITCOIN_TIMEDATA_H

#include <stdint.h>

int64_t GetTimeOffset(void);
int64_t GetAdjustedTime(void);
void AddTimeData(const unsigned char *ip, int ip_len, int64_t nOffsetSample);

#endif
