/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "services/chain_evidence_controller.h"

#include "models/database.h"
#include "models/db_txn.h"

#include <stdio.h>
#include <string.h>

#ifdef ZCL_TESTING
static bool g_cec_test_fail_commit_after_csr;

void chain_evidence_controller_test_fail_commit_after_csr(bool fail)
{
    g_cec_test_fail_commit_after_csr = fail;
}
#endif

static bool bytes32_nonzero(const uint8_t b[32])
{
    uint8_t acc = 0;
    for (int i = 0; i < 32; i++)
        acc |= b[i];
    return acc != 0;
}

static bool u256_nonzero(const struct uint256 *u)
{
    return u && bytes32_nonzero(u->data);
}

const char *chain_evidence_controller_state_name(enum chain_evidence_controller_state state)
{
    static const char *names[] = {
        [CEC_EMPTY]                 = "empty",
        [CEC_HEADERS_WORK_VALIDATED] = "headers_work_validated",
        [CEC_SNAPSHOT_UTXO_HASH_VERIFIED] = "snapshot_utxo_hash_verified",
        [CEC_TIP_FOLLOWING]         = "tip_following",
        [CEC_BACKGROUND_VALIDATING] = "background_validating",
        [CEC_FULLY_VALIDATED]       = "fully_validated",
        [CEC_CONTRADICTION_FROZEN]  = "contradiction_frozen",
    };
    if (state >= 0 && state < CEC_NUM_STATES)
        return names[state];
    return "unknown";
}

const char *chain_evidence_controller_result_name(enum chain_evidence_controller_result result)
{
    switch (result) {
    case CEC_OK:                              return "ok";
    case CEC_REJECTED_NULL_ARG:               return "null_arg";
    case CEC_REJECTED_FROZEN:                 return "frozen";
    case CEC_REJECTED_BAD_STATE:              return "bad_state";
    case CEC_REJECTED_BAD_PROOF:              return "bad_proof";
    case CEC_REJECTED_INCOMPLETE_INDEX_EVIDENCE:
        return "incomplete_index_evidence";
    case CEC_REJECTED_UTXO_AHEAD_OF_INDEX:    return "utxo_ahead_of_index";
    case CEC_REJECTED_CSR:                    return "csr";
    case CEC_REJECTED_PERSIST:                return "persist";
    }
    return "unknown";
}

#define CEC_RECORD_MAGIC 0x43454345u
#define CEC_RECORD_VERSION 1u

struct persisted_evidence_record {
    uint32_t magic;
    uint32_t version;
    uint8_t header_ancestry_linked;
    uint8_t chainwork_recomputed;
    uint8_t nakamoto_selected_best_work;
    uint8_t block_bytes_hash_checked;
    uint8_t utxo_sha3_verified;
    uint8_t mmb_flyclient_proof_verified;
    uint8_t chunk_hash_coverage_verified;
    uint8_t full_validation_complete;
};

static void evidence_to_persisted(const struct chain_evidence_record *in,
                                  struct persisted_evidence_record *out)
{
    memset(out, 0, sizeof(*out));
    out->magic = CEC_RECORD_MAGIC;
    out->version = CEC_RECORD_VERSION;
    if (!in)
        return;
    out->header_ancestry_linked = in->header_ancestry_linked ? 1 : 0;
    out->chainwork_recomputed = in->chainwork_recomputed ? 1 : 0;
    out->nakamoto_selected_best_work =
        in->nakamoto_selected_best_work ? 1 : 0;
    out->block_bytes_hash_checked = in->block_bytes_hash_checked ? 1 : 0;
    out->utxo_sha3_verified = in->utxo_sha3_verified ? 1 : 0;
    out->mmb_flyclient_proof_verified =
        in->mmb_flyclient_proof_verified ? 1 : 0;
    out->chunk_hash_coverage_verified =
        in->chunk_hash_coverage_verified ? 1 : 0;
    out->full_validation_complete = in->full_validation_complete ? 1 : 0;
}

