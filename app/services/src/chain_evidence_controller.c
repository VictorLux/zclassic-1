/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "services/chain_evidence_controller.h"

#include "models/database.h"
#include "models/block.h"
#include "models/db_txn.h"
#include "event/event.h"

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

const char *chain_evidence_source_class_name(enum chain_evidence_source_class source)
{
    switch (source) {
    case CEC_SOURCE_CLASS_UNKNOWN:         return "unknown";
    case CEC_SOURCE_CLASS_NATIVE_P2P:      return "native_p2p";
    case CEC_SOURCE_CLASS_SNAPSHOT:        return "snapshot";
    case CEC_SOURCE_CLASS_LOCAL_IMPORT:    return "local_import";
    case CEC_SOURCE_CLASS_LEGACY_ADVISORY: return "legacy_advisory";
    }
    return "unknown";
}

const char *chain_evidence_publish_state_name(enum chain_evidence_publish_state state)
{
    switch (state) {
    case CEC_PUBLISH_NOT_PUBLISHABLE:        return "not_publishable";
    case CEC_PUBLISH_LOCAL_EVIDENCE:         return "publishable_local_evidence";
    case CEC_PUBLISH_FROZEN_CONTRADICTION:   return "frozen_contradiction";
    }
    return "unknown";
}

#define CEC_RECORD_MAGIC 0x43454345u
#define CEC_RECORD_VERSION 2u

struct persisted_evidence_record {
    uint32_t magic;
    uint32_t version;
    uint32_t source_class;
    uint32_t publish_state;
    uint8_t header_ancestry_linked;
    uint8_t chainwork_recomputed;
    uint8_t nakamoto_selected_best_work;
    uint8_t block_bytes_hash_checked;
    uint8_t utxo_sha3_verified;
    uint8_t mmb_flyclient_proof_verified;
    uint8_t chunk_hash_coverage_verified;
    uint8_t full_validation_complete;
};

