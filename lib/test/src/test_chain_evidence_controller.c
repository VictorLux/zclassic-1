/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"

#include "services/chain_evidence_controller.h"
#include "coins/coins_view.h"
#include "models/database.h"

#include <stdio.h>
#include <string.h>

struct auth_fixture {
    struct node_db ndb;
    struct block_map bm;
    struct active_chain chain;
    struct block_index *header_tip;
    struct coins_view_cache coins_tip;
    struct chain_state_repository csr;
    struct chain_evidence_controller authority;
    struct uint256 hashes[3];
    struct block_index blocks[3];
};

static bool auth_fixture_init(struct auth_fixture *f)
{
    memset(f, 0, sizeof(*f));
    if (!node_db_open(&f->ndb, ":memory:"))
        return false;
    block_map_init(&f->bm);
    active_chain_init(&f->chain);

    struct coins_view null_view;
    memset(&null_view, 0, sizeof(null_view));
    coins_view_cache_init(&f->coins_tip, &null_view);

    for (int i = 0; i < 3; i++) {
        memset(f->hashes[i].data, i + 1, 32);
        block_index_init(&f->blocks[i]);
        f->blocks[i].phashBlock = &f->hashes[i];
        f->blocks[i].nHeight = i;
        f->blocks[i].pprev = i ? &f->blocks[i - 1] : NULL;
        f->blocks[i].nStatus = BLOCK_VALID_TREE;
        block_map_insert(&f->bm, &f->hashes[i], &f->blocks[i]);
        const struct uint256 *canon =
            block_map_find_hash(&f->bm, &f->hashes[i]);
        if (canon)
            f->blocks[i].phashBlock = canon;
    }

    csr_init(&f->csr, &f->bm, &f->chain, &f->header_tip,
             &f->coins_tip, &f->ndb, NULL);
    chain_evidence_controller_init(&f->authority, &f->ndb, &f->csr);
    return true;
}

static void auth_fixture_free(struct auth_fixture *f)
{
    csr_free(&f->csr);
    coins_view_cache_free(&f->coins_tip);
    active_chain_free(&f->chain);
    block_map_free(&f->bm);
    node_db_close(&f->ndb);
}

static struct chain_evidence_controller_snapshot_meta auth_manifest(
    const struct auth_fixture *f)
{
    struct chain_evidence_controller_snapshot_meta m;
    memset(&m, 0, sizeof(m));
    m.anchor_height = 1;
    m.anchor_hash = *f->blocks[1].phashBlock;
    memset(m.utxo_sha3.data, 0xa5, 32);
    memset(m.chainwork, 0x0b, 32);
    memset(m.mmb_root, 0x0c, 32);
    m.utxo_count = 10;
    m.finality_depth = 100;
    m.schema_version = 1;
    m.producer = "unit";
    m.verified.header_ancestry_linked = true;
    m.verified.chainwork_recomputed = true;
    m.verified.nakamoto_selected_best_work = true;
    m.verified.utxo_sha3_verified = true;
    m.verified.mmb_flyclient_proof_verified = true;
    m.verified.chunk_hash_coverage_verified = true;
    return m;
}

static int test_manifest_missing_proofs_freezes(void)
{
    int failures = 0;
    struct auth_fixture f;
    if (!auth_fixture_init(&f))
        return 1;
    struct chain_evidence_controller_snapshot_meta m = auth_manifest(&f);
    m.verified.mmb_flyclient_proof_verified = false;

    enum chain_evidence_controller_result r =
        chain_evidence_controller_import_snapshot_evidence(&f.authority, &m);
    if (r != CEC_REJECTED_BAD_PROOF ||
        f.authority.state != CEC_CONTRADICTION_FROZEN)
        failures++;

    auth_fixture_free(&f);
    return failures;
}

