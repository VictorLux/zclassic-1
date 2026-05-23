/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_CONDITIONS_UTXO_ACTIVATION_PAUSED_H
#define ZCL_CONDITIONS_UTXO_ACTIVATION_PAUSED_H

#include <stdbool.h>

void register_utxo_activation_paused(void);

#ifdef ZCL_TESTING
void utxo_activation_paused_test_reset(void);
void utxo_activation_paused_test_set_reason(const char *reason);
void utxo_activation_paused_test_set_remedy_clear_enabled(bool enabled);
int utxo_activation_paused_test_resume_calls(void);
int utxo_activation_paused_test_repair_calls(void);
#endif

#endif /* ZCL_CONDITIONS_UTXO_ACTIVATION_PAUSED_H */
