/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Main test runner for ZClassic C23 test suite. */

#include "test/test_helpers.h"
#include <signal.h>

/* Required by process_block.c (normally in main.c) */
volatile sig_atomic_t g_shutdown_requested = 0;

int main(void)
{
    setbuf(stdout, NULL); /* Unbuffered for test progress visibility */
    int failures = 0;

    /* Global init required by many test groups */
    chain_params_select(CHAIN_MAIN);
    ecc_start();
    ecc_verify_init();

    failures += test_load_balancer();
    failures += test_game();
    failures += test_crypto();
    failures += test_encoding();
    failures += test_chain();
    failures += test_keys();
    failures += test_script();
    failures += test_net();
    failures += test_transaction();
    failures += test_mempool();
    failures += test_rpc();
    failures += test_sqlite();
    failures += test_activerecord();
    failures += test_validation();
    failures += test_sapling();
    failures += test_sapling_crypto();
    failures += test_merkle_tree();
    failures += test_slp();
    failures += test_models();
    failures += test_core();
    failures += test_json();
    failures += test_robustness();
    failures += test_wallet();
    failures += test_primitives();
    failures += test_bloom();
    failures += test_coins();
    failures += test_store();
    failures += test_blog();
    failures += test_api();
    failures += test_explorer();
    failures += test_mining();
    failures += test_utxo_commitment();
    failures += test_scan_util();
    failures += test_tor();
    failures += test_event();
    failures += test_download();
    failures += test_consensus();
    failures += test_policy();
    failures += test_wallet_view();

    ecc_verify_destroy();
    ecc_stop();

    printf("\n%s (%d failures)\n",
           failures ? "SOME TESTS FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}
