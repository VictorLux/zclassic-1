/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_CONDITIONS_ORPHAN_UTXO_ABOVE_TIP_H
#define ZCL_CONDITIONS_ORPHAN_UTXO_ABOVE_TIP_H

void register_orphan_utxo_above_tip(void);

#ifdef ZCL_TESTING
struct node_db;
void orphan_utxo_above_tip_test_reset(void);
void orphan_utxo_above_tip_test_set_node_db(struct node_db *ndb);
int orphan_utxo_above_tip_test_remedy_calls(void);
#endif

#endif /* ZCL_CONDITIONS_ORPHAN_UTXO_ABOVE_TIP_H */
