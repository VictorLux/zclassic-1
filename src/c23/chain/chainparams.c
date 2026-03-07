/* Copyright (c) 2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "chain/chainparams.h"
#include "encoding/utilstrencodings.h"
#include <assert.h>
#include <string.h>

static struct checkpoint_entry mainnet_checkpoints[] = {
    { 0,      {{0}} },
    { 30000,  {{0}} },
    { 160000, {{0}} },
    { 468200, {{0}} },
    { 2013514, {{0}} },
    { 2879438, {{0}} },
};

static struct checkpoint_entry testnet_checkpoints[] = {
    { 0, {{0}} },
};

static struct checkpoint_entry regtest_checkpoints[] = {
    { 0, {{0}} },
};

static struct chain_params mainParams;
static struct chain_params testNetParams;
static struct chain_params regTestParams;
static const struct chain_params *pCurrentParams = NULL;
static bool params_initialized = false;

static void init_main_params(void)
{
    struct chain_params *p = &mainParams;
    memset(p, 0, sizeof(*p));

    strcpy(p->strNetworkID, "main");
    strcpy(p->strCurrencyUnits, "ZCL");
    p->bip44CoinType = 147;

    p->consensus.fCoinbaseMustBeProtected = true;
    p->consensus.nSubsidySlowStartInterval = 2;
    p->consensus.nPreButtercupSubsidyHalvingInterval = PRE_BUTTERCUP_HALVING_INTERVAL;
    p->consensus.nPostButtercupSubsidyHalvingInterval = POST_BUTTERCUP_HALVING_INTERVAL;
    p->consensus.nMajorityEnforceBlockUpgrade = 750;
    p->consensus.nMajorityRejectBlockOutdated = 950;
    p->consensus.nMajorityWindow = 4000;
    uint256_set_hex(&p->consensus.powLimit,
        "0007ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
    p->consensus.nPowAveragingWindow = 17;
    p->consensus.nPowMaxAdjustDown = 32;
    p->consensus.nPowMaxAdjustUp = 16;
    p->consensus.nPreButtercupPowTargetSpacing = PRE_BUTTERCUP_POW_TARGET_SPACING;
    p->consensus.nPostButtercupPowTargetSpacing = POST_BUTTERCUP_POW_TARGET_SPACING;
    p->consensus.nPowAllowMinDifficultyBlocksAfterHeight = -1;
    p->consensus.nPowAllowMinDifficultyEnabled = false;
    p->consensus.scaleDifficultyAtUpgradeFork = true;

    p->consensus.vUpgrades[BASE_SPROUT].nProtocolVersion = 170002;
    p->consensus.vUpgrades[BASE_SPROUT].nActivationHeight = NETWORK_UPGRADE_ALWAYS_ACTIVE;
    p->consensus.vUpgrades[UPGRADE_TESTDUMMY].nProtocolVersion = 170002;
    p->consensus.vUpgrades[UPGRADE_TESTDUMMY].nActivationHeight = NETWORK_UPGRADE_NO_ACTIVATION;
    p->consensus.vUpgrades[UPGRADE_OVERWINTER].nProtocolVersion = 170005;
    p->consensus.vUpgrades[UPGRADE_OVERWINTER].nActivationHeight = 476969;
    p->consensus.vUpgrades[UPGRADE_SAPLING].nProtocolVersion = 170007;
    p->consensus.vUpgrades[UPGRADE_SAPLING].nActivationHeight = 476969;
    p->consensus.vUpgrades[UPGRADE_BUBBLES].nProtocolVersion = 170009;
    p->consensus.vUpgrades[UPGRADE_BUBBLES].nActivationHeight = 585318;
    p->consensus.vUpgrades[UPGRADE_DIFFADJ].nProtocolVersion = 170010;
    p->consensus.vUpgrades[UPGRADE_DIFFADJ].nActivationHeight = 585322;
    p->consensus.vUpgrades[UPGRADE_BUTTERCUP].nProtocolVersion = 170011;
    p->consensus.vUpgrades[UPGRADE_BUTTERCUP].nActivationHeight = 707000;

    uint256_set_hex(&p->consensus.nMinimumChainWork,
        "000000000000000000000000000000000000000000000000000af996bfd8e482");

    p->pchMessageStart[0] = 0x24;
    p->pchMessageStart[1] = 0xe9;
    p->pchMessageStart[2] = 0x27;
    p->pchMessageStart[3] = 0x64;

    p->nDefaultPort = 8033;
    p->nPruneAfterHeight = 100000;
    p->nEquihashN = 200;
    p->nEquihashK = 9;

    uint256_set_hex(&p->consensus.hashGenesisBlock,
        "0007104ccda289427919efc39dc9e4d499804b7bebc22df55f8b834301260602");

    p->vSeeds[0] = (struct dns_seed){ "zclnet.net", "dnsseed.zclnet.net" };
    p->vSeeds[1] = (struct dns_seed){ "zslp.org", "dnsseed.zslp.org" };
    p->nSeeds = 2;

    /* t1 addresses */
    p->base58Prefixes[B58_PUBKEY_ADDRESS][0] = 0x1C;
    p->base58Prefixes[B58_PUBKEY_ADDRESS][1] = 0xB8;
    p->base58PrefixLengths[B58_PUBKEY_ADDRESS] = 2;
    /* t3 addresses */
    p->base58Prefixes[B58_SCRIPT_ADDRESS][0] = 0x1C;
    p->base58Prefixes[B58_SCRIPT_ADDRESS][1] = 0xBD;
    p->base58PrefixLengths[B58_SCRIPT_ADDRESS] = 2;
    /* 5/K/L WIF */
    p->base58Prefixes[B58_SECRET_KEY][0] = 0x80;
    p->base58PrefixLengths[B58_SECRET_KEY] = 1;
    /* BIP32 xpub */
    p->base58Prefixes[B58_EXT_PUBLIC_KEY][0] = 0x04;
    p->base58Prefixes[B58_EXT_PUBLIC_KEY][1] = 0x88;
    p->base58Prefixes[B58_EXT_PUBLIC_KEY][2] = 0xB2;
    p->base58Prefixes[B58_EXT_PUBLIC_KEY][3] = 0x1E;
    p->base58PrefixLengths[B58_EXT_PUBLIC_KEY] = 4;
    /* BIP32 xprv */
    p->base58Prefixes[B58_EXT_SECRET_KEY][0] = 0x04;
    p->base58Prefixes[B58_EXT_SECRET_KEY][1] = 0x88;
    p->base58Prefixes[B58_EXT_SECRET_KEY][2] = 0xAD;
    p->base58Prefixes[B58_EXT_SECRET_KEY][3] = 0xE4;
    p->base58PrefixLengths[B58_EXT_SECRET_KEY] = 4;
    /* zc payment address */
    p->base58Prefixes[B58_ZCPAYMENT_ADDRESS][0] = 0x16;
    p->base58Prefixes[B58_ZCPAYMENT_ADDRESS][1] = 0x9A;
    p->base58PrefixLengths[B58_ZCPAYMENT_ADDRESS] = 2;
    /* SK spending key */
    p->base58Prefixes[B58_ZCSPENDING_KEY][0] = 0xAB;
    p->base58Prefixes[B58_ZCSPENDING_KEY][1] = 0x36;
    p->base58PrefixLengths[B58_ZCSPENDING_KEY] = 2;
    /* ZiVK viewing key */
    p->base58Prefixes[B58_ZCVIEWING_KEY][0] = 0xA8;
    p->base58Prefixes[B58_ZCVIEWING_KEY][1] = 0xAB;
    p->base58Prefixes[B58_ZCVIEWING_KEY][2] = 0xD3;
    p->base58PrefixLengths[B58_ZCVIEWING_KEY] = 3;

    strcpy(p->bech32HRPs[BECH32_SAPLING_PAYMENT_ADDRESS], "zs");
    strcpy(p->bech32HRPs[BECH32_SAPLING_FULL_VIEWING_KEY], "zviews");
    strcpy(p->bech32HRPs[BECH32_SAPLING_INCOMING_VIEWING_KEY], "zivks");
    strcpy(p->bech32HRPs[BECH32_SAPLING_EXTENDED_SPEND_KEY], "secret-extended-key-main");

    p->fMiningRequiresPeers = true;
    p->fDefaultConsistencyChecks = false;
    p->fRequireStandard = true;
    p->fMineBlocksOnDemand = false;
    p->fTestnetToBeDeprecatedFieldRPC = false;

    uint256_set_hex(&mainnet_checkpoints[0].hash,
        "0007104ccda289427919efc39dc9e4d499804b7bebc22df55f8b834301260602");
    uint256_set_hex(&mainnet_checkpoints[1].hash,
        "000000005c2ad200c3c7c8e627f67b306659efca1268c9bb014335fdadc0c392");
    uint256_set_hex(&mainnet_checkpoints[2].hash,
        "000000065093005a1a46ee95d6d66c2b07008220ca64dd3b3a93bbd1945480c0");
    uint256_set_hex(&mainnet_checkpoints[3].hash,
        "000000009bd5548c851c2b237894d6807a53bf1e2808402545e27a995ae4f3c3");
    uint256_set_hex(&mainnet_checkpoints[4].hash,
        "000019679aa2ea97a3f18bd9265bc91a09929ea0b1acc0fc5ef77cdf3cf906e7");
    uint256_set_hex(&mainnet_checkpoints[5].hash,
        "000007e8fccb9e4831c7d7376a283b016ead6166491f951f4f083dbe366992b2");

    p->checkpointData.entries = mainnet_checkpoints;
    p->checkpointData.nEntries = 6;
    p->checkpointData.nTimeLastCheckpoint = 1729305135;
    p->checkpointData.nTransactionsLastCheckpoint = 5293850;
    p->checkpointData.fTransactionsPerDay = 1060;

    static const char *main_founders[] = {
        "t3Vz22vK5z2LcKEdg16Yv4FFneEL1zg9ojd",
        "t3cL9AucCajm3HXDhb5jBnJK2vapVoXsop3",
        "t3fqvkzrrNaMcamkQMwAyHRjfDdM2xQvDTR",
        "t3TgZ9ZT2CTSK44AnUPi6qeNaHa2eC7pUyF",
        "t3SpkcPQPfuRYHsP5vz3Pv86PgKo5m9KVmx",
        "t3Xt4oQMRPagwbpQqkgAViQgtST4VoSWR6S",
        "t3ayBkZ4w6kKXynwoHZFUSSgXRKtogTXNgb",
        "t3adJBQuaa21u7NxbR8YMzp3km3TbSZ4MGB",
        "t3K4aLYagSSBySdrfAGGeUd5H9z5Qvz88t2",
        "t3RYnsc5nhEvKiva3ZPhfRSk7eyh1CrA6Rk",
        "t3Ut4KUq2ZSMTPNE67pBU5LqYCi2q36KpXQ",
        "t3ZnCNAvgu6CSyHm1vWtrx3aiN98dSAGpnD",
        "t3fB9cB3eSYim64BS9xfwAHQUKLgQQroBDG",
        "t3cwZfKNNj2vXMAHBQeewm6pXhKFdhk18kD",
        "t3YcoujXfspWy7rbNUsGKxFEWZqNstGpeG4",
        "t3bLvCLigc6rbNrUTS5NwkgyVrZcZumTRa4",
        "t3VvHWa7r3oy67YtU4LZKGCWa2J6eGHvShi",
        "t3eF9X6X2dSo7MCvTjfZEzwWrVzquxRLNeY",
        "t3esCNwwmcyc8i9qQfyTbYhTqmYXZ9AwK3X",
        "t3M4jN7hYE2e27yLsuQPPjuVek81WV3VbBj",
        "t3gGWxdC67CYNoBbPjNvrrWLAWxPqZLxrVY",
        "t3LTWeoxeWPbmdkUD3NWBquk4WkazhFBmvU",
        "t3P5KKX97gXYFSaSjJPiruQEX84yF5z3Tjq",
        "t3f3T3nCWsEpzmD35VK62JgQfFig74dV8C9",
        "t3Rqonuzz7afkF7156ZA4vi4iimRSEn41hj",
        "t3fJZ5jYsyxDtvNrWBeoMbvJaQCj4JJgbgX",
        "t3Pnbg7XjP7FGPBUuz75H65aczphHgkpoJW",
        "t3WeKQDxCijL5X7rwFem1MTL9ZwVJkUFhpF",
        "t3Y9FNi26J7UtAUC4moaETLbMo8KS1Be6ME",
        "t3aNRLLsL2y8xcjPheZZwFy3Pcv7CsTwBec",
        "t3gQDEavk5VzAAHK8TrQu2BWDLxEiF1unBm",
        "t3Rbykhx1TUFrgXrmBYrAJe2STxRKFL7G9r",
        "t3aaW4aTdP7a8d1VTE1Bod2yhbeggHgMajR",
        "t3YEiAa6uEjXwFL2v5ztU1fn3yKgzMQqNyo",
        "t3g1yUUwt2PbmDvMDevTCPWUcbDatL2iQGP",
        "t3dPWnep6YqGPuY1CecgbeZrY9iUwH8Yd4z",
        "t3QRZXHDPh2hwU46iQs2776kRuuWfwFp4dV",
        "t3enhACRxi1ZD7e8ePomVGKn7wp7N9fFJ3r",
        "t3PkLgT71TnF112nSwBToXsD77yNbx2gJJY",
        "t3LQtHUDoe7ZhhvddRv4vnaoNAhCr2f4oFN",
        "t3fNcdBUbycvbCtsD2n9q3LuxG7jVPvFB8L",
        "t3dKojUU2EMjs28nHV84TvkVEUDu1M1FaEx",
        "t3aKH6NiWN1ofGd8c19rZiqgYpkJ3n679ME",
        "t3MEXDF9Wsi63KwpPuQdD6by32Mw2bNTbEa",
        "t3WDhPfik343yNmPTqtkZAoQZeqA83K7Y3f",
        "t3PSn5TbMMAEw7Eu36DYctFezRzpX1hzf3M",
        "t3R3Y5vnBLrEn8L6wFjPjBLnxSUQsKnmFpv",
        "t3Pcm737EsVkGTbhsu2NekKtJeG92mvYyoN",
    };
    p->nFoundersRewardAddresses = 48;
    for (size_t i = 0; i < 48; i++)
        strcpy(p->vFoundersRewardAddress[i], main_founders[i]);
}

