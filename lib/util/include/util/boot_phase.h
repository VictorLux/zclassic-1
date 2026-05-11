/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * boot_phase — per-step stall watchdog for the boot sequence.
 *
 * A scoped wrapper around a boot step that:
 *   * logs `[boot-phase] BEGIN <name>` at start
 *   * starts a background pthread that, every 30s while the phase is
 *     active, logs `[boot-phase] STALL <name> <elapsed_ms>ms` so a
 *     silent CPU loop becomes visible in node.log instead of leaving
 *     the operator guessing
 *   * logs `[boot-phase] END <name> <elapsed_ms>ms` on completion
 *
 * Usage:
 *   struct boot_phase p;
 *   boot_phase_begin(&p, "block_index_load");
 *   ... slow work ...
 *   boot_phase_end(&p);
 *
 * Designed to be cheap (one pthread per phase, sleeps 1s and checks
 * an atomic flag) and crash-safe (end joins; begin failing falls
 * back to no-op so the boot still proceeds). Lifecycle is fully
 * contained in the begin/end pair on a single thread — no shared
 * state, no init/shutdown calls required at process start. */

#ifndef ZCL_BOOT_PHASE_H
#define ZCL_BOOT_PHASE_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#define BOOT_PHASE_NAME_MAX 64

struct boot_phase {
    char     name[BOOT_PHASE_NAME_MAX];
    int64_t  start_ms;
    pthread_t watchdog;
    _Atomic bool active;
    bool     watchdog_started;
};

void boot_phase_begin(struct boot_phase *p, const char *name);
void boot_phase_end(struct boot_phase *p);

#endif
