/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Chain domain supervisor children — declarative liveness registration.
 *
 * Owns the chain.coord_escalation child: a Round-5 C4 contract that, after
 * 900 s of fatal mirror-lag breach + frozen local height, tries
 * evidence-based revalidation of the next-child block (Wave M) and falls
 * back to force_mirror_promotion. Registered in the `chain` domain
 * (g_chain_sup). */

#ifndef ZCL_CHAIN_SUPERVISOR_H
#define ZCL_CHAIN_SUPERVISOR_H

struct main_state;

/* Register the chain-domain supervisor children. Idempotent — a second
 * call is a no-op. `ms` is the live chainstate the escalation child reads. */
void chain_supervisor_register(struct main_state *ms);

#endif /* ZCL_CHAIN_SUPERVISOR_H */