static bool evidence_from_persisted(const void *buf, size_t len,
                                    struct chain_evidence_record *out)
{
    const struct persisted_evidence_record *p = buf;
    if (!buf || len != sizeof(*p) || !out ||
        p->magic != CEC_RECORD_MAGIC ||
        p->version != CEC_RECORD_VERSION)
        return false;
    memset(out, 0, sizeof(*out));
    out->header_ancestry_linked = p->header_ancestry_linked != 0;
    out->chainwork_recomputed = p->chainwork_recomputed != 0;
    out->nakamoto_selected_best_work = p->nakamoto_selected_best_work != 0;
    out->block_bytes_hash_checked = p->block_bytes_hash_checked != 0;
    out->utxo_sha3_verified = p->utxo_sha3_verified != 0;
    out->mmb_flyclient_proof_verified =
        p->mmb_flyclient_proof_verified != 0;
    out->chunk_hash_coverage_verified =
        p->chunk_hash_coverage_verified != 0;
    out->full_validation_complete = p->full_validation_complete != 0;
    return true;
}

static bool persist_evidence(struct chain_evidence_controller *a,
                             const char *key,
                             const struct chain_evidence_record *evidence)
{
    struct persisted_evidence_record p;
    evidence_to_persisted(evidence, &p);
    return a && a->ndb && node_db_state_set(a->ndb, key, &p, sizeof(p));
}

static bool load_evidence(struct node_db *ndb, const char *key,
                          struct chain_evidence_record *out)
{
    struct persisted_evidence_record p;
    size_t len = 0;
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    if (!ndb || !node_db_state_get(ndb, key, &p, sizeof(p), &len))
        return false;
    return evidence_from_persisted(&p, len, out);
}

static bool block_evidence_key(const struct uint256 *hash,
                               char out[sizeof("cec.block_evidence.") + 64])
{
    char hex[65];
    if (!hash || !out)
        return false;
    uint256_get_hex(hash, hex);
    snprintf(out, sizeof("cec.block_evidence.") + 64,
             "cec.block_evidence.%s", hex);
    return true;
}

bool chain_evidence_record_has_block_index_required(
    const struct chain_evidence_record *evidence)
{
    return evidence &&
           evidence->header_ancestry_linked &&
           evidence->chainwork_recomputed &&
           evidence->nakamoto_selected_best_work &&
           evidence->block_bytes_hash_checked;
}

bool chain_evidence_record_has_snapshot_required(
    const struct chain_evidence_record *evidence)
{
    return evidence &&
           evidence->header_ancestry_linked &&
           evidence->chainwork_recomputed &&
           evidence->nakamoto_selected_best_work &&
           evidence->utxo_sha3_verified &&
           evidence->mmb_flyclient_proof_verified &&
           evidence->chunk_hash_coverage_verified;
}

bool chain_evidence_controller_mark_block_evidence(
    struct chain_evidence_controller *authority,
    const struct uint256 *block_hash,
    const struct chain_evidence_record *evidence)
{
    char key[sizeof("cec.block_evidence.") + 64];
    if (!authority || !authority->ndb || !block_hash || !evidence ||
        !block_evidence_key(block_hash, key))
        return false;
    return persist_evidence(authority, key, evidence);
}

static bool persist_state(struct chain_evidence_controller *a,
                          enum chain_evidence_controller_state state)
{
    const char *name = chain_evidence_controller_state_name(state);
    if (!a || !a->ndb)
        return false;
    if (!node_db_state_set(a->ndb, "cec.sync_state", name, strlen(name) + 1))
        return false;
    a->state = state;
    return true;
}

static bool persist_i64(struct chain_evidence_controller *a, const char *key, int64_t v)
{
    return a && a->ndb && node_db_state_set_int(a->ndb, key, v);
}

static bool persist_blob(struct chain_evidence_controller *a, const char *key,
                         const void *value, size_t len)
{
    return a && a->ndb && node_db_state_set(a->ndb, key, value, len);
}

static enum chain_evidence_controller_state parse_state(const char *name)
{
    if (!name || !*name)
        return CEC_EMPTY;
    for (int i = 0; i < CEC_NUM_STATES; i++) {
        if (strcmp(name, chain_evidence_controller_state_name((enum chain_evidence_controller_state)i)) == 0)
            return (enum chain_evidence_controller_state)i;
    }
    return CEC_EMPTY;
}

