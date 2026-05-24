/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * header_probe_poll — Job (Wave S / Phase 3 dissolve PR-1).
 *
 * Periodic supervisor child that drives the header_probe service's
 * polling cadence. Replaces the heartbeat-ring-driven `hp_on_tick`
 * with a typed liveness contract registered in the network
 * supervisor domain.
 *
 * Why this shape:
 *   - The heartbeat ring is a shared sweeper thread (single point of
 *     failure; Round 5 supervisor split was motivated by an 8.6 h
 *     wedge of that thread). Moving each periodic to its own
 *     supervisor child gives operators (and `zcl_state
 *     subsystem=supervisor`) visibility into stall age + ticks_run.
 *   - The Job owns scheduling ONLY. Peer selection, RPC, batched
 *     validation, accept_block_header — all stay in
 *     app/services/src/header_probe_service.c (PR-2 will dissolve).
 *
 * Boot wiring: call `header_probe_poll_register()` from
 * `config/src/boot_services.c` after `header_probe_init()`. The
 * supervisor must already be started (Round 5 supervisor tree).
 *
 * See `docs/dissolve/header_probe_service.md` § PR-1. */

#ifndef ZCL_JOB_HEADER_PROBE_POLL_H
#define ZCL_JOB_HEADER_PROBE_POLL_H

#include <stdbool.h>

/* Register the Job with the network supervisor domain. Idempotent —
 * second + subsequent calls are no-ops. The contract uses a 30 s
 * tick cadence (matching the legacy heartbeat default). */
void header_probe_poll_register(void);

/* True once `header_probe_poll_register` has produced a valid
 * supervisor child id. Used by tests + diagnostics. */
bool header_probe_poll_is_registered(void);

#endif /* ZCL_JOB_HEADER_PROBE_POLL_H */
