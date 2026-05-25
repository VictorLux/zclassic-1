/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_SERVICES_CHAIN_EVIDENCE_STORE_H
#define ZCL_SERVICES_CHAIN_EVIDENCE_STORE_H

#include "services/chain_evidence_controller.h"

#include <stdbool.h>

bool chain_evidence_store_persist(
    struct chain_evidence_controller *authority,
    const char *key,
    const struct chain_evidence_record *evidence);

bool chain_evidence_store_load(struct node_db *ndb,
                               const char *key,
                               struct chain_evidence_record *out);

#endif /* ZCL_SERVICES_CHAIN_EVIDENCE_STORE_H */
