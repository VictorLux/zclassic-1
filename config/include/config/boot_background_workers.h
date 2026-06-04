/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Boot background-workers unit — the long-lived helper threads that
 * app_init_services spawns at scattered points during runtime startup and
 * that app_shutdown_svc joins on teardown.
 *
 * Workers in this unit:
 *   - payment_processor_thread        (store payment scanner + tip watchdog)
 *   - background_utxo_replay          (post-snapshot chain validation)
 *   - build_snapshot_offer_thread     (fast-sync offer + manifests)
 *   - address_backfill_service_thread (advisory address aggregation)
 *   - hodl_history_worker_thread      (explorer HODL time-series filler)
 *   - projection_backfill_service_thread (reducer projection catch-up)
 *
 * Each worker has a boot_start_ / boot_join_ pair declared below; the start
 * calls live at their existing scattered points in app_init_services (boot
 * order preserved) and the joins in app_shutdown_svc. The generic thread
 * helpers (start/bounded-join) move with these workers; boot_join_thread_bounded
 * is exposed because the catchup-job helpers that stay in boot_services.c reuse
 * it.
 *
 * Not for use outside config/src/.
 */

#ifndef ZCL_BOOT_BACKGROUND_WORKERS_H
#define ZCL_BOOT_BACKGROUND_WORKERS_H

#include <pthread.h>
#include <stdbool.h>
#include <time.h>

struct boot_svc_ctx;

/* Generic bounded thread join used by both this unit and the catchup-job
 * helpers that remain in boot_services.c. Joins with a timeout; on timeout or
 * error it logs and detaches so shutdown never blocks indefinitely. */
bool boot_join_thread_bounded(pthread_t thread, const char *name,
                              int timeout_sec);

/* Store payment processor (store profile). Start is gated by
 * boot_store_payment_start in boot_services.c. */
bool boot_start_payment_service(struct boot_svc_ctx *svc);
void boot_join_payment_service(struct boot_svc_ctx *svc);

/* Background UTXO replay after a snapshot import (delta replay). */
bool boot_start_replay_service(struct boot_svc_ctx *svc);
void boot_join_replay_service(struct boot_svc_ctx *svc);

/* Fast-sync snapshot offer + chunk/block manifest builder. */
bool boot_start_offer_service(struct boot_svc_ctx *svc);
void boot_join_offer_service(struct boot_svc_ctx *svc);

/* Advisory per-address aggregation backfill. */
bool boot_start_address_backfill_service(struct boot_svc_ctx *svc);
void boot_join_address_backfill_service(struct boot_svc_ctx *svc);

/* Explorer HODL-wave history filler. */
bool boot_start_hodl_history_service(struct boot_svc_ctx *svc);
void boot_join_hodl_history_service(struct boot_svc_ctx *svc);

/* Reducer projection backfill watcher (drives the catchup job forward). */
bool boot_start_projection_backfill_service(struct boot_svc_ctx *svc);
void boot_join_projection_backfill_service(struct boot_svc_ctx *svc);

/* Snapshot transaction-index build job. The job init lives in
 * app_init_services; start spawns it, join reaps it on shutdown. */
bool boot_start_tx_index_service(struct boot_svc_ctx *svc);
void boot_join_tx_index_service(struct boot_svc_ctx *svc);

#endif