static int test_old_metadata_is_ignored(void)
{
    int failures = 0;
    struct auth_fixture f;
    if (!auth_fixture_init(&f))
        return 1;

    const char old_mode[] = "snapshot_assumed";
    const char old_index[] = "trusted";
    node_db_state_set(&f.ndb, "sync_mode", old_mode, sizeof(old_mode));
    node_db_state_set(&f.ndb, "block_index_trust_state",
                      old_index, sizeof(old_index));

    chain_evidence_controller_load_state(&f.authority);
    if (f.authority.state != CEC_EMPTY)
        failures++;

    struct chain_evidence_controller_view view;
    chain_evidence_controller_snapshot(&f.authority, &view);
    if (view.block_index_evidence_state.header_ancestry_linked ||
        view.block_index_evidence_state.chainwork_recomputed ||
        view.block_index_evidence_state.block_bytes_hash_checked)
        failures++;

    auth_fixture_free(&f);
    return failures;
}

static int test_csr_commit_does_not_write_evidence_metadata(void)
{
    int failures = 0;
    struct auth_fixture f;
    if (!auth_fixture_init(&f))
        return 1;

    struct chain_state_commit commit = {
        .new_tip = &f.blocks[1],
        .new_coins_best = *f.blocks[1].phashBlock,
        .expected_utxo_count = 0,
        .update_header_tip = true,
        .rollback_auth = NULL,
        .wallet_scan_height = -1,
        .reason = "unit.csr_only",
    };
    if (csr_commit_tip(&f.csr, &commit) != CSR_OK)
        failures++;

    char buf[64];
    size_t len = 0;
    memset(buf, 0, sizeof(buf));
    if (node_db_state_get(&f.ndb, "cec.active_tip_evidence",
                          buf, sizeof(buf), &len))
        failures++;
    len = 0;
    if (node_db_state_get(&f.ndb, "active_tip_evidence",
                          buf, sizeof(buf), &len))
        failures++;

    auth_fixture_free(&f);
    return failures;
}

static int test_verified_local_block_bootstraps_tip_evidence(void)
{
    int failures = 0;
    struct auth_fixture f;
    if (!auth_fixture_init(&f))
        return 1;

    struct chain_evidence_controller_tip_request req = {
        .new_tip = &f.blocks[1],
        .utxo_max_height = 1,
        .update_header_tip = true,
        .reason = "unit.local_validation",
    };
    req.verified.header_ancestry_linked = true;
    req.verified.chainwork_recomputed = true;
    req.verified.nakamoto_selected_best_work = true;
    req.verified.block_bytes_hash_checked = true;

    if (chain_evidence_controller_promote_tip(&f.authority, &req) != CEC_OK)
        failures++;
    if (f.authority.state != CEC_TIP_FOLLOWING)
        failures++;
    struct chain_evidence_controller_view view;
    chain_evidence_controller_snapshot(&f.authority, &view);
    if (!view.active_tip_evidence.block_bytes_hash_checked ||
        !view.block_index_evidence_state.nakamoto_selected_best_work ||
        view.active_tip_height != 1)
        failures++;

    auth_fixture_free(&f);
    return failures;
}

static int test_incomplete_evidence_tip_promotion_rejected(void)
{
    int failures = 0;
    struct auth_fixture f;
    if (!auth_fixture_init(&f))
        return 1;
    struct chain_evidence_controller_snapshot_meta m = auth_manifest(&f);
    if (chain_evidence_controller_import_snapshot_evidence(&f.authority, &m)
        != CEC_OK)
        failures++;

    struct chain_evidence_controller_tip_request req = {
        .new_tip = &f.blocks[1],
        .utxo_max_height = 1,
        .update_header_tip = true,
        .reason = "unit",
    };
    if (chain_evidence_controller_promote_tip(&f.authority, &req)
        != CEC_REJECTED_INCOMPLETE_INDEX_EVIDENCE)
        failures++;
    if (f.authority.state != CEC_SNAPSHOT_UTXO_HASH_VERIFIED)
        failures++;

    auth_fixture_free(&f);
    return failures;
}