struct persisted_evidence_record_v1 {
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
    out->source_class = (uint32_t)in->source_class;
    out->publish_state = (uint32_t)in->publish_state;
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
    if (!buf || !out || len < sizeof(struct persisted_evidence_record_v1))
        return false;
    memset(out, 0, sizeof(*out));
    if (len == sizeof(struct persisted_evidence_record_v1)) {
        const struct persisted_evidence_record_v1 *v1 = buf;
        if (v1->magic != CEC_RECORD_MAGIC || v1->version != 1u)
            return false;
        out->publish_state = CEC_PUBLISH_LOCAL_EVIDENCE;
        out->header_ancestry_linked = v1->header_ancestry_linked != 0;
        out->chainwork_recomputed = v1->chainwork_recomputed != 0;
        out->nakamoto_selected_best_work =
            v1->nakamoto_selected_best_work != 0;
        out->block_bytes_hash_checked = v1->block_bytes_hash_checked != 0;
        out->utxo_sha3_verified = v1->utxo_sha3_verified != 0;
        out->mmb_flyclient_proof_verified =
            v1->mmb_flyclient_proof_verified != 0;
        out->chunk_hash_coverage_verified =
            v1->chunk_hash_coverage_verified != 0;
        out->full_validation_complete = v1->full_validation_complete != 0;
        if (out->utxo_sha3_verified &&
            out->mmb_flyclient_proof_verified &&
            out->chunk_hash_coverage_verified)
            out->source_class = CEC_SOURCE_CLASS_SNAPSHOT;
        else if (out->block_bytes_hash_checked)
            out->source_class = CEC_SOURCE_CLASS_NATIVE_P2P;
        return true;
    }
    if (len != sizeof(*p) ||
        p->magic != CEC_RECORD_MAGIC ||
        p->version != CEC_RECORD_VERSION)
        return false;
    if (p->version >= 2u) {
        out->source_class = (enum chain_evidence_source_class)p->source_class;
        out->publish_state = (enum chain_evidence_publish_state)p->publish_state;
    }
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

static bool load_u256(struct node_db *ndb, const char *key,
                      struct uint256 *out)
{
    size_t len = 0;
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    return ndb && node_db_state_get(ndb, key, out->data, 32, &len) &&
           len == 32;
}

static bool u256_equal(const struct uint256 *a, const struct uint256 *b)
{
    return a && b && memcmp(a->data, b->data, 32) == 0;
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
           evidence->publish_state == CEC_PUBLISH_LOCAL_EVIDENCE &&
           evidence->header_ancestry_linked &&
           evidence->chainwork_recomputed &&
           evidence->nakamoto_selected_best_work &&
           evidence->block_bytes_hash_checked;
}

bool chain_evidence_record_has_snapshot_required(
    const struct chain_evidence_record *evidence)
{
    return evidence &&
           evidence->source_class == CEC_SOURCE_CLASS_SNAPSHOT &&
           evidence->publish_state == CEC_PUBLISH_LOCAL_EVIDENCE &&
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

static int state_get_i32(struct node_db *ndb, const char *key, int def);

static bool cec_tip_ancestry_linked(struct chain_state_repository *csr,
                                    struct block_index *tip)
{
    if (!tip || !tip->phashBlock)
        return false;
    if (csr && csr->block_map) {
        struct block_index *found =
            block_map_find(csr->block_map, tip->phashBlock);
        if (found != tip)
            return false;
    }
    for (struct block_index *p = tip; p; p = p->pprev) {
        if (!p->phashBlock)
            return false;
        if (csr && csr->block_map &&
            block_map_find(csr->block_map, p->phashBlock) != p)
            return false;
        if (p->nHeight == 0)
            return p->pprev == NULL;
        if (!p->pprev || p->pprev->nHeight != p->nHeight - 1)
            return false;
    }
    return false;
}

static bool cec_reconstruct_active_tip_evidence(
    struct chain_evidence_controller *authority,
    struct block_index *active_tip,
    const struct chain_state_view *csv)
{
    if (!authority || !authority->ndb || !active_tip || !active_tip->phashBlock ||
        !csv || csv->tip_height < 0)
        return false;
    if (!cec_tip_ancestry_linked(authority->csr, active_tip))
        return false;
    if (csv->header_height >= 0 && csv->header_height < csv->tip_height)
        return false;
    if (!u256_equal(&csv->tip_hash, active_tip->phashBlock))
        return false;
    if (!u256_equal(&csv->coins_best_block, active_tip->phashBlock))
        return false;
    if (csv->sql_max_height >= 0 && csv->sql_max_height < csv->tip_height)
        return false;
    if (arith_uint256_is_zero(&active_tip->nChainWork))
        return false;

    struct chain_evidence_record reconstructed = {
        .source_class = CEC_SOURCE_CLASS_NATIVE_P2P,
        .publish_state = CEC_PUBLISH_LOCAL_EVIDENCE,
        .header_ancestry_linked = true,
        .chainwork_recomputed = true,
        .nakamoto_selected_best_work = true,
        .block_bytes_hash_checked = true,
    };
    return chain_evidence_controller_mark_block_evidence(
               authority, active_tip->phashBlock, &reconstructed) &&
           persist_blob(authority, "cec.active_tip_hash",
                        active_tip->phashBlock->data, 32) &&
           persist_i64(authority, "cec.active_tip_height",
                       active_tip->nHeight) &&
           persist_i64(authority, "cec.coins_best_block_height",
                       active_tip->nHeight) &&
           persist_i64(authority, "cec.utxo_max_height",
                       active_tip->nHeight) &&
           persist_i64(authority, "cec.publish_state",
                       CEC_PUBLISH_LOCAL_EVIDENCE) &&
           persist_i64(authority, "cec.active_tip_source_class",
                       CEC_SOURCE_CLASS_NATIVE_P2P) &&
           persist_i64(authority, "cec.repaired_active_tip_evidence", 1) &&
           persist_evidence(authority, "cec.block_index_evidence_state",
                            &reconstructed) &&
           persist_evidence(authority, "cec.active_tip_evidence",
                            &reconstructed);
}

static void chain_evidence_controller_reconcile_startup(
    struct chain_evidence_controller *authority)
{
    if (!authority || !authority->ndb || !authority->csr ||
        authority->state == CEC_CONTRADICTION_FROZEN)
        return;

    struct chain_state_view csv;
    memset(&csv, 0, sizeof(csv));
    csr_snapshot(authority->csr, &csv);
    if (csv.tip_height < 0)
        return;

    struct block_index *active_tip = authority->csr->chain_active
        ? active_chain_tip(authority->csr->chain_active) : NULL;
    if (!active_tip || !active_tip->phashBlock)
        return;

    struct uint256 persisted_hash;
    bool has_persisted_hash =
        load_u256(authority->ndb, "cec.active_tip_hash", &persisted_hash);
    int persisted_height = state_get_i32(authority->ndb,
                                         "cec.active_tip_height", -1);
    struct chain_evidence_record active_evidence;
    bool has_active_evidence =
        load_evidence(authority->ndb, "cec.active_tip_evidence",
                      &active_evidence);

    if (has_persisted_hash &&
        !u256_equal(&persisted_hash, active_tip->phashBlock)) {
        chain_evidence_controller_freeze(authority,
                                         "active_tip_hash_mismatch");
        return;
    }
    if (persisted_height >= 0 && persisted_height != active_tip->nHeight) {
        chain_evidence_controller_freeze(authority,
                                         "active_tip_height_mismatch");
        return;
    }
    if (!u256_equal(&csv.coins_best_block, active_tip->phashBlock)) {
        chain_evidence_controller_freeze(authority,
                                         "csr_cursor_mismatch");
        return;
    }
    /* Derived-state lag is not a contradiction. After a clean shutdown the
     * persisted pindex_best_header / blocks-table max can be behind the
     * active tip; the active tip is the source of truth. Self-heal
     * pindex_best_header forward and log a one-liner; the lagging signal
     * will catch up via P2P / projection. A freeze here was sticky and
     * required manual node.db surgery to clear. */
    if (csv.header_height >= 0 && csv.header_height < active_tip->nHeight) {
        fprintf(stderr,  // obs-ok:cec-self-heal-header-tip
                "[cec] reconcile_startup: pindex_best_header h=%d behind "
                "active_tip h=%d — advancing in-memory tracker\n",
                csv.header_height, active_tip->nHeight);
        if (authority->csr && authority->csr->pindex_best_hdr)
            *authority->csr->pindex_best_hdr = active_tip;
    }
    if (csv.sql_max_height >= 0 && csv.sql_max_height < active_tip->nHeight) {
        fprintf(stderr,  // obs-ok:cec-self-heal-sql-max
                "[cec] reconcile_startup: blocks.max_height=%lld behind "
                "active_tip h=%d — projection will backfill\n",
                (long long)csv.sql_max_height, active_tip->nHeight);
    }

    if (!has_active_evidence ||
        !chain_evidence_record_has_block_index_required(&active_evidence)) {
        if (!cec_reconstruct_active_tip_evidence(authority, active_tip, &csv)) {
            chain_evidence_controller_freeze(authority,
                                             "missing_active_tip_evidence");
        }
    }
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
    chain_evidence_controller_reconcile_startup(authority);
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
        (void)node_db_state_set_int(authority->ndb, "cec.publish_state",
                                    CEC_PUBLISH_FROZEN_CONTRADICTION);
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
        .source_class = CEC_SOURCE_CLASS_SNAPSHOT,
        .publish_state = CEC_PUBLISH_LOCAL_EVIDENCE,
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
        !persist_i64(authority, "cec.publish_state",
                     CEC_PUBLISH_NOT_PUBLISHABLE) ||
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
    (void)csr_restore_in_memory_view(csr, old_tip, old_header,
                                     old_coins_best);
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
    struct chain_evidence_record verified = request->verified;
    if (verified.source_class == CEC_SOURCE_CLASS_UNKNOWN)
        verified.source_class = CEC_SOURCE_CLASS_NATIVE_P2P;
    if (verified.publish_state == CEC_PUBLISH_NOT_PUBLISHABLE)
        verified.publish_state = CEC_PUBLISH_LOCAL_EVIDENCE;

    if (!chain_evidence_record_has_block_index_required(&verified)) {
        /* Don't freeze the controller on a missing-flag record: during
         * genesis-up sync the evidence flags (header_ancestry_linked,
         * chainwork_recomputed, nakamoto_selected_best_work,
         * block_bytes_hash_checked) can be temporarily missing on a
         * re-arrival of a block whose evidence record was constructed
         * before all the flags were stamped — e.g. a stale cached
         * record from a prior reorg disconnect, or a worker-thread
         * race that constructs the record before block_index_integrity
         * has finished marking it. Permanent freeze on this transient
         * shape blocks the chain
         * forever. The actual integrity of new_tip is checked further
         * down by csr_validate_locked (tip-in-index, hash-match,
         * sql-cross-check) before the commit lands. Returning
         * INCOMPLETE_INDEX_EVIDENCE without freeze lets the caller
         * retry on the next pass once the evidence record is rebuilt. */
        fprintf(stderr,  // obs-ok:incomplete-evidence-transient
                "[cec] tip promotion missing block-index evidence "
                "(transient) h=%d ancestry=%d work=%d nakamoto=%d "
                "bytes=%d — controller stays in state=%s for retry\n",
                request->new_tip->nHeight,
                verified.header_ancestry_linked,
                verified.chainwork_recomputed,
                verified.nakamoto_selected_best_work,
                verified.block_bytes_hash_checked,
                chain_evidence_controller_state_name(authority->state));
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

    /* chain_advance opens a node.db transaction at its step 3 BEFORE
     * calling process_block_commit_tip which routes here. db_txn_begin
     * correctly refuses nesting and returns NULL. Detect the outer
     * transaction and skip our own DB_TXN_SCOPE: the persist calls
     * below will join the existing transaction, and the caller's
     * commit/rollback will atomically close both our evidence
     * persistence AND the block-index write.
     *
     * When no outer txn exists (e.g. standalone csr_commit_tip from a
     * test or boot anchor promote), open our own and commit at end.
     * The cleanup attribute auto-rollbacks our owned txn if we early-
     * return without calling db_txn_commit. db_txn_auto_rollback is a
     * no-op on NULL handles, so the outer-txn case is safe. */
    struct node_db_status _ndb_status = {0};
    if (authority->ndb)
        node_db_get_status(authority->ndb, &_ndb_status);
    bool outer_txn_present = _ndb_status.tx_open;
    __attribute__((cleanup(db_txn_auto_rollback)))
    struct db_txn *txn = NULL;
    if (!outer_txn_present) {
        txn = db_txn_begin(authority->ndb, "cec.promote_tip");
        if (!txn) {
            fprintf(stderr,  // obs-ok:transient-txn-failure
                    "[cec] tip promotion txn open failed (transient) h=%d\n",
                    request->new_tip->nHeight);
            return CEC_REJECTED_PERSIST;
        }
    }

    bool persisted =
        chain_evidence_controller_mark_block_evidence(
            authority, request->new_tip->phashBlock, &verified) &&
        persist_blob(authority, "cec.active_tip_hash",
                     request->new_tip->phashBlock->data, 32) &&
        persist_i64(authority, "cec.active_tip_height",
                    request->new_tip->nHeight) &&
        persist_i64(authority, "cec.coins_best_block_height",
                    request->new_tip->nHeight) &&
        persist_i64(authority, "cec.utxo_max_height",
                    request->utxo_max_height) &&
        persist_i64(authority, "cec.publish_state",
                    CEC_PUBLISH_LOCAL_EVIDENCE) &&
        persist_i64(authority, "cec.active_tip_source_class",
                    verified.source_class) &&
        persist_evidence(authority, "cec.block_index_evidence_state",
                         &verified) &&
        persist_evidence(authority, "cec.active_tip_evidence",
                         &verified);
    if (persisted && next_state != old_state) {
        const char *name = chain_evidence_controller_state_name(next_state);
        persisted = persist_blob(authority, "cec.sync_state",
                                 name, strlen(name) + 1);
    }
    if (!persisted) {
        /* Persistence failure is transient (SQLite contention,
         * mid-write commit clash, etc.) — NOT a chain-state
         * contradiction. Do not freeze: that sets
         * state=CEC_CONTRADICTION_FROZEN permanently, rejecting EVERY
         * subsequent commit with "(frozen)" and wedging the node on
         * the first sqlite hiccup. The wedge was reproducible on
         * fresh-datadir genesis-up sync at h=3 where the first persist
         * failed once and the chain never advanced again. Freeze is
         * reserved for true contradictions (evidence integrity
         * violations, snapshot/index disagreement). Caller retries on
         * REJECTED_PERSIST. */
        fprintf(stderr,  // obs-ok:transient-persist-failure-emits-event-below
                "[cec] tip promotion persist failure (transient) h=%d — "
                "controller stays in state=%s for retry\n",
                request->new_tip->nHeight,
                chain_evidence_controller_state_name(old_state));
        event_emitf(EV_CHAIN_TIP_REJECTED, 0,
                    "code=cec_persist_transient h=%d",
                    request->new_tip->nHeight);
        authority->state = old_state;
        return CEC_REJECTED_PERSIST;
    }

    /* When the new tip is below the current active tip, this
     * "promotion" is in fact a disconnect — used by disconnect_tip
     * during sibling-fork reorg recovery. The evidence controller has
     * already vetted the new tip via chain_evidence_record; pass that
     * authority through to CSR as a rollback authorization so the
     * UTXO-orphan-rows guard (csr step 7) doesn't reject the legitimate
     * rollback. Without this, sibling_fork_rollback wedges at the
     * disconnect step with utxo_delta_too_big.
     *
     * Use active_chain_height() rather than old_tip pointer for the
     * comparison. After a body-pull anchor promotion that walked back
     * through unlinked pprev pointers, c->chain[c->height] can be NULL
     * while c->height is non-negative — `active_chain_tip()` returns
     * NULL in that phantom state, but CSR step 7 still computes
     * from_height from `active_chain_height()` and fires the
     * orphan-rows guard. Comparing against the height field directly
     * closes the gap. */
    int old_active_height = (authority->csr && authority->csr->chain_active)
        ? active_chain_height(authority->csr->chain_active)
        : -1;
    struct chain_state_rollback_authorization rollback_auth = {
        .source = CSR_ROLLBACK_SOURCE_VALIDATION,
        .decision = POLICY_ALLOW,
        .from_height = (int64_t)old_active_height,
        .to_height = request->new_tip->nHeight,
        .max_depth = INT64_MAX,
        .evidence_class = "evidence_controller_vouched_rollback",
        .reason = request->reason ? request->reason
                                  : "chain_evidence_controller.promote_tip",
    };
    bool is_rollback = (old_active_height >= 0 &&
                        request->new_tip->nHeight < old_active_height);

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
        /* CSR rejections are typically transient or recoverable
         * (stale_index when block_index races ahead of active_chain,
         * utxo_delta_too_big when rollback auth missing — those
         * legitimate cases are fixed at the CSR layer). Freezing the
         * controller on any remaining CSR rejection wedges the node
         * for the rest of the process lifetime even though the
         * underlying issue might clear in the next pass. Restore
         * old_state and return CEC_REJECTED_CSR; the caller retries. */
        fprintf(stderr,  // obs-ok:csr-rejection-pre-existing-emit
                "[cec] csr rejected tip promotion h=%d reason=%s — "
                "controller stays in state=%s for retry\n",
                request->new_tip->nHeight,
                csr_result_name(csr),
                chain_evidence_controller_state_name(old_state));
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

    /* Commit our txn only if we opened it ourselves. The outer caller
     * (chain_advance) is responsible for committing its own txn after
     * our writes land in it. */
    if (txn && !db_txn_commit(txn)) {
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
    int64_t v = -1;

    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->active_tip_height = -1;
    out->header_tip_height = -1;
    out->persisted_active_tip_height = -1;
    out->snapshot_anchor_height = -1;
    out->background_validation_height = -1;
    out->utxo_max_height = -1;
    out->coins_best_block_height = -1;
    out->sqlite_max_height = -1;
    if (!authority)
        return;

    out->state = chain_evidence_controller_load_state(authority);
    memset(&csv, 0, sizeof(csv));
    csr_snapshot(authority->csr, &csv);
    out->active_tip_height = csv.tip_height;
    out->header_tip_height = csv.header_height;
    out->sqlite_max_height = (int)csv.sql_max_height;
    out->coins_best_block_hash = csv.coins_best_block;
    out->has_coins_best_block_hash = u256_nonzero(&csv.coins_best_block);
    out->active_tip_hash = csv.tip_hash;
    out->has_active_tip_hash = u256_nonzero(&csv.tip_hash);
    if (authority->csr && authority->csr->pindex_best_hdr &&
        *authority->csr->pindex_best_hdr &&
        (*authority->csr->pindex_best_hdr)->phashBlock) {
        out->header_tip_hash =
            *(*authority->csr->pindex_best_hdr)->phashBlock;
        out->has_header_tip_hash = true;
    }
    if (load_u256(authority->ndb, "cec.active_tip_hash",
                  &out->persisted_active_tip_hash))
        out->has_persisted_active_tip_hash = true;
    out->persisted_active_tip_height =
        state_get_i32(authority->ndb, "cec.active_tip_height", -1);
    out->snapshot_anchor_height =
        state_get_i32(authority->ndb, "cec.snapshot_anchor_height", -1);
    out->background_validation_height =
        state_get_i32(authority->ndb, "cec.background_validation_height", -1);
    out->utxo_max_height =
        state_get_i32(authority->ndb, "cec.utxo_max_height", -1);
    out->coins_best_block_height =
        state_get_i32(authority->ndb, "cec.coins_best_block_height", -1);
    if (authority->ndb &&
        node_db_state_get_int(authority->ndb, "cec.active_tip_source_class",
                              &v))
        out->active_tip_source_class = (enum chain_evidence_source_class)v;
    v = CEC_PUBLISH_NOT_PUBLISHABLE;
    if (authority->ndb &&
        node_db_state_get_int(authority->ndb, "cec.publish_state", &v))
        out->publish_state = (enum chain_evidence_publish_state)v;
    v = 0;
    if (authority->ndb &&
        node_db_state_get_int(authority->ndb,
                              "cec.repaired_active_tip_evidence", &v))
        out->repaired_active_tip_evidence = v != 0;
    (void)load_evidence(authority->ndb, "cec.block_index_evidence_state",
                        &out->block_index_evidence_state);
    (void)load_evidence(authority->ndb, "cec.active_tip_evidence",
                        &out->active_tip_evidence);
    (void)load_evidence(authority->ndb, "cec.snapshot_evidence",
                        &out->snapshot_evidence);
    (void)load_evidence(authority->ndb, "cec.header_chain_evidence",
                        &out->header_chain_evidence);
    if (out->active_tip_source_class == CEC_SOURCE_CLASS_UNKNOWN &&
        out->active_tip_evidence.source_class != CEC_SOURCE_CLASS_UNKNOWN)
        out->active_tip_source_class = out->active_tip_evidence.source_class;
    if (out->publish_state == CEC_PUBLISH_NOT_PUBLISHABLE &&
        out->active_tip_evidence.publish_state != CEC_PUBLISH_NOT_PUBLISHABLE)
        out->publish_state = out->active_tip_evidence.publish_state;
    snprintf(out->contradiction_reason, sizeof(out->contradiction_reason),
             "%s", authority->contradiction_reason);

    out->missing_active_tip_evidence =
        out->active_tip_height >= 0 &&
        !chain_evidence_record_has_block_index_required(
            &out->active_tip_evidence);
    out->publish_state_not_local =
        out->active_tip_height >= 0 &&
        out->publish_state != CEC_PUBLISH_LOCAL_EVIDENCE;
    out->active_tip_hash_mismatch =
        out->has_active_tip_hash &&
        out->has_persisted_active_tip_hash &&
        !u256_equal(&out->active_tip_hash, &out->persisted_active_tip_hash);
    out->csr_cursor_mismatch =
        out->has_active_tip_hash &&
        out->has_coins_best_block_hash &&
        !u256_equal(&out->active_tip_hash, &out->coins_best_block_hash);

    if (out->state == CEC_CONTRADICTION_FROZEN) {
        snprintf(out->health_reason, sizeof(out->health_reason),
                 "%s", out->contradiction_reason[0]
                           ? out->contradiction_reason
                           : "chain_evidence_contradiction");
    } else if (out->active_tip_hash_mismatch) {
        snprintf(out->health_reason, sizeof(out->health_reason),
                 "active_tip_hash_mismatch");
    } else if (out->csr_cursor_mismatch) {
        snprintf(out->health_reason, sizeof(out->health_reason),
                 "csr_cursor_mismatch");
    } else if (out->publish_state_not_local) {
        snprintf(out->health_reason, sizeof(out->health_reason),
                 "publish_state_not_local");
    } else if (out->missing_active_tip_evidence) {
        snprintf(out->health_reason, sizeof(out->health_reason),
                 "missing_active_tip_evidence");
    }
}
