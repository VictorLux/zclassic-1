/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Quorum oracle (T2.2) — multi-source consensus on (height → hash).
 *
 * Generalises the zclassicd-only `zclassicd_oracle_service` into a
 * pluggable N-source vote. Today wires two sources:
 *
 *   QO_SRC_LOCAL       — our own active_chain at the probed height
 *   QO_SRC_ZCLASSICD   — RPC getblockhash against the local zclassicd
 *
 * A future P2P-peer source (QO_SRC_PEER) will hash a peer's header
 * chain at the probed height. The probe API is shaped so that source
 * is purely additive — no caller changes required.
 *
 * Verdict logic:
 *   - If at least `min_agree` non-error sources return the same hash,
 *     the quorum matches.
 *   - If two sources return DIFFERENT non-error hashes, the quorum
 *     disagrees and oracle_policy is fed the disagreement.
 *
 * Callers that need stronger trust than "zclassicd only" (e.g.
 * rolling-anchor window commit) should consult this service. */

#ifndef ZCL_SERVICES_QUORUM_ORACLE_SERVICE_H
#define ZCL_SERVICES_QUORUM_ORACLE_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

struct json_value;

enum quorum_oracle_source {
    QO_SRC_LOCAL     = 0,
    QO_SRC_ZCLASSICD = 1,
    QO_SRC_PEER      = 2,        /* reserved — not yet implemented */
    QO_SRC_NUM       = 3,
};

struct quorum_oracle_source_result {
    bool present;                /* did this source contribute a hash? */
    bool error;                  /* present && error → RPC/IO failure */
    char hash_hex[65];           /* lowercased, NUL-terminated */
};

enum quorum_oracle_verdict {
    QO_VERDICT_NO_DATA       = 0, /* not enough sources contributed */
    QO_VERDICT_QUORUM_MATCH  = 1, /* min_agree sources concur */
    QO_VERDICT_QUORUM_SPLIT  = 2, /* sources disagree */
};

struct quorum_oracle_result {
    int height;
    struct quorum_oracle_source_result by_source[QO_SRC_NUM];
    enum quorum_oracle_verdict verdict;
    int agreeing_sources;        /* how many produced the winning hash */
    char winning_hash_hex[65];   /* set on QO_VERDICT_QUORUM_MATCH */
};

struct quorum_oracle_config {
    int min_agree;               /* default 2 */
};

void quorum_oracle_init(const struct quorum_oracle_config *cfg);

/* Synchronous probe. Returns true if any source contributed data;
 * verdict + per-source fields populated in *out. On QO_VERDICT_QUORUM_SPLIT
 * the disagreement is forwarded to oracle_policy. */
bool quorum_oracle_probe(int height, struct quorum_oracle_result *out);

bool quorum_oracle_dump_state_json(struct json_value *out, const char *key);

void quorum_oracle_reset_for_test(void);

#endif /* ZCL_SERVICES_QUORUM_ORACLE_SERVICE_H */
