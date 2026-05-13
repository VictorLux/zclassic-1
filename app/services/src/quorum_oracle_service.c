/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Quorum oracle — see services/quorum_oracle_service.h for the contract.
 *
 * Today two sources are wired:
 *   - QO_SRC_LOCAL:     active_chain_at(height) → blockhash
 *   - QO_SRC_ZCLASSICD: zclassicd_oracle_probe(height)
 *
 * The verdict logic accepts arbitrary N — a future P2P source plugs in
 * without changing callers. */

#include "services/quorum_oracle_service.h"
#include "services/oracle_policy.h"
#include "services/zclassicd_oracle_service.h"

#include "chain/chain.h"
#include "core/uint256.h"
#include "controllers/wallet_helpers.h"
#include "json/json.h"
#include "util/log_macros.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define QO_DEFAULT_MIN_AGREE 2

static struct {
    pthread_mutex_t lock;
    bool   initialized;
    int    min_agree;

    _Atomic int64_t total_probes;
    _Atomic int64_t total_matches;
    _Atomic int64_t total_splits;
    _Atomic int64_t total_no_data;
    _Atomic int     last_height;
    _Atomic int64_t last_probe_unix;
} g_qo = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

void quorum_oracle_init(const struct quorum_oracle_config *cfg)
{
    pthread_mutex_lock(&g_qo.lock);
    if (g_qo.initialized) {
        pthread_mutex_unlock(&g_qo.lock);
        return;
    }
    g_qo.min_agree = (cfg && cfg->min_agree > 0)
                         ? cfg->min_agree : QO_DEFAULT_MIN_AGREE;
    g_qo.initialized = true;
    pthread_mutex_unlock(&g_qo.lock);
}

static void qo_probe_local(int height,
                            struct quorum_oracle_source_result *out)
{
    out->present = false;
    out->error = false;
    out->hash_hex[0] = '\0';
    struct main_state *ms = wallet_rpc_main_state();
    if (!ms) return;
    struct block_index *bi = active_chain_at(&ms->chain_active, height);
    if (!bi || !bi->phashBlock) return;
    uint256_get_hex(bi->phashBlock, out->hash_hex);
    out->present = true;
}

static void qo_probe_zclassicd(int height,
                                struct quorum_oracle_source_result *out)
{
    out->present = false;
    out->error = false;
    out->hash_hex[0] = '\0';
    struct zclassicd_oracle_probe_result r;
    if (!zclassicd_oracle_probe(height, &r)) return;
    if (r.error) {
        out->present = true;
        out->error = true;
        return;
    }
    if (r.their_hash[0] == '\0') return;
    snprintf(out->hash_hex, sizeof(out->hash_hex), "%s", r.their_hash);
    out->present = true;
}

bool quorum_oracle_probe(int height, struct quorum_oracle_result *out)
{
    if (!out || height < 0) return false;
    memset(out, 0, sizeof(*out));
    out->height = height;

    if (!g_qo.initialized) quorum_oracle_init(NULL);

    qo_probe_local    (height, &out->by_source[QO_SRC_LOCAL]);
    qo_probe_zclassicd(height, &out->by_source[QO_SRC_ZCLASSICD]);
    /* QO_SRC_PEER reserved — leaves slot empty. */

    /* Count occurrences of each non-error hash. */
    int counts[QO_SRC_NUM] = {0};
    int total_with_hash = 0;
    for (int i = 0; i < QO_SRC_NUM; i++) {
        if (!out->by_source[i].present) continue;
        if (out->by_source[i].error) continue;
        if (out->by_source[i].hash_hex[0] == '\0') continue;
        total_with_hash++;
        for (int j = 0; j <= i; j++) {
            if (!out->by_source[j].present || out->by_source[j].error)
                continue;
            if (out->by_source[j].hash_hex[0] == '\0') continue;
            if (strcasecmp(out->by_source[i].hash_hex,
                           out->by_source[j].hash_hex) == 0) {
                counts[j]++;
            }
        }
    }

    int min_agree = g_qo.min_agree;
    int best_idx = -1, best_count = 0;
    for (int i = 0; i < QO_SRC_NUM; i++) {
        if (counts[i] > best_count) {
            best_count = counts[i];
            best_idx = i;
        }
    }

    atomic_fetch_add(&g_qo.total_probes, 1);
    atomic_store(&g_qo.last_height, height);
    atomic_store(&g_qo.last_probe_unix, (int64_t)time(NULL));

    if (total_with_hash == 0) {
        out->verdict = QO_VERDICT_NO_DATA;
        out->agreeing_sources = 0;
        atomic_fetch_add(&g_qo.total_no_data, 1);
        return true;
    }
    if (best_count >= min_agree) {
        out->verdict = QO_VERDICT_QUORUM_MATCH;
        out->agreeing_sources = best_count;
        snprintf(out->winning_hash_hex, sizeof(out->winning_hash_hex),
                 "%s", out->by_source[best_idx].hash_hex);
        atomic_fetch_add(&g_qo.total_matches, 1);
        return true;
    }

    /* Split. Find any two non-matching sources and feed their pair to
     * oracle_policy so the HALT/PANIC ladder engages. */
    out->verdict = QO_VERDICT_QUORUM_SPLIT;
    out->agreeing_sources = best_count;
    atomic_fetch_add(&g_qo.total_splits, 1);

    const char *a = NULL, *b = NULL;
    for (int i = 0; i < QO_SRC_NUM; i++) {
        if (!out->by_source[i].present || out->by_source[i].error)
            continue;
        if (out->by_source[i].hash_hex[0] == '\0') continue;
        if (!a) {
            a = out->by_source[i].hash_hex;
        } else if (strcasecmp(a, out->by_source[i].hash_hex) != 0) {
            b = out->by_source[i].hash_hex;
            break;
        }
    }
    if (a && b) {
        oracle_policy_record_disagreement(height, a, b);
        fprintf(stderr,
                "[quorum_oracle] split at h=%d: %s vs %s\n",
                height, a, b);
    }
    return true;
}

bool quorum_oracle_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out) return false;
    json_set_object(out);
    json_push_kv_int(out, "min_agree", g_qo.min_agree);
    json_push_kv_int(out, "total_probes",
                     atomic_load(&g_qo.total_probes));
    json_push_kv_int(out, "total_matches",
                     atomic_load(&g_qo.total_matches));
    json_push_kv_int(out, "total_splits",
                     atomic_load(&g_qo.total_splits));
    json_push_kv_int(out, "total_no_data",
                     atomic_load(&g_qo.total_no_data));
    json_push_kv_int(out, "last_height",
                     atomic_load(&g_qo.last_height));
    json_push_kv_int(out, "last_probe_unix",
                     atomic_load(&g_qo.last_probe_unix));
    /* Source roster (advertises future expansion). */
    json_push_kv_str(out, "source_local",     "wired");
    json_push_kv_str(out, "source_zclassicd", "wired");
    json_push_kv_str(out, "source_peer",      "reserved");
    return true;
}

void quorum_oracle_reset_for_test(void)
{
    pthread_mutex_lock(&g_qo.lock);
    g_qo.initialized = false;
    g_qo.min_agree = 0;
    pthread_mutex_unlock(&g_qo.lock);
    atomic_store(&g_qo.total_probes, 0);
    atomic_store(&g_qo.total_matches, 0);
    atomic_store(&g_qo.total_splits, 0);
    atomic_store(&g_qo.total_no_data, 0);
    atomic_store(&g_qo.last_height, 0);
    atomic_store(&g_qo.last_probe_unix, 0);
}
