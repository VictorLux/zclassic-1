/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "util/boot_phase.h"
#include "health/heartbeat.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define BOOT_PHASE_STALL_SECS 30

static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void boot_phase_on_stall(void *ctx)
{
    struct boot_phase *p = (struct boot_phase *)ctx;
    if (!p) return;
    int64_t elapsed = mono_ms() - p->start_ms;
    fprintf(stderr,
        "[boot-phase] STALL %s %lldms (no progress reported)\n",
        p->name, (long long)elapsed);
    fflush(stderr);
}

void boot_phase_begin(struct boot_phase *p, const char *name)
{
    if (!p) return;
    memset(p, 0, sizeof(*p));
    if (name) {
        size_t n = strlen(name);
        if (n >= BOOT_PHASE_NAME_MAX) n = BOOT_PHASE_NAME_MAX - 1;
        memcpy(p->name, name, n);
        p->name[n] = '\0';
    } else {
        snprintf(p->name, sizeof(p->name), "(unnamed)");
    }
    p->start_ms = mono_ms();
    p->health_id = HEALTH_INVALID_ID;

    fprintf(stderr, "[boot-phase] BEGIN %s\n", p->name);
    fflush(stderr);

    /* Lazy-start the heartbeat sweeper. health_start() is idempotent
     * so multiple boot phases (or other subsystems) calling it is
     * fine — only the first one spawns the thread. Production boot
     * paths don't need a separate health_start() call. */
    (void)health_start();

    /* Register so that the heartbeat sweeper fires our stall callback
     * if the phase hasn't ended within BOOT_PHASE_STALL_SECS. We do
     * not heartbeat — the entry is unregistered on phase end. */
    p->health_id = health_register(p->name, BOOT_PHASE_STALL_SECS,
                                    boot_phase_on_stall, p);
}

void boot_phase_end(struct boot_phase *p)
{
    if (!p) return;
    int64_t elapsed = mono_ms() - p->start_ms;
    if (p->health_id != HEALTH_INVALID_ID) {
        health_unregister(p->health_id);
        p->health_id = HEALTH_INVALID_ID;
    }
    fprintf(stderr, "[boot-phase] END %s %lldms\n",
            p->name, (long long)elapsed);
    fflush(stderr);
}
