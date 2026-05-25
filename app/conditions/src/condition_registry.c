/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "conditions/condition_registry.h"

void register_block_failed_mask_at_tip(void);
void register_contradiction_frozen(void);
void register_chain_stalled_with_data(void);
void register_utxo_activation_paused(void);
void register_header_stall_at_height(void);
void register_sync_state_stuck(void);
void register_download_queue_starved(void);
void register_local_header_refill_needed(void);
void register_peer_floor_violated(void);
void register_sync_violation_lag(void);
void register_tip_wedged_resnapshot(void);
void register_snapshot_receive_stalled(void);

void condition_registry_register_all(void)
{
    register_block_failed_mask_at_tip();
    register_contradiction_frozen();
    register_chain_stalled_with_data();
    register_utxo_activation_paused();
    register_header_stall_at_height();
    register_sync_state_stuck();
    register_download_queue_starved();
    register_local_header_refill_needed();
    register_peer_floor_violated();
    register_sync_violation_lag();
    register_tip_wedged_resnapshot();
    register_snapshot_receive_stalled();
}
