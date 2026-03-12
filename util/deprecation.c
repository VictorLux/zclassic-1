/* Copyright (c) 2017 The Zcash developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "util/deprecation.h"
#include "util/clientversion.h"
#include "util/ui_interface.h"
#include "util/util.h"
#include <stdio.h>

void EnforceNodeDeprecation(int nHeight, bool forceLogging, bool fThread)
{
    (void)fThread;

    int blocksToDeprecation = DEPRECATION_HEIGHT - nHeight;
    if (blocksToDeprecation <= 0) {
        if (blocksToDeprecation == 0 || forceLogging) {
            LogPrintf("*** This version has been deprecated as of block height %d. "
                      "You should upgrade to the latest version of Zclassic.\n",
                      DEPRECATION_HEIGHT);
            if (uiInterface.ThreadSafeMessageBox)
                uiInterface.ThreadSafeMessageBox(
                    "This version has been deprecated. Please upgrade.",
                    "", UI_MSG_ERROR);
        }
    } else if (blocksToDeprecation == DEPRECATION_WARN_LIMIT ||
               (blocksToDeprecation < DEPRECATION_WARN_LIMIT && forceLogging)) {
        LogPrintf("*** This version will be deprecated at block height %d. "
                  "You should upgrade to the latest version of Zclassic.\n",
                  DEPRECATION_HEIGHT);
        if (uiInterface.ThreadSafeMessageBox)
            uiInterface.ThreadSafeMessageBox(
                "This version will be deprecated soon. Please upgrade.",
                "", UI_MSG_WARNING);
    }
}
