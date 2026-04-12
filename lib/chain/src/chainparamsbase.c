/* Copyright (c) 2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "chain/chainparamsbase.h"
#include "util/log_macros.h"
#include <assert.h>
#include <stddef.h>

/* GetBoolArg is defined in util.c */
extern bool GetBoolArg(const char *arg, bool default_val);

static struct base_chain_params mainParams = { 8023, "" };
static struct base_chain_params testNetParams = { 18023, "testnet3" };
static struct base_chain_params regTestParams = { 18023, "regtest" };

static const struct base_chain_params *pCurrentBaseParams = NULL;

const struct base_chain_params *BaseParams(void)
{
    assert(pCurrentBaseParams);
    return pCurrentBaseParams;
}

void SelectBaseParams(enum chain_network network)
{
    switch (network) {
    case CHAIN_MAIN:
        pCurrentBaseParams = &mainParams;
        break;
    case CHAIN_TESTNET:
        pCurrentBaseParams = &testNetParams;
        break;
    case CHAIN_REGTEST:
        pCurrentBaseParams = &regTestParams;
        break;
    default:
        assert(0 && "Unimplemented network");
        return;
    }
}

enum chain_network NetworkIdFromCommandLine(void)
{
    bool fRegTest = GetBoolArg("-regtest", false);
    bool fTestNet = GetBoolArg("-testnet", false);

    if (fTestNet && fRegTest)
        return CHAIN_MAX_NETWORK_TYPES;
    if (fRegTest)
        return CHAIN_REGTEST;
    if (fTestNet)
        return CHAIN_TESTNET;
    return CHAIN_MAIN;
}

bool SelectBaseParamsFromCommandLine(void)
{
    enum chain_network network = NetworkIdFromCommandLine();
    if (network == CHAIN_MAX_NETWORK_TYPES)
        LOG_FAIL("chainparams", "conflicting -testnet and -regtest flags");
    SelectBaseParams(network);
    return true;
}

bool AreBaseParamsConfigured(void)
{
    return pCurrentBaseParams != NULL;
}
