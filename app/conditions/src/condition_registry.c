/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "conditions/condition_registry.h"

void register_block_failed_mask_at_tip(void);
void register_contradiction_frozen(void);
void register_chain_stalled_with_data(void);

void condition_registry_register_all(void)
{
    register_block_failed_mask_at_tip();
    register_contradiction_frozen();
    register_chain_stalled_with_data();
}
