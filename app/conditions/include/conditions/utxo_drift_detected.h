/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_CONDITIONS_UTXO_DRIFT_DETECTED_H
#define ZCL_CONDITIONS_UTXO_DRIFT_DETECTED_H

void register_utxo_drift_detected(void);

#ifdef ZCL_TESTING
struct node_db;
void utxo_drift_detected_test_reset(void);
void utxo_drift_detected_test_set_node_db(struct node_db *ndb);
int utxo_drift_detected_test_remedy_calls(void);
#endif

#endif /* ZCL_CONDITIONS_UTXO_DRIFT_DETECTED_H */
