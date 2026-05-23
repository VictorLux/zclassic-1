/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_SUPERVISOR_DOMAINS_H
#define ZCL_SUPERVISOR_DOMAINS_H

#include "util/supervisor.h"

extern supervisor_domain_t *g_chain_sup;
extern supervisor_domain_t *g_net_sup;
extern supervisor_domain_t *g_mempool_sup;
extern supervisor_domain_t *g_wallet_sup;
extern supervisor_domain_t *g_feature_sup;
extern supervisor_domain_t *g_onion_sup;
extern supervisor_domain_t *g_op_sup;

void supervisor_domains_init(void);

#endif /* ZCL_SUPERVISOR_DOMAINS_H */