static int test_utxo_ahead_of_evidenced_index_rejected(void)
{
    int failures = 0;
    struct auth_fixture f;
    if (!auth_fixture_init(&f))
        return 1;
    struct chain_evidence_controller_snapshot_meta m = auth_manifest(&f);
    if (chain_evidence_controller_import_snapshot_evidence(&f.authority, &m)
        != CEC_OK)
        failures++;

    struct chain_evidence_controller_tip_request req = {
        .new_tip = &f.blocks[1],
        .utxo_max_height = 2,
        .update_header_tip = true,
        .reason = "unit",
    };
    req.verified.header_ancestry_linked = true;
    req.verified.chainwork_recomputed = true;
    req.verified.nakamoto_selected_best_work = true;
    req.verified.block_bytes_hash_checked = true;
    if (chain_evidence_controller_promote_tip(&f.authority, &req)
        != CEC_REJECTED_UTXO_AHEAD_OF_INDEX)
        failures++;
    if (f.authority.state != CEC_CONTRADICTION_FROZEN)
        failures++;

    auth_fixture_free(&f);
    return failures;
}

static int test_csr_rejection_does_not_persist_tip_evidence(void)
{
    int failures = 0;
    struct auth_fixture f;
    if (!auth_fixture_init(&f))
        return 1;
    struct chain_state_repository uninitialized_csr;
    memset(&uninitialized_csr, 0, sizeof(uninitialized_csr));
    chain_evidence_controller_init(&f.authority, &f.ndb, &uninitialized_csr);

    struct chain_evidence_controller_snapshot_meta m = auth_manifest(&f);
    if (chain_evidence_controller_import_snapshot_evidence(&f.authority, &m)
        != CEC_OK)
        failures++;

    struct chain_evidence_controller_tip_request req = {
        .new_tip = &f.blocks[1],
        .utxo_max_height = 1,
        .update_header_tip = true,
        .reason = "unit.csr_reject",
    };
    req.verified.header_ancestry_linked = true;
    req.verified.chainwork_recomputed = true;
    req.verified.nakamoto_selected_best_work = true;
    req.verified.block_bytes_hash_checked = true;
    if (chain_evidence_controller_promote_tip(&f.authority, &req)
        != CEC_REJECTED_CSR)
        failures++;

    char hex[65];
    char key[sizeof("cec.block_evidence.") + 64];
    char buf[128];
    size_t len = 0;
    uint256_get_hex(f.blocks[1].phashBlock, hex);
    snprintf(key, sizeof(key), "cec.block_evidence.%s", hex);
    if (node_db_state_get(&f.ndb, key, buf, sizeof(buf), &len))
        failures++;
    len = 0;
    if (node_db_state_get(&f.ndb, "cec.active_tip_hash",
                          buf, sizeof(buf), &len))
        failures++;

    auth_fixture_free(&f);
    return failures;
}

static int test_persistence_preflight_blocks_csr_publication(void)
{
    int failures = 0;
    struct auth_fixture f;
    if (!auth_fixture_init(&f))
        return 1;

    struct chain_evidence_controller_snapshot_meta m = auth_manifest(&f);
    if (chain_evidence_controller_import_snapshot_evidence(&f.authority, &m)
        != CEC_OK)
        failures++;

    f.authority.ndb = NULL;

    struct chain_evidence_controller_tip_request req = {
        .new_tip = &f.blocks[1],
        .utxo_max_height = 1,
        .update_header_tip = true,
        .reason = "unit.no_persistence_target",
    };
    req.verified.header_ancestry_linked = true;
    req.verified.chainwork_recomputed = true;
    req.verified.nakamoto_selected_best_work = true;
    req.verified.block_bytes_hash_checked = true;

    if (chain_evidence_controller_promote_tip(&f.authority, &req)
        != CEC_REJECTED_PERSIST)
        failures++;
    if (active_chain_height(&f.chain) != -1)
        failures++;
    if (f.header_tip != NULL)
        failures++;
    if (f.authority.state != CEC_CONTRADICTION_FROZEN)
        failures++;

    f.authority.ndb = &f.ndb;
    auth_fixture_free(&f);
    return failures;
}

