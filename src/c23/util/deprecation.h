/* Copyright (c) 2017 The Zcash developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCASH_DEPRECATION_H
#define ZCASH_DEPRECATION_H

#include <stdbool.h>

#define APPROX_RELEASE_HEIGHT  99235543
#define WEEKS_UNTIL_DEPRECATION 70
#define DEPRECATION_HEIGHT (APPROX_RELEASE_HEIGHT + (WEEKS_UNTIL_DEPRECATION * 7 * 24 * 24))
#define DEPRECATION_WARN_LIMIT (14 * 24 * 24)

void EnforceNodeDeprecation(int nHeight, bool forceLogging, bool fThread);

#endif
