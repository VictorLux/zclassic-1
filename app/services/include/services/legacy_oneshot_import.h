/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * legacy_oneshot_import — Wave S, milestone S-4b.
 *
 * "I have a legacy zclassicd running locally with the chain already
 * synced. Bring zclassic23 to the same tip in ~75 seconds, then exit
 * the procedure cleanly so Wave S stages + live P2P take over for new
 * blocks." That is the entirety of this module's contract.
 *
 * Differences from legacy_cold_import (the older sibling):
 *
 *   1. **Snapshots legacy LevelDBs via `ldb_snapshot_make`** so the
 *      operator does NOT need to stop zclassicd. The snapshot is built
 *      under `<our_datadir>/legacy-attach-stage/` (hardlinked SSTs +
 *      copied metadata + manifest_changed retry loop).
 *   2. **Idempotent + crash-safe via `progress_meta`.** A two-key
 *      protocol on progress.kv:
 *        - `import_in_progress` (sentinel, 1-byte blob) — set before
 *          step 1, cleared atomically with cursor stamps at the end.
 *        - `legacy_attach_tip_hash` (32B) + `legacy_attach_tip_height`
 *          (int32) — record the legacy tip this import attested to.
 *      Re-running with the same legacy tip is a fast no-op. Re-running
 *      with a higher legacy tip wipes + re-imports. Boot finding the
 *      sentinel present without a matching completion record triggers
 *      a wipe + retry on the next boot.
 *   3. **Atomic cursor stamp.** In one `BEGIN IMMEDIATE` txn on
 *      progress.kv this module:
 *        - UPSERTs stage_cursor row for "header_admit" = legacy_tip+1
 *        - UPSERTs stage_cursor row for "validate_headers" = legacy_tip+1
 *        - UPSERTs stage_cursor row for "body_fetch" = legacy_tip+1
 *        - WRITEs progress_meta `legacy_attach_tip_*` markers
 *        - DELETEs the `import_in_progress` sentinel
 *      Wave S stages then return STAGE_IDLE for every height ≤ stamp
 *      and only do real work for new blocks past legacy's tip — no
 *      re-Equihashing of immutable history.
 *
 * Tip activation is delegated to the existing CSR pending-anchor path
 * (`cold_import_pending_*` keys in node.db, resolved later in boot at
 * `boot_resolve_cold_import_pending_anchor`). This module writes those
 * keys at the end of step 5 just like legacy_cold_import does.
 *
 * The module does NOT poll, does NOT daemonize, does NOT spawn threads
 * of its own — it is one-shot blocking work that runs at boot before
 * the Wave S supervisor registers its children.
 *
 * Requires:
 *   - progress.kv already opened (`progress_store_open`).
 *   - block_tree_db (our LevelDB) already opened.
 *   - coins_view_sqlite already opened.
 *   - legacy_datadir contains `blocks/index/CURRENT` and
 *     `chainstate/CURRENT` (a real zclassicd datadir).
 *   - legacy_datadir on the same filesystem as our datadir for
 *     hardlinking blk*.dat (EXDEV falls back to copy — slow but
 *     correct).
 */

#ifndef ZCL_SERVICES_LEGACY_ONESHOT_IMPORT_H
#define ZCL_SERVICES_LEGACY_ONESHOT_IMPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct main_state;
struct coins_view_sqlite;
struct block_tree_db;
struct node_db;

/* Detail of why a one-shot import did or did not run. */
enum loi_outcome {
    LOI_OUTCOME_DID_IMPORT = 0,        /* full pipeline ran, state imported */
    LOI_OUTCOME_NOOP_SAME_TIP = 1,     /* already at legacy tip, no work */
    LOI_OUTCOME_RECOVERED_FROM_CRASH = 2, /* sentinel found, wiped + re-imported */
    LOI_OUTCOME_REFUSED_HAS_STATE = 3, /* our datadir has substantial state */
    LOI_OUTCOME_LEGACY_NOT_FOUND = 4,  /* legacy datadir missing or malformed */
    LOI_OUTCOME_FAILED = 5,            /* an import step failed */
};

struct loi_result {
    enum loi_outcome outcome;
    int32_t  legacy_tip_height;        /* max usable height in legacy index */
    int64_t  block_index_writes;
    int64_t  utxos_imported;
    int64_t  blk_files_linked;
    int64_t  stages_stamped;           /* count of stage_cursor UPSERTs */
    double   total_secs;
    bool     evidence_armed;           /* SHA3 spot-check passed */
    bool     ok;                       /* true iff outcome is DID_IMPORT,
                                        * NOOP_SAME_TIP, or RECOVERED */
};

/* Run the legacy-attach one-shot import. Returns true if the function
 * left the node in a usable state (DID_IMPORT, NOOP_SAME_TIP, RECOVERED);
 * false on FAILED. The two soft-skip outcomes (REFUSED_HAS_STATE,
 * LEGACY_NOT_FOUND) also return true — they are deliberate skips, not
 * errors, so boot continues normally.
 *
 * Blocking: typically completes in 60-120 s on a 3M-block legacy chain.
 * Watchdog: this function calls long_op_begin/_tick/_end internally so
 * the sync watchdog does not fire STATE_STUCK during the bulk work. */
bool legacy_oneshot_import_run(
    const char *our_datadir,
    const char *legacy_datadir,
    struct main_state *ms,
    struct coins_view_sqlite *cvs,
    struct node_db *ndb,
    struct block_tree_db *btdb,
    struct loi_result *out);

/* Convert an outcome to a short stable string for logging. */
const char *loi_outcome_name(enum loi_outcome o);

/* ── Drift gate ──────────────────────────────────────────────────────
 *
 * The list of Wave S stage names whose progress.kv cursors get stamped
 * to legacy_tip+1 at import-end. EVERY time a new Wave S stage ships:
 *
 *   1. Decide whether legacy-attach's imported state SATISFIES that
 *      stage's contract for heights ≤ legacy_tip.
 *      - For header_admit: yes — block_index entries are imported.
 *      - For validate_headers: yes — legacy already Equihash-checked.
 *      - For body_fetch: yes — BLOCK_HAVE_DATA set on imported entries.
 *      - For body_persist (S-5, pending): UNDECIDED — bodies live in
 *        hardlinked blk*.dat, not in blocks.log. Either teach
 *        body_persist that "blk*.dat counts as persisted" or do NOT
 *        add to LOI_STAGES_TO_STAMP. Either decision is fine; silently
 *        forgetting is the bug.
 *      - For S-6 script_validate / S-7 proof_validate: yes — legacy
 *        chain has been live-mined and validators have run.
 *      - For S-8 utxo_apply: yes — chainstate UTXOs are imported.
 *      - For S-9 tip_finalize: yes — but only if we're sure we want
 *        S-4b to claim authoritative tip-finalization.
 *
 *   2. If YES → add the stage name to LOI_STAGES_TO_STAMP[] in
 *      legacy_oneshot_import.c AND update the EXPECTED_LOI_STAGES list
 *      in lib/test/src/test_legacy_oneshot_import.c. The test fails
 *      if either side drifts.
 *
 *   3. If NO → do nothing. The stage will start at cursor=0 on the
 *      first boot after S-4b imports, which is the correct behavior
 *      for a stage that has work to do on imported heights.
 *
 * Iterate with: for (size_t i = 0; i < loi_stages_to_stamp_count();
 * i++) name = loi_stages_to_stamp_at(i); */
size_t      loi_stages_to_stamp_count(void);
const char *loi_stages_to_stamp_at(size_t i);

#endif /* ZCL_SERVICES_LEGACY_ONESHOT_IMPORT_H */
