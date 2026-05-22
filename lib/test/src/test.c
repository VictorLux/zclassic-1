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

    /* Developer-only fast loop: ZCL_TEST_ONLY=persistence runs only the
     * persistence-layer regression tests (agent-2 scope) so an iteration
     * doesn't have to wait for the entire 1500-test suite.  Unset or
     * unknown value runs the full suite unchanged. */
    const char *only = getenv("ZCL_TEST_ONLY");
    if (only && strcmp(only, "onion") == 0) {
        printf("[test] ZCL_TEST_ONLY=onion — running onion bootstrap only\n");
        { extern int test_onion_bootstrap(void);
          failures += test_onion_bootstrap(); }
        printf("\n=== Onion subset complete: %d failure(s) ===\n", failures);
        return failures ? 1 : 0;
    }
    if (only && strcmp(only, "cold_start") == 0) {
        printf("[test] ZCL_TEST_ONLY=cold_start — running cold-start sync only\n");
        { extern int test_cold_start_sync(void);
          failures += test_cold_start_sync(); }
        printf("\n=== Cold-start subset complete: %d failure(s) ===\n", failures);
        return failures ? 1 : 0;
    }
    if (only && strcmp(only, "kill9") == 0) {
        printf("[test] ZCL_TEST_ONLY=kill9 — running kill -9 recovery only\n");
        { extern int test_kill9_recovery(void);
          failures += test_kill9_recovery(); }
        printf("\n=== kill9 subset complete: %d failure(s) ===\n", failures);
        return failures ? 1 : 0;
    }
    if (only && strcmp(only, "chain_advance_atomicity") == 0) {
        printf("[test] ZCL_TEST_ONLY=chain_advance_atomicity — running Move 2 / A5 only\n");
        { extern int test_chain_advance_atomicity(void);
          failures += test_chain_advance_atomicity(); }
        printf("\n=== chain_advance_atomicity subset complete: %d failure(s) ===\n",
               failures);
        return failures ? 1 : 0;
    }
    if (only && strcmp(only, "chain_advance_coordinator") == 0) {
        printf("[test] ZCL_TEST_ONLY=chain_advance_coordinator — running source policy only\n");
        failures += test_chain_advance_coordinator();
        printf("\n=== chain_advance_coordinator subset complete: %d failure(s) ===\n",
               failures);
        return failures ? 1 : 0;
    }
    if (only && strcmp(only, "connman_addnode") == 0) {
        printf("[test] ZCL_TEST_ONLY=connman_addnode — running addnode fallback only\n");
        failures += test_connman_addnode_fallback();
        printf("\n=== connman_addnode subset complete: %d failure(s) ===\n",
               failures);
        return failures ? 1 : 0;
    }
    if (only && strcmp(only, "node_health") == 0) {
        printf("[test] ZCL_TEST_ONLY=node_health — running node health only\n");
        failures += test_node_health_service();
        printf("\n=== node_health subset complete: %d failure(s) ===\n",
               failures);
        return failures ? 1 : 0;
    }
    if (only && strcmp(only, "shielded_payment") == 0) {
        printf("[test] ZCL_TEST_ONLY=shielded_payment — running shielded-payment gate only\n");
        { extern int test_shielded_payment_gate(void);
          failures += test_shielded_payment_gate(); }
        printf("\n=== shielded-payment subset complete: %d failure(s) ===\n",
               failures);
        return failures ? 1 : 0;
    }
    if (only && strcmp(only, "store_e2e") == 0) {
        printf("[test] ZCL_TEST_ONLY=store_e2e — running store e2e gate only\n");
        { extern int test_store_e2e_gate(void);
          failures += test_store_e2e_gate(); }
        printf("\n=== store e2e subset complete: %d failure(s) ===\n",
               failures);
        return failures ? 1 : 0;
    }
    if (only && strcmp(only, "parity_diff") == 0) {
        printf("[test] ZCL_TEST_ONLY=parity_diff — running parity-diff gate only\n");
        failures += test_parity_diff_gate();
        printf("\n=== parity_diff subset complete: %d failure(s) ===\n",
               failures);
        return failures ? 1 : 0;
    }
    if (only && strcmp(only, "event") == 0) {
        printf("[test] ZCL_TEST_ONLY=event — running event subset\n");
        failures += test_event();
        printf("\n=== event subset complete: %d failure(s) ===\n",
               failures);
        return failures ? 1 : 0;
    }
    if (only && strcmp(only, "persistence") == 0) {
        printf("[test] ZCL_TEST_ONLY=persistence — running persistence subset\n");
        failures += test_schema_migration();
        failures += test_db_migration_idempotent();
        failures += test_coins_view_atomicity();
        failures += test_make_lint_gates();
        failures += test_wallet_sqlite_enc();
        failures += test_wallet_keystore();
        { extern int test_wallet_persistence_cycle(void);
          failures += test_wallet_persistence_cycle(); }
        { extern int test_wallet_flush_rollback(void);
          failures += test_wallet_flush_rollback(); }
        { extern int test_wallet_sqlite_open_errors(void);
          failures += test_wallet_sqlite_open_errors(); }
        { extern int test_watch_only(void); failures += test_watch_only(); }
        { extern int test_wallet_canary(void); failures += test_wallet_canary(); }
        failures += test_unclean_shutdown_advance();
        printf("\n=== Persistence subset complete: %d failure(s) ===\n",
               failures);
        return failures ? 1 : 0;
    }
    if (only && strcmp(only, "sqlite") == 0) {
        printf("[test] ZCL_TEST_ONLY=sqlite — running SQLite model subset\n");
        failures += test_sqlite();
        printf("\n=== sqlite subset complete: %d failure(s) ===\n",
               failures);
        return failures ? 1 : 0;
    }
    if (only && strcmp(only, "rpc_safety") == 0) {
        printf("[test] ZCL_TEST_ONLY=rpc_safety — running RPC safety subset\n");
        failures += test_rpc_safety();
        failures += test_make_lint_gates();
        printf("\n=== RPC safety subset complete: %d failure(s) ===\n",
               failures);
        return failures ? 1 : 0;
    }
    if (only && strcmp(only, "power_node_contract") == 0) {
        printf("[test] ZCL_TEST_ONLY=power_node_contract — running doc contract only\n");
        failures += test_power_node_contract_spec();
        printf("\n=== power_node_contract subset complete: %d failure(s) ===\n",
               failures);
        return failures ? 1 : 0;
    }
    if (only && strcmp(only, "self_heal_scan") == 0) {
        printf("[test] ZCL_TEST_ONLY=self_heal_scan — running only\n");
        failures += test_self_heal_scan_fallback();
        printf("\n=== self_heal_scan subset complete: %d failure(s) ===\n",
               failures);
        return failures ? 1 : 0;
    }
    if (only && strcmp(only, "hot_loop_exit") == 0) {
        printf("[test] ZCL_TEST_ONLY=hot_loop_exit — running UTXO hot-loop exit only\n");
        failures += test_connect_tip_hot_loop_exit();
        printf("\n=== hot_loop_exit subset complete: %d failure(s) ===\n",
               failures);
        return failures ? 1 : 0;
    }
    if (only && strcmp(only, "chain_restore") == 0) {
        printf("[test] ZCL_TEST_ONLY=chain_restore — running chain restore only\n");
        failures += test_chain_restore_service();
        printf("\n=== chain_restore subset complete: %d failure(s) ===\n",
               failures);
        return failures ? 1 : 0;
    }
    if (only && strcmp(only, "block_index_loader") == 0) {
        printf("[test] ZCL_TEST_ONLY=block_index_loader — running block index loader only\n");
        failures += test_block_index_loader();
        printf("\n=== block_index_loader subset complete: %d failure(s) ===\n",
               failures);
        return failures ? 1 : 0;
    }
    if (only && strcmp(only, "utxo_recovery") == 0) {
        printf("[test] ZCL_TEST_ONLY=utxo_recovery — running UTXO recovery only\n");
        failures += test_utxo_recovery_service();
        printf("\n=== utxo_recovery subset complete: %d failure(s) ===\n",
               failures);
        return failures ? 1 : 0;
    }
    if (only && strcmp(only, "chain_evidence_controller") == 0) {
        printf("[test] ZCL_TEST_ONLY=chain_evidence_controller — running chain evidence controller only\n");
        failures += test_chain_evidence_controller();
        printf("\n=== chain_evidence_controller subset complete: %d failure(s) ===\n",
               failures);
        return failures ? 1 : 0;
    }
    if (only && strcmp(only, "chain_state_repo") == 0) {
        printf("[test] ZCL_TEST_ONLY=chain_state_repo — running chain state repository only\n");
        failures += test_chain_state_repo();
        printf("\n=== chain_state_repo subset complete: %d failure(s) ===\n",
               failures);
        return failures ? 1 : 0;
    }
    if (only && strcmp(only, "sync_service") == 0) {
        printf("[test] ZCL_TEST_ONLY=sync_service — running sync service only\n");
        failures += test_sync_service();
        printf("\n=== sync_service subset complete: %d failure(s) ===\n",
               failures);
        return failures ? 1 : 0;
    }
    if (only && strcmp(only, "utxo_audit") == 0) {
        printf("[test] ZCL_TEST_ONLY=utxo_audit — running only\n");
        failures += test_utxo_audit();
        printf("\n=== utxo_audit subset complete: %d failure(s) ===\n",
               failures);
        return failures ? 1 : 0;
    }
    if (only && strcmp(only, "mcp_controllers") == 0) {
        printf("[test] ZCL_TEST_ONLY=mcp_controllers — running MCP controller subset\n");
        failures += test_mcp_controllers();
        printf("\n=== mcp_controllers subset complete: %d failure(s) ===\n",
               failures);
        return failures ? 1 : 0;
    }
    if (only && strcmp(only, "make_lint_gates") == 0) {
        printf("[test] ZCL_TEST_ONLY=make_lint_gates — running lint gate subset\n");
        failures += test_make_lint_gates();
        printf("\n=== make_lint_gates subset complete: %d failure(s) ===\n",
               failures);
        return failures ? 1 : 0;
    }
    if (only && strcmp(only, "zclassicd_oracle") == 0) {
        printf("[test] ZCL_TEST_ONLY=zclassicd_oracle — running oracle subset\n");
        failures += test_zclassicd_oracle();
        printf("\n=== zclassicd_oracle subset complete: %d failure(s) ===\n",
               failures);
        return failures ? 1 : 0;
    }
    if (only && strcmp(only, "sync_watchdog") == 0) {
        printf("[test] ZCL_TEST_ONLY=sync_watchdog — running watchdog subset\n");
        failures += test_sync_watchdog();
        printf("\n=== sync_watchdog subset complete: %d failure(s) ===\n",
               failures);
        return failures ? 1 : 0;
    }
    if (only && strcmp(only, "syncdiag_rpc") == 0) {
        printf("[test] ZCL_TEST_ONLY=syncdiag_rpc — running sync RPC diagnostics subset\n");
        failures += test_syncdiag_rpc();
        printf("\n=== syncdiag_rpc subset complete: %d failure(s) ===\n",
               failures);
        return failures ? 1 : 0;
    }
    if (only && strcmp(only, "service_kernel") == 0) {
        printf("[test] ZCL_TEST_ONLY=service_kernel - running service kernel subset\n");
        failures += test_service_kernel();
        printf("\n=== service_kernel subset complete: %d failure(s) ===\n",
               failures);
        return failures ? 1 : 0;
    }
    if (only && strcmp(only, "tor") == 0) {
        printf("[test] ZCL_TEST_ONLY=tor - running Tor integration subset\n");
        failures += test_tor();
        printf("\n=== tor subset complete: %d failure(s) ===\n",
               failures);
        return failures ? 1 : 0;
    }
    if (only && strcmp(only, "fast_sync") == 0) {
        printf("[test] ZCL_TEST_ONLY=fast_sync — running fast sync subset\n");
        failures += test_fast_sync();
        printf("\n=== fast_sync subset complete: %d failure(s) ===\n",
               failures);
        return failures ? 1 : 0;
    }
    if (only && strcmp(only, "explorer") == 0) {
        printf("[test] ZCL_TEST_ONLY=explorer — running explorer subset\n");
        failures += test_explorer();
        printf("\n=== explorer subset complete: %d failure(s) ===\n",
               failures);
        return failures ? 1 : 0;
    }
    if (only && strcmp(only, "mmb") == 0) {
        printf("[test] ZCL_TEST_ONLY=mmb - running MMB subset\n");
        failures += test_mmb();
        printf("\n=== mmb subset complete: %d failure(s) ===\n",
               failures);
        return failures ? 1 : 0;
    }
    if (only && strcmp(only, "addrman") == 0) {
        printf("[test] ZCL_TEST_ONLY=addrman — running addrman subset\n");
        failures += test_addrman_rebalance();
        printf("\n=== addrman subset complete: %d failure(s) ===\n",
               failures);
        return failures ? 1 : 0;
    }
    if (only && strcmp(only, "integrity") == 0) {
        printf("[test] ZCL_TEST_ONLY=integrity — running integrity subset\n");
        failures += test_integrity();
        printf("\n=== integrity subset complete: %d failure(s) ===\n",
               failures);
        return failures ? 1 : 0;
    }
    if (only && strcmp(only, "net") == 0) {
        printf("[test] ZCL_TEST_ONLY=net — running net subset\n");
        failures += test_net();
        failures += test_peer_lifecycle();
        printf("\n=== net subset complete: %d failure(s) ===\n",
               failures);
        return failures ? 1 : 0;
    }
    if (only && strcmp(only, "peer_lifecycle") == 0) {
        printf("[test] ZCL_TEST_ONLY=peer_lifecycle — running peer lifecycle subset\n");
        failures += test_peer_lifecycle();
        printf("\n=== peer_lifecycle subset complete: %d failure(s) ===\n",
               failures);
        return failures ? 1 : 0;
    }
    if (only && strcmp(only, "header_sync") == 0) {
        printf("[test] ZCL_TEST_ONLY=header_sync — running header sync subset\n");
        failures += test_header_sync();
        failures += test_header_sync_stall();
        printf("\n=== header_sync subset complete: %d failure(s) ===\n",
               failures);
        return failures ? 1 : 0;
    }
    if (only && strcmp(only, "snapshot_sync") == 0) {
        printf("[test] ZCL_TEST_ONLY=snapshot_sync - running snapshot sync subset\n");
        failures += test_snapshot_sync_service();
        printf("\n=== snapshot_sync subset complete: %d failure(s) ===\n",
               failures);
        return failures ? 1 : 0;
    }

    failures += test_load_balancer();
    failures += test_game();
    failures += test_crypto();
    failures += test_encoding();
    failures += test_chain();
    { extern int test_pprev_walk(void); failures += test_pprev_walk(); }
    { extern int test_chain_tip(void); failures += test_chain_tip(); }
    { extern int test_checkpoint(void); failures += test_checkpoint(); }
    failures += test_keys();
    failures += test_script();
    failures += test_net();
    failures += test_connman_addnode_fallback();
    failures += test_transaction();
    failures += test_mempool();
    failures += test_rpc();
    failures += test_sqlite();
    failures += test_activerecord();
    failures += test_validation();
    { extern int test_sapling_lazy_init(void);
      failures += test_sapling_lazy_init(); }
    failures += test_sapling();
    failures += test_sapling_crypto();
    failures += test_sapling_tree();
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
    failures += test_chainstate_legacy_reader();
    { extern int test_ldb_snapshot(void);
      failures += test_ldb_snapshot(); }
    { extern int test_utxo_snapshot_loader(void);
      failures += test_utxo_snapshot_loader(); }
    failures += test_store();
    failures += test_blog();
    failures += test_api();
    failures += test_explorer();
    failures += test_mining();
    failures += test_utxo_commitment();
    failures += test_mmr();
    failures += test_mmb();
    { extern int test_sha3_windows(void);
      failures += test_sha3_windows(); }
    failures += test_flyclient();
    failures += test_scan_util();
    failures += test_tor();
    { extern int test_onion_bootstrap(void);
      failures += test_onion_bootstrap(); }
    { extern int test_cold_start_sync(void);
      failures += test_cold_start_sync(); }
    { extern int test_kill9_recovery(void);
      failures += test_kill9_recovery(); }
    { extern int test_shielded_payment_gate(void);
      failures += test_shielded_payment_gate(); }
    { extern int test_store_e2e_gate(void);
      failures += test_store_e2e_gate(); }
    { extern int test_soak_harness(void);
      failures += test_soak_harness(); }
    failures += test_parity_diff_gate();
    failures += test_event();
    failures += test_download();
    failures += test_consensus();
    failures += test_policy();
    failures += test_wallet_view();
    failures += test_fast_sync();
    failures += test_block_scan();
    failures += test_node_health_service();
    failures += test_syncdiag_rpc();
    failures += test_rpc_safety();
    failures += test_chain_state_repo();
    failures += test_chain_evidence_controller();
    failures += test_long_op();
    failures += test_recovery_policy();
    failures += test_db_txn();
    failures += test_sync_service();
    failures += test_sync_state_fsm();
    failures += test_heartbeat();
    failures += test_chain_advance();
    failures += test_chain_advance_coordinator();
    { extern int test_chain_advance_atomicity(void);
      failures += test_chain_advance_atomicity(); }
    failures += test_local_chain_ingest();
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
    failures += test_peer_lifecycle();
    failures += test_secrets_hygiene();
    failures += test_block_index_integrity();
    failures += test_wallet_backup();
    { extern int test_wallet_canary(void); failures += test_wallet_canary(); }
    { extern int test_wallet_persistence_cycle(void);
      failures += test_wallet_persistence_cycle(); }
    { extern int test_wallet_flush_rollback(void);
      failures += test_wallet_flush_rollback(); }
    failures += test_log_json();
    failures += test_http_middleware();
    failures += test_rpc_timeout();
    failures += test_wallet_keystore();
    failures += test_wallet_sqlite_enc();
    { extern int test_zcl_result(void); failures += test_zcl_result(); }
    { extern int test_wallet_sqlite_open_errors(void);
      failures += test_wallet_sqlite_open_errors(); }
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
    failures += test_unclean_shutdown_advance();
    { extern int test_no_hardcoded_home(void);
      failures += test_no_hardcoded_home(); }
    failures += test_cookie_rotation();
    failures += test_reorg_safety();
    failures += test_key_scrub();
    failures += test_block_index_loader();
    failures += test_chain_state_validator();
    failures += test_utxo_recovery_service();
    failures += test_connect_tip_hot_loop_exit();
    failures += test_self_heal_scan_fallback();
    failures += test_utxo_audit();
    failures += test_rpc_error_envelope();
    failures += test_tx_property();
    failures += test_workpool();
    failures += test_service_kernel();
    { extern int test_thread_registry(void);
      failures += test_thread_registry(); }
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
    failures += test_db_migration_idempotent();
    failures += test_coins_view_atomicity();
    failures += test_chain_stall_repro();
    failures += test_failed_child_cap();
    failures += test_power_node_contract_spec();
    failures += test_boot_phase();
    failures += test_path_check();
    failures += test_supervisor();
    failures += test_blocker();
    failures += test_domain_consensus_verify();
    failures += test_application_consensus_validate_block();
    failures += test_block_log_file();
    failures += test_mutator();
    failures += test_shadow_feeder();
    failures += test_diff_with_legacy_shadow();
    failures += test_utxo_snapshot_inmem();
    failures += test_make_lint_gates();
    failures += test_multisig();
    failures += test_mcp_fuzz();
    failures += test_rpc_auth_hardening();
    failures += test_sync_watchdog();
    failures += test_disk_block_io();
    failures += test_msg_handlers();
    failures += test_zclassicd_oracle();
    failures += test_header_probe_service();
    { extern int test_lag_slo(void); failures += test_lag_slo(); }

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
