/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_SUPERVISORS_LEGACY_MIRROR_SUPERVISOR_H
#define ZCL_SUPERVISORS_LEGACY_MIRROR_SUPERVISOR_H

#include <stdbool.h>

bool legacy_mirror_supervisor_start(int cadence_secs);
void legacy_mirror_supervisor_stop(void);
bool legacy_mirror_supervisor_running(void);

#endif /* ZCL_SUPERVISORS_LEGACY_MIRROR_SUPERVISOR_H */