void chain_evidence_controller_init(struct chain_evidence_controller *authority,
                         struct node_db *ndb,
                         struct chain_state_repository *csr)
{
    if (!authority)
        return;
    memset(authority, 0, sizeof(*authority));
    authority->ndb = ndb;
    authority->csr = csr ? csr : csr_instance();
    authority->state = CEC_EMPTY;
    (void)chain_evidence_controller_load_state(authority);
}

enum chain_evidence_controller_state chain_evidence_controller_load_state(
    struct chain_evidence_controller *authority)
{
    char mode[64];
    size_t len = 0;

    if (!authority || !authority->ndb)
        return CEC_EMPTY;

    memset(mode, 0, sizeof(mode));
    if (node_db_state_get(authority->ndb, "cec.sync_state",
                          mode, sizeof(mode) - 1, &len)) {
        mode[sizeof(mode) - 1] = '\0';
        authority->state = parse_state(mode);
    }

    memset(authority->contradiction_reason, 0, sizeof(authority->contradiction_reason));
    (void)node_db_state_get(authority->ndb, "cec.contradiction_reason",
                            authority->contradiction_reason,
                            sizeof(authority->contradiction_reason) - 1, &len);
    return authority->state;
}

void chain_evidence_controller_freeze(struct chain_evidence_controller *authority,
                           const char *reason)
{
    if (!authority)
        return;
    authority->state = CEC_CONTRADICTION_FROZEN;
    snprintf(authority->contradiction_reason, sizeof(authority->contradiction_reason),
             "%s", reason ? reason : "unspecified");
    if (authority->ndb) {
        (void)node_db_state_set(authority->ndb, "cec.contradiction_reason",
                                authority->contradiction_reason,
                                strlen(authority->contradiction_reason) + 1);
        (void)persist_state(authority, CEC_CONTRADICTION_FROZEN);
    }
}

enum chain_evidence_controller_result chain_evidence_controller_import_snapshot_evidence(
    struct chain_evidence_controller *authority,
    const struct chain_evidence_controller_snapshot_meta *snapshot)
{
    if (!authority || !snapshot)
        return CEC_REJECTED_NULL_ARG;
    if (authority->state == CEC_CONTRADICTION_FROZEN)
        return CEC_REJECTED_FROZEN;
    if (snapshot->anchor_height < 0 ||
        !u256_nonzero(&snapshot->anchor_hash) ||
        !u256_nonzero(&snapshot->utxo_sha3) ||
        snapshot->utxo_count == 0 ||
        !bytes32_nonzero(snapshot->chainwork) ||
        !bytes32_nonzero(snapshot->mmb_root) ||
        snapshot->schema_version == 0 ||
        snapshot->finality_depth == 0 ||
        !chain_evidence_record_has_snapshot_required(&snapshot->verified)) {
        chain_evidence_controller_freeze(authority, "snapshot manifest missing verified evidence");
        return CEC_REJECTED_BAD_PROOF;
    }

    struct chain_evidence_record header = {
        .header_ancestry_linked = snapshot->verified.header_ancestry_linked,
        .chainwork_recomputed = snapshot->verified.chainwork_recomputed,
        .nakamoto_selected_best_work =
            snapshot->verified.nakamoto_selected_best_work,
    };

    if (!persist_i64(authority, "cec.snapshot_anchor_height",
                     snapshot->anchor_height) ||
        !persist_blob(authority, "cec.snapshot_anchor_hash",
                      snapshot->anchor_hash.data, 32) ||
        !persist_blob(authority, "cec.snapshot_utxo_sha3",
                      snapshot->utxo_sha3.data, 32) ||
        !persist_i64(authority, "cec.snapshot_validated", 0) ||
        !persist_i64(authority, "cec.background_validation_height", 0) ||
        !persist_blob(authority, "cec.snapshot_chainwork",
                      snapshot->chainwork, 32) ||
        !persist_blob(authority, "cec.snapshot_mmb_root",
                      snapshot->mmb_root, 32) ||
        !persist_i64(authority, "cec.snapshot_utxo_count",
                     (int64_t)snapshot->utxo_count) ||
        !persist_i64(authority, "cec.snapshot_schema_version",
                     snapshot->schema_version) ||
        !persist_i64(authority, "cec.snapshot_finality_depth",
                     snapshot->finality_depth) ||
        !persist_evidence(authority, "cec.header_chain_evidence", &header)) {
        chain_evidence_controller_freeze(authority, "snapshot metadata persistence failed");
        return CEC_REJECTED_PERSIST;
    }
    if (snapshot->producer) {
        if (!persist_blob(authority, "cec.snapshot_producer",
                          snapshot->producer, strlen(snapshot->producer) + 1)) {
            chain_evidence_controller_freeze(authority, "snapshot producer persistence failed");
            return CEC_REJECTED_PERSIST;
        }
    }
    if (!persist_evidence(authority, "cec.snapshot_evidence",
                          &snapshot->verified) ||
        !persist_state(authority, CEC_SNAPSHOT_UTXO_HASH_VERIFIED))
        return CEC_REJECTED_PERSIST;
    return CEC_OK;
}

