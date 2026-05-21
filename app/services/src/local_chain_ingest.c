/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * local_chain_ingest — see services/local_chain_ingest.h for the
 * three-phase pipeline contract (SHA3 windows, chainstate import,
 * per-block advance) and the security model.
 *
 * This translation unit holds the SHARED state and reentrant-safe
 * helpers used by both the public dump/accessor API and the
 * fastimport pipeline. The three heavy-lift phases (SHA3 window
 * verify, LevelDB chainstate import, per-block legacy body-pull) live
 * in local_chain_ingest_fastimport.c — quarantined there because the
 * -fastimport entry point is being demoted to advanced / per-block
 * diagnostic in the cold-start consolidation effort.
 *
 * Reentrant-safe: a single ingest run at a time per process. The
 * dump-state path uses atomics so concurrent zcl_state callers see
 * consistent snapshots without taking the runner's locks.
 */

#include "services/local_chain_ingest.h"

#include "chain/sha3_windows.h"
#include "health/heartbeat.h"
#include "json/json.h"

#include "local_chain_ingest_internal.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── Runtime state for state-dump introspection ─────────────────── */

/* Single shared snapshot.  Writers (the running ingest thread)
 * update via atomic stores or under g_state.lock for the multi-word
 * fields; readers (zcl_state) just copy the values out.  This avoids
 * the dump path ever blocking on the heavy ingest work. */
struct local_chain_ingest_state g_local_ingest_state = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .phase = 0,
    .result = LCI_OK,
    .blocks_done = 0,
    .blocks_total = 0,
    .utxos_imported = 0,
    .windows_verified = 0,
    .evidence_prefix_verified = false,
    .started_at = 0,
    .finished_at = 0,
    .health_id = HEALTH_INVALID_ID,
    .legacy_datadir = {0},
    .last_error = {0},
};

void lci_state_set_error(const char *msg)
{
    pthread_mutex_lock(&g_local_ingest_state.lock);
    snprintf(g_local_ingest_state.last_error,
             sizeof(g_local_ingest_state.last_error), "%s",
             msg ? msg : "");
    pthread_mutex_unlock(&g_local_ingest_state.lock);
}

void lci_state_set_datadir(const char *path)
{
    pthread_mutex_lock(&g_local_ingest_state.lock);
    snprintf(g_local_ingest_state.legacy_datadir,
             sizeof(g_local_ingest_state.legacy_datadir), "%s",
             path ? path : "");
    pthread_mutex_unlock(&g_local_ingest_state.lock);
}

/* health_register_periodic callback — fires from the sweeper thread
 * every PROGRESS_TICK_SECS regardless of ingest activity.  Prints a
 * single-line progress summary so operators can `journalctl -f` and
 * see liveness without enabling chatty per-block logs. */
#define LOCAL_INGEST_TICK_SECS  10

static void local_chain_ingest_tick(void *ctx)
{
    (void)ctx;
    int phase = atomic_load(&g_local_ingest_state.phase);
    if (phase == 0 || phase == 4) return;
    int64_t bdone  = atomic_load(&g_local_ingest_state.blocks_done);
    int64_t btotal = atomic_load(&g_local_ingest_state.blocks_total);
    int64_t utxos  = atomic_load(&g_local_ingest_state.utxos_imported);
    int64_t wins   = atomic_load(&g_local_ingest_state.windows_verified);
    fprintf(stderr, // obs-ok:pre-existing-diagnostic
            "[local_ingest] phase=%d blocks=%" PRId64 "/%" PRId64
            " utxos=%" PRId64 " sha3_windows_verified=%" PRId64 "\n",
            phase, bdone, btotal, utxos, wins);
}

void lci_ensure_health_registered(void)
{
    if (g_local_ingest_state.health_id == HEALTH_INVALID_ID) {
        g_local_ingest_state.health_id = health_register_periodic(
            "local_ingest", LOCAL_INGEST_TICK_SECS,
            local_chain_ingest_tick, NULL);
        /* HEALTH_INVALID_ID on registry-full or before health_start();
         * the ingest still runs, just without the periodic log line. */
    }
}

/* ── Result name table ───────────────────────────────────────────── */

