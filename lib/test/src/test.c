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
    failures += test_bn254();
    failures += test_merkle_tree();
    failures += test_slp();
    failures += test_models();
    failures += test_core();
    failures += test_znam();
    failures += test_htlc();
    failures += test_file_market();
    failures += test_strong_params();
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
    failures += test_mmr();
    failures += test_mmb();
    failures += test_flyclient();
    failures += test_scan_util();
    failures += test_tor();
    failures += test_event();
    failures += test_download();
    failures += test_consensus();
    failures += test_policy();
    failures += test_wallet_view();
    failures += test_fast_sync();
    failures += test_block_scan();
    failures += test_node_health_service();
    failures += test_chain_state_repo();
    failures += test_recovery_policy();
    failures += test_db_txn();
    failures += test_sync_service();
    failures += test_snapshot_sync_service();
    failures += test_file_controller();
    failures += test_file_ops();
    failures += test_integrity();
    failures += test_protocols();
    failures += test_chain_restore_service();
    failures += test_chain_activation_controller();
    failures += test_mcp_router();
    failures += test_mcp_controllers();
    failures += test_mcp_middleware();
    failures += test_mcp_metrics();
    failures += test_mcp_e2e();
    failures += test_db_validators();
    failures += test_peer_scoring();
    failures += test_peer_bandwidth();
    failures += test_secrets_hygiene();
    failures += test_block_index_integrity();
    failures += test_wallet_backup();
    failures += test_log_json();
    failures += test_http_middleware();
    failures += test_rpc_timeout();
    failures += test_wallet_keystore();
    failures += test_wallet_sqlite_enc();
    { extern int test_watch_only(void); failures += test_watch_only(); }
    { extern int test_coin_selection(void); failures += test_coin_selection(); }
    failures += test_disk_monitor();
    failures += test_db_maintenance();
    failures += test_mempool_limits();
    failures += test_addrman_integrity();
    failures += test_ibd_throttle();
    failures += test_consensus_reject_events();
    failures += test_consensus_reject_index();
    failures += test_chain_rollback();
    failures += test_alerts();
    failures += test_ws_events();
    failures += test_trace();
    failures += test_phgr13_fix();
    failures += test_cookie_rotation();
    failures += test_reorg_safety();
    failures += test_key_scrub();
    failures += test_block_index_loader();
    failures += test_chain_state_validator();
    failures += test_utxo_recovery_service();
    failures += test_rpc_error_envelope();
    failures += test_tx_property();
    failures += test_workpool();
    failures += test_bip113_bip65();
    failures += test_mempool_orphan();
    failures += test_fee_estimation();
    failures += test_header_sync();
    failures += test_header_sync_stall();
    failures += test_hd_keychain();
    failures += test_mnemonic();
    failures += test_bip44();
    failures += test_compact_blocks();
    failures += test_dandelion();
    failures += test_addrman_rebalance();
    failures += test_block_pruning();
    failures += test_schema_migration();
    failures += test_multisig();
    failures += test_mcp_fuzz();
    failures += test_rpc_auth_hardening();
    failures += test_sync_watchdog();

    /* Spec-based user story tests */
    failures += spec_wallet_dashboard();
    failures += spec_wallet_send();
    failures += spec_wallet_receive();
    failures += spec_wallet_shield();
    failures += spec_wallet_node();
    failures += spec_wallet_history();
    failures += spec_wallet_coins();
    failures += spec_wallet_pulse();
    failures += spec_wallet_tx_detail();
    failures += spec_wallet_navigation();
    failures += spec_wallet_errors();
    failures += spec_wallet_privacy();
    failures += spec_wallet_sovereignty();
    failures += spec_wallet_celebration();
    failures += spec_wallet_empowerment();
    failures += spec_wallet_flow();
    failures += spec_wallet_accessibility();
    failures += spec_data_hooks();
    failures += spec_event_observers();
    failures += spec_state_machine();
    failures += spec_ux_sierra();
    failures += spec_html_quality();
    failures += spec_user_journeys();
    failures += spec_e2e_wallet();
    failures += spec_render_audit();
    failures += spec_smoke();
    failures += spec_100_stories();
    failures += spec_consensus_compat();

    ecc_verify_destroy();
    ecc_stop();

    printf("\n%s (%d failures)\n",
           failures ? "SOME TESTS FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}
