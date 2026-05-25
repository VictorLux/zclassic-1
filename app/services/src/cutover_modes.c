/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "services/cutover_modes.h"

#include <stdatomic.h>

#define CUTOVER_MODE_HEADER_ADMIT      0x01u
#define CUTOVER_MODE_VALIDATE_HEADERS  0x02u

static _Atomic unsigned g_header_pipeline_modes = 0;

static unsigned bit_for_mode(unsigned bit, cutover_stage_mode_t mode)
{
    return mode == CUTOVER_STAGE_MODE_AUTHORITATIVE ? bit : 0u;
}

static void cutover_modes_update_one(unsigned bit, cutover_stage_mode_t mode)
{
    unsigned old_bits = atomic_load(&g_header_pipeline_modes);
    unsigned new_bits;
    do {
        new_bits = old_bits & ~bit;
        new_bits |= bit_for_mode(bit, mode);
    } while (!atomic_compare_exchange_weak(&g_header_pipeline_modes,
                                           &old_bits,
                                           new_bits));
}

void cutover_modes_set_header_admit(cutover_stage_mode_t mode)
{
    cutover_modes_update_one(CUTOVER_MODE_HEADER_ADMIT, mode);
}

void cutover_modes_set_validate_headers(cutover_stage_mode_t mode)
{
    cutover_modes_update_one(CUTOVER_MODE_VALIDATE_HEADERS, mode);
}

void cutover_modes_set_header_pipeline(cutover_stage_mode_t header_admit,
                                       cutover_stage_mode_t validate_headers)
{
    unsigned bits = bit_for_mode(CUTOVER_MODE_HEADER_ADMIT, header_admit) |
                    bit_for_mode(CUTOVER_MODE_VALIDATE_HEADERS,
                                 validate_headers);
    atomic_store(&g_header_pipeline_modes, bits);
}

cutover_stage_mode_t cutover_modes_get_header_admit(void)
{
    return (atomic_load(&g_header_pipeline_modes) &
            CUTOVER_MODE_HEADER_ADMIT)
        ? CUTOVER_STAGE_MODE_AUTHORITATIVE
        : CUTOVER_STAGE_MODE_SHADOW;
}

cutover_stage_mode_t cutover_modes_get_validate_headers(void)
{
    return (atomic_load(&g_header_pipeline_modes) &
            CUTOVER_MODE_VALIDATE_HEADERS)
        ? CUTOVER_STAGE_MODE_AUTHORITATIVE
        : CUTOVER_STAGE_MODE_SHADOW;
}
