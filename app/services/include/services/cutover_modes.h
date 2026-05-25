/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_SERVICES_CUTOVER_MODES_H
#define ZCL_SERVICES_CUTOVER_MODES_H

typedef enum {
    CUTOVER_STAGE_MODE_SHADOW = 0,
    CUTOVER_STAGE_MODE_AUTHORITATIVE
} cutover_stage_mode_t;

void cutover_modes_set_header_admit(cutover_stage_mode_t mode);
void cutover_modes_set_validate_headers(cutover_stage_mode_t mode);
void cutover_modes_set_header_pipeline(cutover_stage_mode_t header_admit,
                                       cutover_stage_mode_t validate_headers);

cutover_stage_mode_t cutover_modes_get_header_admit(void);
cutover_stage_mode_t cutover_modes_get_validate_headers(void);

#endif /* ZCL_SERVICES_CUTOVER_MODES_H */
