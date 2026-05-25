/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "services/chain_evidence_controller.h"

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