static int test_evidence_transaction_can_join_outer_publication_txn(void)
{
    int failures = 0;
    struct auth_fixture f;
    if (!auth_fixture_init(&f))
        return 1;

    struct chain_evidence_controller_snapshot_meta m = auth_manifest(&f);
    if (chain_evidence_controller_import_snapshot_evidence(&f.authority, &m)
        != CEC_OK)
        failures++;

    if (!node_db_begin(&f.ndb))
        failures++;

    struct chain_evidence_controller_tip_request req = {
        .new_tip = &f.blocks[1],
        .utxo_max_height = 1,
        .update_header_tip = true,
        .reason = "unit.txn_required",
    };
    req.verified.header_ancestry_linked = true;
    req.verified.chainwork_recomputed = true;
    req.verified.nakamoto_selected_best_work = true;
    req.verified.block_bytes_hash_checked = true;

    if (chain_evidence_controller_promote_tip(&f.authority, &req)
        != CEC_OK)
        failures++;
    if (active_chain_height(&f.chain) != 1)
        failures++;
    if (f.header_tip != &f.blocks[1])
        failures++;

    if (!node_db_commit(&f.ndb))
        failures++;
    auth_fixture_free(&f);
    return failures;
}

static int test_valid_evidenced_snapshot_promotes_to_tip_following(void)
{
    int failures = 0;
    struct auth_fixture f;
    if (!auth_fixture_init(&f))
        return 1;
    struct chain_evidence_controller_snapshot_meta m = auth_manifest(&f);
    if (chain_evidence_controller_import_snapshot_evidence(&f.authority, &m)
        != CEC_OK)
        failures++;

    struct chain_evidence_controller_tip_request req = {
        .new_tip = &f.blocks[1],
        .utxo_max_height = 1,
        .update_header_tip = true,
        .reason = "unit",
    };
    req.verified.header_ancestry_linked = true;
    req.verified.chainwork_recomputed = true;
    req.verified.nakamoto_selected_best_work = true;
    req.verified.block_bytes_hash_checked = true;
    if (chain_evidence_controller_promote_tip(&f.authority, &req) != CEC_OK)
        failures++;
    if (active_chain_height(&f.chain) != 1)
        failures++;
    if (f.header_tip != &f.blocks[1])
        failures++;
    {
        struct uint256 persisted_best;
        size_t len = 0;
        memset(&persisted_best, 0, sizeof(persisted_best));
        if (!node_db_state_get(&f.ndb, "coins_best_block",
                               persisted_best.data,
                               sizeof(persisted_best.data), &len) ||
            len != sizeof(persisted_best.data) ||
            memcmp(persisted_best.data,
                   f.blocks[1].phashBlock->data,
                   sizeof(persisted_best.data)) != 0)
            failures++;
    }
    if (f.authority.state != CEC_TIP_FOLLOWING)
        failures++;
    {
        struct chain_evidence_controller_view view;
        chain_evidence_controller_snapshot(&f.authority, &view);
        if (!view.block_index_evidence_state.header_ancestry_linked ||
            !view.block_index_evidence_state.chainwork_recomputed ||
            !view.block_index_evidence_state.block_bytes_hash_checked)
            failures++;
        if (!view.active_tip_evidence.nakamoto_selected_best_work)
            failures++;
    }

    auth_fixture_free(&f);
    return failures;
}