static bool state_allows_tip_promotion(enum chain_evidence_controller_state state)
{
    return state == CEC_EMPTY ||
           state == CEC_HEADERS_WORK_VALIDATED ||
           state == CEC_SNAPSHOT_UTXO_HASH_VERIFIED ||
           state == CEC_TIP_FOLLOWING ||
           state == CEC_BACKGROUND_VALIDATING ||
           state == CEC_FULLY_VALIDATED;
}

static void cec_restore_csr_view(struct chain_state_repository *csr,
                                 struct block_index *old_tip,
                                 struct block_index *old_header,
                                 const struct uint256 *old_coins_best)
{
    if (!csr)
        return;
    if (csr->chain_active)
        (void)active_chain_set_tip(csr->chain_active, old_tip);
    if (csr->pindex_best_hdr)
        *csr->pindex_best_hdr = old_header;
    if (csr->coins_tip && old_coins_best)
        coins_view_cache_set_best_block(csr->coins_tip, old_coins_best);
}

enum chain_evidence_controller_result chain_evidence_controller_promote_tip(
    struct chain_evidence_controller *authority,
    const struct chain_evidence_controller_tip_request *request)
{
    if (!authority || !request || !request->new_tip ||
        !request->new_tip->phashBlock)
        return CEC_REJECTED_NULL_ARG;
    if (authority->state == CEC_CONTRADICTION_FROZEN)
        return CEC_REJECTED_FROZEN;
    if (!state_allows_tip_promotion(authority->state))
        return CEC_REJECTED_BAD_STATE;
    if (!chain_evidence_record_has_block_index_required(&request->verified)) {
        chain_evidence_controller_freeze(authority,
            "tip promotion missing block-index evidence: header_ancestry_linked,chainwork_recomputed,nakamoto_selected_best_work,block_bytes_hash_checked");
        return CEC_REJECTED_INCOMPLETE_INDEX_EVIDENCE;
    }
    if (request->utxo_max_height > request->new_tip->nHeight) {
        chain_evidence_controller_freeze(authority,
            "utxo height exceeds evidenced block-index height");
        return CEC_REJECTED_UTXO_AHEAD_OF_INDEX;
    }
    if (!authority->ndb) {
        chain_evidence_controller_freeze(authority,
            "tip promotion has no evidence persistence target");
        return CEC_REJECTED_PERSIST;
    }

    struct block_index *old_tip = NULL;
    struct block_index *old_header = NULL;
    struct uint256 old_coins_best;
    memset(&old_coins_best, 0, sizeof(old_coins_best));
    if (authority->csr) {
        if (authority->csr->chain_active)
            old_tip = active_chain_tip(authority->csr->chain_active);
        if (authority->csr->pindex_best_hdr)
            old_header = *authority->csr->pindex_best_hdr;
        if (authority->csr->coins_tip)
            coins_view_cache_get_best_block(authority->csr->coins_tip,
                                            &old_coins_best);
    }

    enum chain_evidence_controller_state old_state = authority->state;
    enum chain_evidence_controller_state next_state = old_state;
    if (old_state == CEC_EMPTY ||
        old_state == CEC_SNAPSHOT_UTXO_HASH_VERIFIED ||
        old_state == CEC_HEADERS_WORK_VALIDATED)
        next_state = CEC_TIP_FOLLOWING;

    DB_TXN_SCOPE(txn, authority->ndb, "cec.promote_tip");
    if (!txn) {
        chain_evidence_controller_freeze(authority,
            "tip promotion evidence transaction failed");
        return CEC_REJECTED_PERSIST;
    }

    bool persisted =
        chain_evidence_controller_mark_block_evidence(
            authority, request->new_tip->phashBlock, &request->verified) &&
        persist_blob(authority, "cec.active_tip_hash",
                     request->new_tip->phashBlock->data, 32) &&
        persist_i64(authority, "cec.active_tip_height",
                    request->new_tip->nHeight) &&
        persist_i64(authority, "cec.coins_best_block_height",
                    request->new_tip->nHeight) &&
        persist_i64(authority, "cec.utxo_max_height",
                    request->utxo_max_height) &&
        persist_evidence(authority, "cec.block_index_evidence_state",
                         &request->verified) &&
        persist_evidence(authority, "cec.active_tip_evidence",
                         &request->verified);
    if (persisted && next_state != old_state) {
        const char *name = chain_evidence_controller_state_name(next_state);
        persisted = persist_blob(authority, "cec.sync_state",
                                 name, strlen(name) + 1);
    }
    if (!persisted) {
        chain_evidence_controller_freeze(authority,
            "tip promotion evidence persistence failed before csr commit");
        authority->state = old_state;
        return CEC_REJECTED_PERSIST;
    }

    /* Wave 9e: when the new tip is below the current active tip, this
     * "promotion" is in fact a disconnect — used by disconnect_tip
     * during sibling-fork reorg recovery. The evidence controller has
     * already vetted the new tip via chain_evidence_record; pass that
     * authority through to CSR as a rollback authorization so the
     * UTXO-orphan-rows guard (csr step 7) doesn't reject the legitimate
     * rollback. Without this, sibling_fork_rollback wedges at the
     * disconnect step with utxo_delta_too_big. */
    struct chain_state_rollback_authorization rollback_auth = {
        .source = CSR_ROLLBACK_SOURCE_VALIDATION,
        .decision = POLICY_ALLOW,
        .from_height = old_tip ? old_tip->nHeight : -1,
        .to_height = request->new_tip->nHeight,
        .max_depth = INT64_MAX,
        .evidence_class = "evidence_controller_vouched_rollback",
        .reason = request->reason ? request->reason
                                  : "chain_evidence_controller.promote_tip",
    };
    bool is_rollback = (old_tip != NULL &&
                        request->new_tip->nHeight < old_tip->nHeight);

    struct chain_state_commit commit = {
        .new_tip = request->new_tip,
        .new_coins_best = *request->new_tip->phashBlock,
        .expected_utxo_count = 0,
        .update_header_tip = request->update_header_tip,
        .persist_coins_best = true,
        .rollback_auth = is_rollback ? &rollback_auth : NULL,
        .wallet_scan_height = -1,
        .reason = request->reason ? request->reason : "chain_evidence_controller.promote_tip",
    };
    enum csr_result csr = csr_commit_tip(authority->csr, &commit);
    if (csr != CSR_OK) {
        char msg[160];
        snprintf(msg, sizeof(msg), "chain_state_repository rejected promotion: %s",
                 csr_result_name(csr));
        chain_evidence_controller_freeze(authority, msg);
        authority->state = old_state;
        return CEC_REJECTED_CSR;
    }

#ifdef ZCL_TESTING
    if (g_cec_test_fail_commit_after_csr) {
        g_cec_test_fail_commit_after_csr = false;
        chain_evidence_controller_freeze(authority,
            "test-forced tip promotion evidence transaction commit failure after csr commit");
        cec_restore_csr_view(authority->csr, old_tip, old_header,
                             &old_coins_best);
        authority->state = old_state;
        return CEC_REJECTED_PERSIST;
    }
#endif

    if (!db_txn_commit(txn)) {
        chain_evidence_controller_freeze(authority,
            "tip promotion evidence transaction commit failed after csr commit");
        cec_restore_csr_view(authority->csr, old_tip, old_header,
                             &old_coins_best);
        authority->state = old_state;
        return CEC_REJECTED_PERSIST;
    }
    authority->state = next_state;
    return CEC_OK;
}

