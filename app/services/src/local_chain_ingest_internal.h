/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Internal shared declarations for the local_chain_ingest module.
 *
 * The module is split across two translation units:
 *
 *   local_chain_ingest.c             — shared state, progress tracking,
 *                                      result-name table, detector,
 *                                      evidence-prefix accessors,
 *                                      zcl_state JSON dump.
 *
 *   local_chain_ingest_fastimport.c  — the three-phase fastimport
 *                                      pipeline: SHA3 window verify of
 *                                      mmap'd blk*.dat, LevelDB
 *                                      chainstate import, per-block
 *                                      legacy body-pull + chain_advance.
 *                                      Quarantined here because the
 *                                      -fastimport entry point is being
 *                                      demoted to advanced / diagnostic
 *                                      in cold-start consolidation.
 *
 * This header is for INTERNAL use only — callers go through the public
 * services/local_chain_ingest.h. Nothing in this file should be visible
 * outside the local_chain_ingest translation units.
 */

#ifndef ZCL_SERVICES_LOCAL_CHAIN_INGEST_INTERNAL_H
#define ZCL_SERVICES_LOCAL_CHAIN_INGEST_INTERNAL_H

#include "services/local_chain_ingest.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/* Shared runtime state for the ingest run. Writers (the running ingest
 * thread, defined in *_fastimport.c) update via atomic stores or under
 * g_state.lock for the multi-word fields; readers (zcl_state dump in
 * the core file) just copy the values out. */
struct local_chain_ingest_state {
    pthread_mutex_t lock;
    _Atomic int  phase;            /* 0=idle, 1=sha3, 2=chainstate, 3=blocks, 4=done */
    _Atomic int  result;           /* enum local_ingest_result; LCI_OK only after phase 4 */
    _Atomic int64_t blocks_done;
    _Atomic int64_t blocks_total;
    _Atomic int64_t utxos_imported;
    _Atomic int64_t windows_verified;
    _Atomic bool evidence_prefix_verified;  /* T3.3: full prefix verified this boot */
    _Atomic int64_t started_at;    /* unix seconds; 0 → never run */
    _Atomic int64_t finished_at;   /* unix seconds; 0 → in progress */
    int           health_id;
    char          legacy_datadir[512];
    char          last_error[256];
};

extern struct local_chain_ingest_state g_local_ingest_state;

/* Shared helpers implemented in local_chain_ingest.c, called by the
 * fastimport phases to update the shared state safely. */
void lci_state_set_error(const char *msg);
void lci_state_set_datadir(const char *path);
void lci_ensure_health_registered(void);

#endif /* ZCL_SERVICES_LOCAL_CHAIN_INGEST_INTERNAL_H */