static int test_commit_failure_after_csr_restores_concrete_state(void)
{
    int failures = 0;
    struct auth_fixture f;
    if (!auth_fixture_init(&f))
        return 1;

    struct chain_state_rollback_authorization rollback_auth = {
        .source = CSR_ROLLBACK_SOURCE_TEST,
        .decision = POLICY_ALLOW,
        .from_height = -1,
        .to_height = 0,
        .max_depth = 0,
        .evidence_class = "unit_baseline",
        .reason = "unit.baseline",
    };
    struct chain_state_commit baseline = {
        .new_tip = &f.blocks[0],
        .new_coins_best = *f.blocks[0].phashBlock,
        .expected_utxo_count = 0,
        .update_header_tip = true,
        .persist_coins_best = false,
        .rollback_auth = &rollback_auth,
        .wallet_scan_height = -1,
        .reason = "unit.baseline",
    };
    if (csr_commit_tip(&f.csr, &baseline) != CSR_OK)
        failures++;

    struct chain_evidence_controller_snapshot_meta m = auth_manifest(&f);
    if (chain_evidence_controller_import_snapshot_evidence(&f.authority, &m)
        != CEC_OK)
        failures++;

    struct chain_evidence_controller_tip_request req = {
        .new_tip = &f.blocks[1],
        .utxo_max_height = 1,
        .update_header_tip = true,
        .reason = "unit.force_commit_failure",
    };
    req.verified.header_ancestry_linked = true;
    req.verified.chainwork_recomputed = true;
    req.verified.nakamoto_selected_best_work = true;
    req.verified.block_bytes_hash_checked = true;

    chain_evidence_controller_test_fail_commit_after_csr(true);
    if (chain_evidence_controller_promote_tip(&f.authority, &req)
        != CEC_REJECTED_PERSIST)
        failures++;

    if (active_chain_tip(&f.chain) != &f.blocks[0])
        failures++;
    if (f.header_tip != &f.blocks[0])
        failures++;
    {
        struct uint256 coins_best;
        coins_view_cache_get_best_block(&f.coins_tip, &coins_best);
        if (memcmp(coins_best.data, f.blocks[0].phashBlock->data,
                   sizeof(coins_best.data)) != 0)
            failures++;
    }
    if (f.authority.state != CEC_SNAPSHOT_UTXO_HASH_VERIFIED)
        failures++;

    auth_fixture_free(&f);
    return failures;
}

static int test_full_validation_requires_matching_utxo_sha3(void)
{
    int failures = 0;
    struct auth_fixture f;
    if (!auth_fixture_init(&f))
        return 1;
    struct chain_evidence_controller_snapshot_meta m = auth_manifest(&f);
    if (chain_evidence_controller_import_snapshot_evidence(&f.authority, &m)
        != CEC_OK)
        failures++;

    struct uint256 wrong;
    memset(wrong.data, 0xff, 32);
    if (chain_evidence_controller_mark_fully_validated(&f.authority, &wrong)
        != CEC_REJECTED_BAD_PROOF)
        failures++;
    if (f.authority.state != CEC_CONTRADICTION_FROZEN)
        failures++;

    auth_fixture_free(&f);
    return failures;
}

int test_chain_evidence_controller(void)
{
    int failures = 0;
    failures += test_manifest_missing_proofs_freezes();
    failures += test_old_metadata_is_ignored();
    failures += test_csr_commit_does_not_write_evidence_metadata();
    failures += test_verified_local_block_bootstraps_tip_evidence();
    failures += test_incomplete_evidence_tip_promotion_rejected();
    failures += test_utxo_ahead_of_evidenced_index_rejected();
    failures += test_csr_rejection_does_not_persist_tip_evidence();
    failures += test_persistence_preflight_blocks_csr_publication();
    failures += test_evidence_transaction_can_join_outer_publication_txn();
    failures += test_valid_evidenced_snapshot_promotes_to_tip_following();
    failures += test_commit_failure_after_csr_restores_concrete_state();
    failures += test_full_validation_requires_matching_utxo_sha3();
    return failures;
}