const char *local_ingest_result_name(enum local_ingest_result r)
{
    static const char *names[] = {
        [LCI_OK]                   = "ok",
        [LCI_SOURCE_MISSING]       = "source_missing",
        [LCI_SHA3_WINDOW_MISMATCH] = "sha3_window_mismatch",
        [LCI_CHAINSTATE_MISMATCH]  = "chainstate_mismatch",
        [LCI_ABORTED]              = "aborted",
        [LCI_INTERNAL_ERROR]       = "internal_error",
    };
    if (r >= 0 && r < LCI_NUM_RESULTS) return names[r];
    return "unknown";
}

/* ── Detector ────────────────────────────────────────────────────── */

bool local_chain_ingest_detect_legacy_datadir(const char *path)
{
    if (!path || !path[0]) return false;
    char buf[1024];
    int n = snprintf(buf, sizeof(buf), "%s/blocks/blk00000.dat", path);
    if (n <= 0 || (size_t)n >= sizeof(buf)) return false;
    struct stat st;
    if (stat(buf, &st) != 0) return false;
    return S_ISREG(st.st_mode);
}

/* ── T3.3 evidence-prefix accessors ──────────────────────────────── */

/* Exposed via services/local_chain_ingest.h so the bg-validation
 * service can skip historical heights covered by the verified static
 * SHA3 prefix. */
bool local_chain_ingest_evidence_prefix_verified(void)
{
    return atomic_load(&g_local_ingest_state.evidence_prefix_verified);
}

int local_chain_ingest_evidence_prefix_end_height(void)
{
    if (g_sha3_windows_count == 0) return -1; // raw-return-ok:sentinel-no-compile-time-windows
    return (int)(g_sha3_windows_count * SHA3_WINDOW_SIZE) - 1;
}

/* ── State dump for zcl_state subsystem=local_ingest ────────────── */

bool local_chain_ingest_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out) return false;
    /* Caller is expected to have json_set_object'd `out` first (per the
     * *_dump_state_json convention in CLAUDE.md).  We tolerate either
     * initialised-or-not by setting it explicitly here too — json_set_object
     * is idempotent. */
    json_set_object(out);

    int phase = atomic_load(&g_local_ingest_state.phase);
    int result = atomic_load(&g_local_ingest_state.result);
    int64_t started  = atomic_load(&g_local_ingest_state.started_at);
    int64_t finished = atomic_load(&g_local_ingest_state.finished_at);
    int64_t bdone    = atomic_load(&g_local_ingest_state.blocks_done);
    int64_t btotal   = atomic_load(&g_local_ingest_state.blocks_total);
    int64_t utxos    = atomic_load(&g_local_ingest_state.utxos_imported);
    int64_t wins     = atomic_load(&g_local_ingest_state.windows_verified);

    pthread_mutex_lock(&g_local_ingest_state.lock);
    char datadir_copy[sizeof(g_local_ingest_state.legacy_datadir)];
    char err_copy[sizeof(g_local_ingest_state.last_error)];
    memcpy(datadir_copy, g_local_ingest_state.legacy_datadir,
           sizeof(datadir_copy));
    memcpy(err_copy, g_local_ingest_state.last_error, sizeof(err_copy));
    pthread_mutex_unlock(&g_local_ingest_state.lock);

    json_push_kv_int (out, "phase", phase);
    json_push_kv_str (out, "result_name",
                      local_ingest_result_name((enum local_ingest_result)result));
    json_push_kv_int (out, "result_code", result);
    json_push_kv_int (out, "started_at", started);
    json_push_kv_int (out, "finished_at", finished);
    json_push_kv_int (out, "blocks_done", bdone);
    json_push_kv_int (out, "blocks_total", btotal);
    json_push_kv_int (out, "utxos_imported", utxos);
    json_push_kv_int (out, "windows_verified", wins);
    json_push_kv_int (out, "windows_table_size",
                      (int64_t)g_sha3_windows_count);
    json_push_kv_str (out, "legacy_datadir", datadir_copy);
    json_push_kv_str (out, "last_error", err_copy);
    json_push_kv_bool(out, "in_progress",
                      (started > 0 && finished == 0));
    return true;
}
