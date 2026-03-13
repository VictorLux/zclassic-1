/* Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "chain/checkpoints.h"
#include <time.h>

static const double SIGCHECK_VERIFICATION_FACTOR = 5.0;

int checkpoints_get_total_blocks_estimate(const struct checkpoint_data *data)
{
    if (data->nEntries == 0)
        return 0;
    return data->entries[data->nEntries - 1].height;
}

double checkpoints_guess_verification_progress(
    const struct checkpoint_data *data,
    const struct block_index *pindex, bool fSigchecks)
{
    if (pindex == NULL)
        return 0.0;

    int64_t nNow = time(NULL);
    double fSigcheckFactor = fSigchecks ? SIGCHECK_VERIFICATION_FACTOR : 1.0;
    double fWorkBefore = 0.0;
    double fWorkAfter = 0.0;

    if ((int64_t)pindex->nChainTx <= data->nTransactionsLastCheckpoint) {
        double nCheapBefore = (double)pindex->nChainTx;
        double nCheapAfter = (double)data->nTransactionsLastCheckpoint - nCheapBefore;
        double nExpensiveAfter = (double)(nNow - data->nTimeLastCheckpoint) /
                                 86400.0 * data->fTransactionsPerDay;
        fWorkBefore = nCheapBefore;
        fWorkAfter = nCheapAfter + nExpensiveAfter * fSigcheckFactor;
    } else {
        double nCheapBefore = (double)data->nTransactionsLastCheckpoint;
        double nExpensiveBefore = (double)pindex->nChainTx - nCheapBefore;
        double nExpensiveAfter = (double)(nNow - block_index_get_time(pindex)) /
                                 86400.0 * data->fTransactionsPerDay;
        fWorkBefore = nCheapBefore + nExpensiveBefore * fSigcheckFactor;
        fWorkAfter = nExpensiveAfter * fSigcheckFactor;
    }

    return fWorkBefore / (fWorkBefore + fWorkAfter);
}
