/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * boot_phase — per-step stall logger for the boot sequence.
 *
 * A scoped wrapper around a boot step that:
 *   * logs `[boot-phase] BEGIN <name>` at start
 *   * registers a 30s stall entry in lib/health so if the phase
 *     hangs, `[boot-phase] STALL <name> <elapsed_ms>ms` appears in
 *     node.log on the heartbeat sweeper's tick (~1s after the
 *     deadline)
 *   * logs `[boot-phase] END <name> <elapsed_ms>ms` on completion,
 *     unregistering the health entry
 *
 * Usage:
 *   struct boot_phase p;
 *   boot_phase_begin(&p, "block_index_load");
 *   ... slow work ...
 *   boot_phase_end(&p);
 *
 * Before this module used the unified watchdog, every boot phase
 * spawned its own pthread that polled an atomic flag. With ~10 boot
 * phases that was 10 transient threads. After: zero. The heartbeat
 * sweeper (lib/health) does the work for all of them, and operator
 * visibility (STALL fires once per phase that exceeds 30s) is
 * preserved. */

#ifndef ZCL_BOOT_PHASE_H
#define ZCL_BOOT_PHASE_H

#include "health/heartbeat.h"

#include <stdint.h>

#define BOOT_PHASE_NAME_MAX 64

struct boot_phase {
    char                name[BOOT_PHASE_NAME_MAX];
    int64_t             start_ms;
    health_subsystem_id health_id;
};

void boot_phase_begin(struct boot_phase *p, const char *name);
void boot_phase_end(struct boot_phase *p);

#endif
