/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_SERVICES_UTXO_AUDIT_SERVICE_H
#define ZCL_SERVICES_UTXO_AUDIT_SERVICE_H

#include "models/database.h"
#include "util/result.h"

#include <stdbool.h>
#include <stdint.h>

enum utxo_audit_status {
    UTXO_AUDIT_LOCAL_ONLY = 0,
    UTXO_AUDIT_MATCH,
    UTXO_AUDIT_DRIFT,
    UTXO_AUDIT_ERROR,
};

struct utxo_audit_result {
    enum utxo_audit_status status;
    char local_sha3[65];
    char remote_sha3[65];
    char source[64];
    int32_t local_height;
    int32_t remote_height;
    uint64_t local_utxo_count;
    bool drift_detected;
    char error[128];
};

const char *utxo_audit_status_name(enum utxo_audit_status status);

struct zcl_result utxo_audit_local(struct node_db *ndb, int32_t height,
                                   struct utxo_audit_result *out);

struct zcl_result utxo_audit_compare_remote(struct node_db *ndb,
                                            const char *remote_sha3,
                                            int32_t remote_height,
                                            const char *source,
                                            struct utxo_audit_result *out);

#endif /* ZCL_SERVICES_UTXO_AUDIT_SERVICE_H */