static void init_test_params(void)
{
    struct chain_params *p = &testNetParams;
    memset(p, 0, sizeof(*p));

    strcpy(p->strNetworkID, "test");
    strcpy(p->strCurrencyUnits, "ZCT");
    p->bip44CoinType = 1;

    p->consensus.fCoinbaseMustBeProtected = true;
    p->consensus.nSubsidySlowStartInterval = 2;
    p->consensus.nPreButtercupSubsidyHalvingInterval = PRE_BUTTERCUP_HALVING_INTERVAL;
    p->consensus.nPostButtercupSubsidyHalvingInterval = POST_BUTTERCUP_HALVING_INTERVAL;
    p->consensus.nMajorityEnforceBlockUpgrade = 51;
    p->consensus.nMajorityRejectBlockOutdated = 75;
    p->consensus.nMajorityWindow = 400;
    uint256_set_hex(&p->consensus.powLimit,
        "07ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
    p->consensus.nPowAveragingWindow = 17;
    p->consensus.nPowMaxAdjustDown = 32;
    p->consensus.nPowMaxAdjustUp = 16;
    p->consensus.nPreButtercupPowTargetSpacing = PRE_BUTTERCUP_POW_TARGET_SPACING;
    p->consensus.nPostButtercupPowTargetSpacing = POST_BUTTERCUP_POW_TARGET_SPACING;
    p->consensus.nPowAllowMinDifficultyBlocksAfterHeight = 299187;
    p->consensus.nPowAllowMinDifficultyEnabled = true;
    p->consensus.scaleDifficultyAtUpgradeFork = false;

    p->consensus.vUpgrades[BASE_SPROUT].nProtocolVersion = 170002;
    p->consensus.vUpgrades[BASE_SPROUT].nActivationHeight = NETWORK_UPGRADE_ALWAYS_ACTIVE;
    p->consensus.vUpgrades[UPGRADE_TESTDUMMY].nProtocolVersion = 170002;
    p->consensus.vUpgrades[UPGRADE_TESTDUMMY].nActivationHeight = NETWORK_UPGRADE_NO_ACTIVATION;
    p->consensus.vUpgrades[UPGRADE_OVERWINTER].nProtocolVersion = 170003;
    p->consensus.vUpgrades[UPGRADE_OVERWINTER].nActivationHeight = 20;
    p->consensus.vUpgrades[UPGRADE_SAPLING].nProtocolVersion = 170007;
    p->consensus.vUpgrades[UPGRADE_SAPLING].nActivationHeight = 20;
    p->consensus.vUpgrades[UPGRADE_BUBBLES].nProtocolVersion = 170008;
    p->consensus.vUpgrades[UPGRADE_BUBBLES].nActivationHeight = 6350;
    p->consensus.vUpgrades[UPGRADE_DIFFADJ].nProtocolVersion = 170009;
    p->consensus.vUpgrades[UPGRADE_DIFFADJ].nActivationHeight = NETWORK_UPGRADE_NO_ACTIVATION;
    p->consensus.vUpgrades[UPGRADE_BUTTERCUP].nProtocolVersion = 170010;
    p->consensus.vUpgrades[UPGRADE_BUTTERCUP].nActivationHeight = 78856;

    uint256_set_hex(&p->consensus.nMinimumChainWork,
        "0000000000000000000000000000000000000000000000000000000000000000");

    p->pchMessageStart[0] = 0xfa;
    p->pchMessageStart[1] = 0x1a;
    p->pchMessageStart[2] = 0xf9;
    p->pchMessageStart[3] = 0xbf;

    p->nDefaultPort = 18033;
    p->nPruneAfterHeight = 1000;
    p->nEquihashN = 200;
    p->nEquihashK = 9;

    uint256_set_hex(&p->consensus.hashGenesisBlock,
        "03e1c4bb705c871bf9bfda3e74b7f8f86bff267993c215a89d5795e3708e5e1f");

    p->vSeeds[0] = (struct dns_seed){ "testnet_node1", "167.71.172.5" };
    p->nSeeds = 1;

    /* tm addresses */
    p->base58Prefixes[B58_PUBKEY_ADDRESS][0] = 0x1D;
    p->base58Prefixes[B58_PUBKEY_ADDRESS][1] = 0x25;
    p->base58PrefixLengths[B58_PUBKEY_ADDRESS] = 2;
    /* t2 addresses */
    p->base58Prefixes[B58_SCRIPT_ADDRESS][0] = 0x1C;
    p->base58Prefixes[B58_SCRIPT_ADDRESS][1] = 0xBA;
    p->base58PrefixLengths[B58_SCRIPT_ADDRESS] = 2;
    /* 9/c WIF */
    p->base58Prefixes[B58_SECRET_KEY][0] = 0xEF;
    p->base58PrefixLengths[B58_SECRET_KEY] = 1;
    /* BIP32 tpub */
    p->base58Prefixes[B58_EXT_PUBLIC_KEY][0] = 0x04;
    p->base58Prefixes[B58_EXT_PUBLIC_KEY][1] = 0x35;
    p->base58Prefixes[B58_EXT_PUBLIC_KEY][2] = 0x87;
    p->base58Prefixes[B58_EXT_PUBLIC_KEY][3] = 0xCF;
    p->base58PrefixLengths[B58_EXT_PUBLIC_KEY] = 4;
    /* BIP32 tprv */
    p->base58Prefixes[B58_EXT_SECRET_KEY][0] = 0x04;
    p->base58Prefixes[B58_EXT_SECRET_KEY][1] = 0x35;
    p->base58Prefixes[B58_EXT_SECRET_KEY][2] = 0x83;
    p->base58Prefixes[B58_EXT_SECRET_KEY][3] = 0x94;
    p->base58PrefixLengths[B58_EXT_SECRET_KEY] = 4;
    /* zt payment address */
    p->base58Prefixes[B58_ZCPAYMENT_ADDRESS][0] = 0x16;
    p->base58Prefixes[B58_ZCPAYMENT_ADDRESS][1] = 0xB6;
    p->base58PrefixLengths[B58_ZCPAYMENT_ADDRESS] = 2;
    /* ST spending key */
    p->base58Prefixes[B58_ZCSPENDING_KEY][0] = 0xAC;
    p->base58Prefixes[B58_ZCSPENDING_KEY][1] = 0x08;
    p->base58PrefixLengths[B58_ZCSPENDING_KEY] = 2;
    /* ZiVt viewing key */
    p->base58Prefixes[B58_ZCVIEWING_KEY][0] = 0xA8;
    p->base58Prefixes[B58_ZCVIEWING_KEY][1] = 0xAC;
    p->base58Prefixes[B58_ZCVIEWING_KEY][2] = 0x0C;
    p->base58PrefixLengths[B58_ZCVIEWING_KEY] = 3;

    strcpy(p->bech32HRPs[BECH32_SAPLING_PAYMENT_ADDRESS], "ztestsapling");
    strcpy(p->bech32HRPs[BECH32_SAPLING_FULL_VIEWING_KEY], "zviewtestsapling");
    strcpy(p->bech32HRPs[BECH32_SAPLING_INCOMING_VIEWING_KEY], "zivktestsapling");
    strcpy(p->bech32HRPs[BECH32_SAPLING_EXTENDED_SPEND_KEY], "secret-extended-key-test");

    p->fMiningRequiresPeers = true;
    p->fDefaultConsistencyChecks = false;
    p->fRequireStandard = true;
    p->fMineBlocksOnDemand = false;
    p->fTestnetToBeDeprecatedFieldRPC = true;

    uint256_set_hex(&testnet_checkpoints[0].hash,
        "03e1c4bb705c871bf9bfda3e74b7f8f86bff267993c215a89d5795e3708e5e1f");

    p->checkpointData.entries = testnet_checkpoints;
    p->checkpointData.nEntries = 1;
    p->checkpointData.nTimeLastCheckpoint = 0;
    p->checkpointData.nTransactionsLastCheckpoint = 0;
    p->checkpointData.fTransactionsPerDay = 0;

    p->nFoundersRewardAddresses = 0;
}

static void init_regtest_params(void)
{
    struct chain_params *p = &regTestParams;
    memset(p, 0, sizeof(*p));

    strcpy(p->strNetworkID, "regtest");
    strcpy(p->strCurrencyUnits, "REG");
    p->bip44CoinType = 1;

    p->consensus.fCoinbaseMustBeProtected = false;
    p->consensus.nSubsidySlowStartInterval = 0;
    p->consensus.nPreButtercupSubsidyHalvingInterval = PRE_BUTTERCUP_REGTEST_HALVING_INTERVAL;
    p->consensus.nPostButtercupSubsidyHalvingInterval = POST_BUTTERCUP_REGTEST_HALVING_INTERVAL;
    p->consensus.nMajorityEnforceBlockUpgrade = 750;
    p->consensus.nMajorityRejectBlockOutdated = 950;
    p->consensus.nMajorityWindow = 1000;
    uint256_set_hex(&p->consensus.powLimit,
        "0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f");
    p->consensus.nPowAveragingWindow = 17;
    p->consensus.nPowMaxAdjustDown = 0;
    p->consensus.nPowMaxAdjustUp = 0;
    p->consensus.nPreButtercupPowTargetSpacing = PRE_BUTTERCUP_POW_TARGET_SPACING;
    p->consensus.nPostButtercupPowTargetSpacing = POST_BUTTERCUP_POW_TARGET_SPACING;
    p->consensus.nPowAllowMinDifficultyBlocksAfterHeight = 0;
    p->consensus.nPowAllowMinDifficultyEnabled = true;
    p->consensus.scaleDifficultyAtUpgradeFork = false;

    p->consensus.vUpgrades[BASE_SPROUT].nProtocolVersion = 170002;
    p->consensus.vUpgrades[BASE_SPROUT].nActivationHeight = NETWORK_UPGRADE_ALWAYS_ACTIVE;
    p->consensus.vUpgrades[UPGRADE_TESTDUMMY].nProtocolVersion = 170002;
    p->consensus.vUpgrades[UPGRADE_TESTDUMMY].nActivationHeight = NETWORK_UPGRADE_NO_ACTIVATION;
    p->consensus.vUpgrades[UPGRADE_OVERWINTER].nProtocolVersion = 170003;
    p->consensus.vUpgrades[UPGRADE_OVERWINTER].nActivationHeight = NETWORK_UPGRADE_NO_ACTIVATION;
    p->consensus.vUpgrades[UPGRADE_SAPLING].nProtocolVersion = 170006;
    p->consensus.vUpgrades[UPGRADE_SAPLING].nActivationHeight = NETWORK_UPGRADE_NO_ACTIVATION;
    p->consensus.vUpgrades[UPGRADE_BUBBLES].nProtocolVersion = 170008;
    p->consensus.vUpgrades[UPGRADE_BUBBLES].nActivationHeight = NETWORK_UPGRADE_NO_ACTIVATION;
    p->consensus.vUpgrades[UPGRADE_DIFFADJ].nProtocolVersion = 170009;
    p->consensus.vUpgrades[UPGRADE_DIFFADJ].nActivationHeight = NETWORK_UPGRADE_NO_ACTIVATION;
    p->consensus.vUpgrades[UPGRADE_BUTTERCUP].nProtocolVersion = 170010;
    p->consensus.vUpgrades[UPGRADE_BUTTERCUP].nActivationHeight = NETWORK_UPGRADE_NO_ACTIVATION;

    uint256_set_hex(&p->consensus.nMinimumChainWork,
        "0000000000000000000000000000000000000000000000000000000000000000");

    p->pchMessageStart[0] = 0xaa;
    p->pchMessageStart[1] = 0xe8;
    p->pchMessageStart[2] = 0x3f;
    p->pchMessageStart[3] = 0x5f;

    p->nDefaultPort = 18033;
    p->nPruneAfterHeight = 1000;
    p->nEquihashN = 48;
    p->nEquihashK = 5;

    uint256_set_hex(&p->consensus.hashGenesisBlock,
        "0575f78ee8dc057deee78ef691876e3be29833aaee5e189bb0459c087451305a");

    p->nSeeds = 0;

    /* Same as testnet prefixes */
    p->base58Prefixes[B58_PUBKEY_ADDRESS][0] = 0x1D;
    p->base58Prefixes[B58_PUBKEY_ADDRESS][1] = 0x25;
    p->base58PrefixLengths[B58_PUBKEY_ADDRESS] = 2;
    p->base58Prefixes[B58_SCRIPT_ADDRESS][0] = 0x1C;
    p->base58Prefixes[B58_SCRIPT_ADDRESS][1] = 0xBA;
    p->base58PrefixLengths[B58_SCRIPT_ADDRESS] = 2;
    p->base58Prefixes[B58_SECRET_KEY][0] = 0xEF;
    p->base58PrefixLengths[B58_SECRET_KEY] = 1;
    p->base58Prefixes[B58_EXT_PUBLIC_KEY][0] = 0x04;
    p->base58Prefixes[B58_EXT_PUBLIC_KEY][1] = 0x35;
    p->base58Prefixes[B58_EXT_PUBLIC_KEY][2] = 0x87;
    p->base58Prefixes[B58_EXT_PUBLIC_KEY][3] = 0xCF;
    p->base58PrefixLengths[B58_EXT_PUBLIC_KEY] = 4;
    p->base58Prefixes[B58_EXT_SECRET_KEY][0] = 0x04;
    p->base58Prefixes[B58_EXT_SECRET_KEY][1] = 0x35;
    p->base58Prefixes[B58_EXT_SECRET_KEY][2] = 0x83;
    p->base58Prefixes[B58_EXT_SECRET_KEY][3] = 0x94;
    p->base58PrefixLengths[B58_EXT_SECRET_KEY] = 4;
    p->base58Prefixes[B58_ZCPAYMENT_ADDRESS][0] = 0x16;
    p->base58Prefixes[B58_ZCPAYMENT_ADDRESS][1] = 0xB6;
    p->base58PrefixLengths[B58_ZCPAYMENT_ADDRESS] = 2;
    p->base58Prefixes[B58_ZCSPENDING_KEY][0] = 0xAC;
    p->base58Prefixes[B58_ZCSPENDING_KEY][1] = 0x08;
    p->base58PrefixLengths[B58_ZCSPENDING_KEY] = 2;
    p->base58Prefixes[B58_ZCVIEWING_KEY][0] = 0xA8;
    p->base58Prefixes[B58_ZCVIEWING_KEY][1] = 0xAC;
    p->base58Prefixes[B58_ZCVIEWING_KEY][2] = 0x0C;
    p->base58PrefixLengths[B58_ZCVIEWING_KEY] = 3;

    strcpy(p->bech32HRPs[BECH32_SAPLING_PAYMENT_ADDRESS], "zregtestsapling");
    strcpy(p->bech32HRPs[BECH32_SAPLING_FULL_VIEWING_KEY], "zviewregtestsapling");
    strcpy(p->bech32HRPs[BECH32_SAPLING_INCOMING_VIEWING_KEY], "zivkregtestsapling");
    strcpy(p->bech32HRPs[BECH32_SAPLING_EXTENDED_SPEND_KEY], "secret-extended-key-regtest");

    p->fMiningRequiresPeers = false;
    p->fDefaultConsistencyChecks = true;
    p->fRequireStandard = false;
    p->fMineBlocksOnDemand = true;
    p->fTestnetToBeDeprecatedFieldRPC = false;

    uint256_set_hex(&regtest_checkpoints[0].hash,
        "0575f78ee8dc057deee78ef691876e3be29833aaee5e189bb0459c087451305a");

    p->checkpointData.entries = regtest_checkpoints;
    p->checkpointData.nEntries = 1;
    p->checkpointData.nTimeLastCheckpoint = 0;
    p->checkpointData.nTransactionsLastCheckpoint = 0;
    p->checkpointData.fTransactionsPerDay = 0;

    p->nFoundersRewardAddresses = 1;
    strcpy(p->vFoundersRewardAddress[0], "t2FwcEhFdNXuFMv1tcYwaBJtYVtMj8b1uTg");
}

static void ensure_initialized(void)
{
    if (!params_initialized) {
        init_main_params();
        init_test_params();
        init_regtest_params();
        params_initialized = true;
    }
}

const struct chain_params *chain_params_get(void)
{
    ensure_initialized();
    assert(pCurrentParams);
    return pCurrentParams;
}

void chain_params_select(enum chain_network network)
{
    ensure_initialized();
    SelectBaseParams(network);
    switch (network) {
    case CHAIN_MAIN:    pCurrentParams = &mainParams; break;
    case CHAIN_TESTNET: pCurrentParams = &testNetParams; break;
    case CHAIN_REGTEST: pCurrentParams = &regTestParams; break;
    default: assert(false); break;
    }
}

const unsigned char *chain_params_base58_prefix(const struct chain_params *p,
                                                 enum base58_type type,
                                                 size_t *len_out)
{
    *len_out = p->base58PrefixLengths[type];
    return p->base58Prefixes[type];
}
