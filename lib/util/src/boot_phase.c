/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "util/boot_phase.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void *boot_phase_watchdog(void *arg)
{
    struct boot_phase *p = (struct boot_phase *)arg;
    int slept = 0;
    while (atomic_load(&p->active)) {
        struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
        nanosleep(&ts, NULL);
        slept++;
        if (slept >= 30) {
            int64_t elapsed = mono_ms() - p->start_ms;
            fprintf(stderr,
                "[boot-phase] STALL %s %lldms (no progress reported)\n",
                p->name, (long long)elapsed);
            fflush(stderr);
            slept = 0;
        }
    }
    return NULL;
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
    atomic_store(&p->active, true);

    fprintf(stderr, "[boot-phase] BEGIN %s\n", p->name);
    fflush(stderr);

    if (pthread_create(&p->watchdog, NULL, boot_phase_watchdog, p) == 0)
        p->watchdog_started = true;
}

void boot_phase_end(struct boot_phase *p)
{
    if (!p) return;
    int64_t elapsed = mono_ms() - p->start_ms;
    atomic_store(&p->active, false);
    if (p->watchdog_started) {
        pthread_join(p->watchdog, NULL);
        p->watchdog_started = false;
    }
    fprintf(stderr, "[boot-phase] END %s %lldms\n",
            p->name, (long long)elapsed);
    fflush(stderr);
}
