/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "services/utxo_audit_service.h"

#include "coins/utxo_commitment.h"
#include "encoding/utilstrencodings.h"
#include "event/event.h"
#include "util/log_macros.h"

#include <stdio.h>
#include <string.h>

static bool is_sha3_hex(const char *hex)
{
    return hex && strlen(hex) == 64 && IsHex(hex);
}

const char *utxo_audit_status_name(enum utxo_audit_status status)
{
    switch (status) {
    case UTXO_AUDIT_LOCAL_ONLY: return "local_only";
    case UTXO_AUDIT_MATCH:      return "match";
    case UTXO_AUDIT_DRIFT:      return "drift";
    case UTXO_AUDIT_ERROR:      return "error";
    default:                    return "unknown";
    }
}

bool utxo_audit_local(struct node_db *ndb, int32_t height,
                      struct utxo_audit_result *out)
{
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    out->status = UTXO_AUDIT_ERROR;

    if (!ndb || !ndb->open || !ndb->db) {
        snprintf(out->error, sizeof(out->error), "database not available");
        LOG_FAIL("utxo_audit", "local audit without open database");
    }

    uint8_t local_hash[32];
    uint64_t count = 0;
    utxo_commitment_sha3_compute(ndb->db, local_hash, &count);
    HexStr(local_hash, 32, false, out->local_sha3, sizeof(out->local_sha3));
    out->local_utxo_count = count;
    out->local_height = height;
    out->status = UTXO_AUDIT_LOCAL_ONLY;

    (void)node_db_state_set(ndb, "utxo_audit_last_local_sha3",
                            out->local_sha3, strlen(out->local_sha3));
    (void)node_db_state_set_int(ndb, "utxo_audit_last_height",
                                (int64_t)height);
    return true;
}

bool utxo_audit_compare_remote(struct node_db *ndb,
                               const char *remote_sha3,
                               int32_t remote_height,
                               const char *source,
                               struct utxo_audit_result *out)
{
    if (!out)
        return false;
    if (!utxo_audit_local(ndb, remote_height, out))
        return false;

    if (!is_sha3_hex(remote_sha3)) {
        out->status = UTXO_AUDIT_ERROR;
        snprintf(out->error, sizeof(out->error), "remote_sha3 must be 64 hex chars");
        LOG_FAIL("utxo_audit", "invalid remote sha3");
    }

    snprintf(out->remote_sha3, sizeof(out->remote_sha3), "%s", remote_sha3);
    snprintf(out->source, sizeof(out->source), "%s",
             source && source[0] ? source : "peer-commitment");
    out->remote_height = remote_height;

    out->drift_detected = strcmp(out->local_sha3, out->remote_sha3) != 0;
    out->status = out->drift_detected ? UTXO_AUDIT_DRIFT : UTXO_AUDIT_MATCH;
    (void)node_db_state_set_int(ndb, "utxo_drift_detected",
                                out->drift_detected ? 1 : 0);
    (void)node_db_state_set(ndb, "utxo_audit_last_remote_sha3",
                            out->remote_sha3, strlen(out->remote_sha3));

    if (out->drift_detected) {
        event_emitf(EV_UTXO_DRIFT_DETECTED, 0,
                    "local_sha3=%s remote_sha3=%s height=%d source=%s",
                    out->local_sha3, out->remote_sha3, remote_height,
                    out->source);
    } else {
        event_emitf(EV_UTXO_AUDIT_OK, 0,
                    "sha3=%s height=%d source=%s utxos=%llu",
                    out->local_sha3, remote_height, out->source,
                    (unsigned long long)out->local_utxo_count);
    }

    return true;
}