enum chain_evidence_controller_result chain_evidence_controller_mark_background_progress(
    struct chain_evidence_controller *authority,
    int height)
{
    if (!authority)
        return CEC_REJECTED_NULL_ARG;
    if (authority->state == CEC_CONTRADICTION_FROZEN)
        return CEC_REJECTED_FROZEN;
    if (!persist_i64(authority, "cec.background_validation_height", height))
        return CEC_REJECTED_PERSIST;
    if (authority->state == CEC_TIP_FOLLOWING ||
        authority->state == CEC_SNAPSHOT_UTXO_HASH_VERIFIED) {
        if (!persist_state(authority, CEC_BACKGROUND_VALIDATING))
            return CEC_REJECTED_PERSIST;
    }
    return CEC_OK;
}

enum chain_evidence_controller_result chain_evidence_controller_mark_fully_validated(
    struct chain_evidence_controller *authority,
    const struct uint256 *utxo_sha3)
{
    struct uint256 expected;
    size_t len = 0;

    if (!authority || !utxo_sha3)
        return CEC_REJECTED_NULL_ARG;
    if (authority->state == CEC_CONTRADICTION_FROZEN)
        return CEC_REJECTED_FROZEN;
    memset(&expected, 0, sizeof(expected));
    if (!authority->ndb ||
        !node_db_state_get(authority->ndb, "cec.snapshot_utxo_sha3",
                           expected.data, 32, &len) ||
        len != 32 ||
        memcmp(expected.data, utxo_sha3->data, 32) != 0) {
        chain_evidence_controller_freeze(authority,
                              "background validation UTXO commitment mismatch");
        return CEC_REJECTED_BAD_PROOF;
    }
    struct chain_evidence_record snapshot_evidence;
    (void)load_evidence(authority->ndb, "cec.snapshot_evidence",
                        &snapshot_evidence);
    snapshot_evidence.full_validation_complete = true;
    if (!persist_i64(authority, "cec.snapshot_validated", 1) ||
        !persist_evidence(authority, "cec.snapshot_evidence",
                          &snapshot_evidence) ||
        !persist_state(authority, CEC_FULLY_VALIDATED))
        return CEC_REJECTED_PERSIST;
    return CEC_OK;
}

