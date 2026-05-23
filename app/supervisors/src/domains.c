/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "supervisors/domains.h"

supervisor_domain_t *g_chain_sup;
supervisor_domain_t *g_net_sup;
supervisor_domain_t *g_mempool_sup;
supervisor_domain_t *g_wallet_sup;
supervisor_domain_t *g_feature_sup;
supervisor_domain_t *g_onion_sup;
supervisor_domain_t *g_op_sup;

void supervisor_domains_init(void)
{
    if (!g_chain_sup)   g_chain_sup   = supervisor_create_domain("chain");
    if (!g_net_sup)     g_net_sup     = supervisor_create_domain("net");
    if (!g_mempool_sup) g_mempool_sup = supervisor_create_domain("mempool");
    if (!g_wallet_sup)  g_wallet_sup  = supervisor_create_domain("wallet");
    if (!g_feature_sup) g_feature_sup = supervisor_create_domain("feature");
    if (!g_onion_sup)   g_onion_sup   = supervisor_create_domain("onion");
    if (!g_op_sup)      g_op_sup      = supervisor_create_domain("op");
}
