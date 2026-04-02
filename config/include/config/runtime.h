/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_RUNTIME_H
#define ZCL_RUNTIME_H

struct node_db;
struct tx_mempool;
struct wallet;

struct app_runtime_context {
    struct node_db *node_db;
    struct tx_mempool *mempool;
    struct wallet *wallet;
};

/* Runtime registry lifecycle:
 * - boot/config code sets the current runtime during service startup
 * - long-lived consumers may read it while the node is running
 * - shutdown clears it before owned resources are freed
 */
void app_runtime_set_current(struct app_runtime_context *runtime);
const struct app_runtime_context *app_runtime_current(void);

struct node_db *app_runtime_node_db(void);
struct tx_mempool *app_runtime_mempool(void);
struct wallet *app_runtime_wallet(void);

#endif