static int state_get_i32(struct node_db *ndb, const char *key, int def)
{
    int64_t v = def;
    if (ndb)
        (void)node_db_state_get_int(ndb, key, &v);
    return (int)v;
}

void chain_evidence_controller_snapshot(struct chain_evidence_controller *authority,
                             struct chain_evidence_controller_view *out)
{
    struct chain_state_view csv;

    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->active_tip_height = -1;
    out->header_tip_height = -1;
    out->snapshot_anchor_height = -1;
    out->background_validation_height = -1;
    out->utxo_max_height = -1;
    out->coins_best_block_height = -1;
    if (!authority)
        return;

    out->state = chain_evidence_controller_load_state(authority);
    memset(&csv, 0, sizeof(csv));
    csr_snapshot(authority->csr, &csv);
    out->active_tip_height = csv.tip_height;
    out->header_tip_height = csv.header_height;
    out->snapshot_anchor_height =
        state_get_i32(authority->ndb, "cec.snapshot_anchor_height", -1);
    out->background_validation_height =
        state_get_i32(authority->ndb, "cec.background_validation_height", -1);
    out->utxo_max_height =
        state_get_i32(authority->ndb, "cec.utxo_max_height", -1);
    out->coins_best_block_height =
        state_get_i32(authority->ndb, "cec.coins_best_block_height", -1);
    (void)load_evidence(authority->ndb, "cec.block_index_evidence_state",
                        &out->block_index_evidence_state);
    (void)load_evidence(authority->ndb, "cec.active_tip_evidence",
                        &out->active_tip_evidence);
    (void)load_evidence(authority->ndb, "cec.snapshot_evidence",
                        &out->snapshot_evidence);
    (void)load_evidence(authority->ndb, "cec.header_chain_evidence",
                        &out->header_chain_evidence);
    snprintf(out->contradiction_reason, sizeof(out->contradiction_reason),
             "%s", authority->contradiction_reason);
}
