/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * legacy_cold_import.c — see header.
 *
 * The pipeline does not call process_new_block or activate_best_chain
 * even once. It writes durable state directly:
 *
 *   - blk*.dat hardlinks (or copy fallback on EXDEV)
 *   - block-index records into our LevelDB
 *   - chainstate UTXOs into our coins.db
 *   - pending cold-import anchor metadata for CSR publication after
 *     boot has loaded the imported block index
 *
 * The normal boot then loads our LevelDB and chain_restore picks up
 * the pending anchor through CSR, populating active_chain by walking pprev.
 * bg_validation re-verifies every block bit-exact over the next hours.
 */

#include "platform/time_compat.h"
#include "services/legacy_cold_import.h"

#include "chain/chain.h"
#include "chain/chainparams.h"
#include "chain/sha3_windows.h"
#include "core/random.h"
#include "core/uint256.h"
#include "crypto/sha3.h"
#include "storage/block_index_db.h"
#include "storage/blocks_index_legacy_reader.h"
#include "storage/blocks_mmap_reader.h"
#include "storage/chainstate_legacy_reader.h"
#include "storage/coins_view_sqlite.h"
#include "storage/dbwrapper.h"
#include "models/database.h"
#include "util/log_macros.h"
#include "util/long_op.h"
#include "util/safe_alloc.h"
#include "util/thread_registry.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include "coins/coins_view.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Refuse to cold-import when our active tip is at or above this. The
 * threshold is intentionally generous — a fresh genesis-only install
 * has height 0, an aborted previous import might leave 1-100. */
#define LCI_REFUSE_ABOVE_TIP 1000

#define LCI_SPOTCHECK_K 5

static int64_t lci_now_ms(void)
{
    struct timespec ts;
    platform_time_monotonic_timespec(&ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Hash one SHA3 window using payloads from mmap. */
static bool lci_verify_window(struct blocks_mmap *bmr,
                              const struct legacy_block_loc *map,
                              size_t map_count,
                              size_t wi)
{
    if (wi >= g_sha3_windows_count) return false;
    int start = g_sha3_windows[wi].start_height;
    int end   = start + SHA3_WINDOW_SIZE - 1;
    if (end < 0 || (size_t)end >= map_count) return false;

    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);

    for (int h = start; h <= end; h++) {
        const struct legacy_block_loc *loc = &map[(size_t)h];
        if (loc->height < 0) return false;
        size_t len = 0;
        const uint8_t *bytes =
            bmr_get_payload(bmr, loc->nFile, loc->nDataPos, &len);
        if (!bytes || len == 0) return false;
        sha3_256_write(&ctx, bytes, len);
    }

    uint8_t digest[32];
    sha3_256_finalize(&ctx, digest);
    return memcmp(digest, g_sha3_windows[wi].hash, 32) == 0;
}

static bool lci_spotcheck(struct blocks_mmap *bmr,
                          const struct legacy_block_loc *map,
                          size_t map_count,
                          int legacy_tip,
                          int k)
{
    if (g_sha3_windows_count == 0) return false;
    size_t max_w = g_sha3_windows_count;
    if (legacy_tip > 0) {
        size_t covered = (size_t)(legacy_tip + 1) / SHA3_WINDOW_SIZE;
        if (covered < max_w) max_w = covered;
    }
    if (max_w == 0) return false;
    if ((size_t)k > max_w) k = (int)max_w;

    size_t picked[16];
    if (k > (int)(sizeof(picked) / sizeof(picked[0])))
        k = (int)(sizeof(picked) / sizeof(picked[0]));
    unsigned char rand_buf[16 * 4];
    GetRandBytes(rand_buf, sizeof(rand_buf));
    for (int i = 0; i < k; i++) {
        uint32_t r = (uint32_t)rand_buf[i*4]
                   | ((uint32_t)rand_buf[i*4+1] << 8)
                   | ((uint32_t)rand_buf[i*4+2] << 16)
                   | ((uint32_t)rand_buf[i*4+3] << 24);
        picked[i] = (size_t)(r % max_w);
    }

    fprintf(stderr, // obs-ok:pre-existing-diagnostic
            "[cold_import] SHA3 spotcheck: K=%d windows over [0..%zu) "
            "(legacy_tip=%d)\n", k, max_w, legacy_tip);
    for (int i = 0; i < k; i++) {
        if (!lci_verify_window(bmr, map, map_count, picked[i])) {
            fprintf(stderr,
                    "[cold_import] spotcheck FAILED at window %zu — "
                    "refusing to import\n", picked[i]);
            return false;
        }
        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                "[cold_import] spotcheck OK: w=%zu\n", picked[i]);
    }
    return true;
}

/* Hardlink (or copy on EXDEV) every blk*.dat from legacy/blocks/ into
 * our blocks/. Returns count of files linked, or -1 on fatal error.
 * Skips files that already exist in our blocks/. */
static int64_t lci_link_blk_files(const char *legacy_blocks_dir,
                                  const char *our_blocks_dir)
{
    DIR *d = opendir(legacy_blocks_dir);
    if (!d) {
        fprintf(stderr, "[cold_import] cannot opendir %s: %s\n",
                legacy_blocks_dir, strerror(errno));
        return -1;  // raw-return-ok:logged-above
    }
    /* Ensure our blocks dir exists. */
    mkdir(our_blocks_dir, 0755);

    int64_t linked = 0;
    int64_t copied = 0;
    int64_t skipped = 0;
    int64_t errors = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        size_t nlen = strlen(de->d_name);
        /* Match blkNNNNN.dat */
        if (nlen < 12 || strncmp(de->d_name, "blk", 3) != 0 ||
            strcmp(de->d_name + nlen - 4, ".dat") != 0)
            continue;
        char src[1024], dst[1024];
        snprintf(src, sizeof(src), "%s/%s", legacy_blocks_dir, de->d_name);
        snprintf(dst, sizeof(dst), "%s/%s", our_blocks_dir, de->d_name);
        struct stat st;
        if (stat(dst, &st) == 0) {
            skipped++;
            continue;  /* already present */
        }
        if (link(src, dst) == 0) {
            linked++;
            continue;
        }
        if (errno != EXDEV && errno != EPERM) {
            fprintf(stderr, // obs-ok:pre-existing-diagnostic
                    "[cold_import] link(%s -> %s) failed: %s\n",
                    src, dst, strerror(errno));
            errors++;
            continue;
        }
        /* Cross-FS or noperm: fall through to copy. */
        FILE *fsrc = fopen(src, "rb");
        FILE *fdst = fopen(dst, "wb");
        if (!fsrc || !fdst) {
            if (fsrc) fclose(fsrc);
            if (fdst) fclose(fdst);
            fprintf(stderr, // obs-ok:pre-existing-diagnostic
                    "[cold_import] open failed for copy %s -> %s\n",
                    src, dst);
            errors++;
            continue;
        }
        char buf[1u << 20];  /* 1 MB chunks */
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), fsrc)) > 0) {
            if (fwrite(buf, 1, n, fdst) != n) { errors++; break; }
        }
        fclose(fsrc);
        fclose(fdst);
        copied++;
    }
    closedir(d);
    fprintf(stderr, // obs-ok:pre-existing-diagnostic
            "[cold_import] blk files: linked=%" PRId64 " copied=%" PRId64
            " skipped=%" PRId64 " errors=%" PRId64 "\n",
            linked, copied, skipped, errors);
    return (errors > 0) ? -1 : (linked + copied);
}

/* Walk the legacy blocks/index/ LevelDB 'b' keys and write each record
 * to OUR LevelDB via WriteBatch. dbwrapper handles obfuscation on both
 * sides. Returns the number of records written, or -1 on error.
 *
 * Also returns by reference the (hash, height) of the entry with the
 * highest height — the legacy tip. */
static int64_t lci_copy_block_index(const char *legacy_blocks_index_dir,
                                    struct block_tree_db *our_btdb,
                                    struct uint256 *out_tip_hash,
                                    int32_t *out_tip_height)
{
    if (out_tip_height) *out_tip_height = -1;
    memset(out_tip_hash, 0, sizeof(*out_tip_hash));

    struct db_wrapper src;
    if (!db_wrapper_open(&src, legacy_blocks_index_dir,
                         16u << 20, false, false)) {
        fprintf(stderr,
                "[cold_import] cannot open %s — zclassicd still "
                "running? Stop it first.\n", legacy_blocks_index_dir);
        return -1;  // raw-return-ok:logged-above
    }

    struct db_wrapper *dst = &our_btdb->db;

    struct db_iterator it;
    db_iter_init(&it, &src);
    const char seek_key = 'b';
    db_iter_seek(&it, &seek_key, 1);

    struct db_batch batch;
    db_batch_init(&batch);
    int64_t written = 0;
    int64_t batch_fill = 0;
    enum { BATCH_LIMIT = 5000 };
    int32_t best_h = -1;

    /* WS-2a: surround the iteration with a long_op_scope so the sync
     * watchdog does not fire STATE_STUCK while we are quietly copying
     * the legacy LevelDB into ours. Each completed batch ticks. */
    struct long_op_scope lo_scope;
    long_op_begin(&lo_scope, "legacy_cold_import.bulk_copy");

    while (db_iter_valid(&it)) {
        if (thread_registry_shutdown_requested()) break;

        size_t klen = 0;
        const char *k = db_iter_key(&it, &klen);
        if (klen < 1 || k[0] != 'b') break;
        if (klen != 33) { db_iter_next(&it); continue; }

        size_t vlen = 0;
        const char *v = db_iter_value(&it, &vlen);
        if (!v || vlen == 0) { db_iter_next(&it); continue; }

        /* Peek into the serialized disk_block_index for height + status
         * filtering. We only want HAVE_DATA + !FAILED entries for our
         * imported index — fork entries with no data would just clutter
         * our block_map. */
        struct disk_block_index dbi;
        disk_block_index_init(&dbi);
        struct byte_stream s;
        stream_init_from_data(&s, (unsigned char *)v, vlen);
        bool ok = disk_block_index_deserialize(&dbi, &s);
        stream_free(&s);
        if (!ok) { db_iter_next(&it); continue; }

        bool have_data = (dbi.nStatus & BLOCK_HAVE_DATA) != 0;
        bool failed = (dbi.nStatus & BLOCK_FAILED_MASK) != 0;
        if (!have_data || failed) { db_iter_next(&it); continue; }

        if (dbi.nHeight > best_h) {
            best_h = dbi.nHeight;
            if (out_tip_hash)
                memcpy(out_tip_hash->data, k + 1, 32);
        }

        db_batch_put(&batch, k, klen, v, vlen);
        batch_fill++;
        written++;

        if (batch_fill >= BATCH_LIMIT) {
            if (!db_write_batch(dst, &batch, false)) {
                fprintf(stderr, // obs-ok:pre-existing-diagnostic
                        "[cold_import] db_write_batch failed\n");
                db_batch_free(&batch);
                db_iter_free(&it);
                db_wrapper_close(&src);
                long_op_end(&lo_scope);
                return -1;  // raw-return-ok:logged-above
            }
            db_batch_clear(&batch);
            batch_fill = 0;
            long_op_tick(&lo_scope);
        }

        db_iter_next(&it);
    }

    if (batch_fill > 0) {
        if (!db_write_batch(dst, &batch, false)) {
            fprintf(stderr, // obs-ok:pre-existing-diagnostic
                    "[cold_import] final db_write_batch failed\n");
            db_batch_free(&batch);
            db_iter_free(&it);
            db_wrapper_close(&src);
            long_op_end(&lo_scope);
            return -1;  // raw-return-ok:logged-above
        }
    }
    db_batch_free(&batch);
    db_iter_free(&it);
    db_wrapper_close(&src);
    long_op_end(&lo_scope);

    if (out_tip_height) *out_tip_height = best_h;
    return written;
}

/* Bulk-import chainstate UTXOs from legacy. */
struct lci_chainstate_ctx {
    struct utxo_bulk_rec *batch;
    size_t fill;
    size_t cap;
    struct coins_view_sqlite *cvs;
    int64_t inserted;
    int64_t records;
    int errors;
};
static bool lci_chainstate_flush(struct lci_chainstate_ctx *c)
{
    if (c->fill == 0) return true;
    int64_t w = coins_view_sqlite_bulk_insert(c->cvs, c->batch, c->fill);
    if (w != (int64_t)c->fill) {
        c->errors++;
        return false;
    }
    c->inserted += w;
    c->fill = 0;
    return true;
}
static bool lci_chainstate_cb(const struct uint256 *txid,
                              const struct legacy_coins *lc,
                              void *vctx)
{
    struct lci_chainstate_ctx *c = vctx;
    if (thread_registry_shutdown_requested()) return false;
    c->records++;
    for (size_t i = 0; i < lc->num_vouts; i++) {
        if (c->fill >= c->cap) {
            if (!lci_chainstate_flush(c)) return false;
        }
        c->batch[c->fill++] = (struct utxo_bulk_rec){
            .txid = txid->data,
            .vout = lc->vouts[i].n,
            .value = lc->vouts[i].value,
            .script = lc->vouts[i].script,
            .script_len = (uint32_t)lc->vouts[i].script_len,
            .height = (uint32_t)lc->height,
            .is_coinbase = lc->coinbase ? 1u : 0u,
        };
    }
    return true;
}

bool legacy_cold_import_blocking(
    struct main_state *ms,
    struct coins_view_sqlite *cvs,
    struct node_db *ndb,
    struct block_tree_db *btdb,
    const char *our_datadir,
    const char *legacy_datadir,
    struct lci_cold_result *out)
{
    struct lci_cold_result r = (struct lci_cold_result){0};
    r.legacy_tip = -1;
    if (out) *out = r;

    if (!ms || !cvs || !ndb || !ndb->open || !btdb ||
        !our_datadir || !legacy_datadir) {
        LOG_FAIL("legacy_cold_import", "bad args");
    }

    int our_tip = active_chain_height(&ms->chain_active);
    if (our_tip > LCI_REFUSE_ABOVE_TIP) {
        fprintf(stderr,
                "[cold_import] REFUSING: our active_tip=%d > %d. "
                "Cold-import is for empty datadirs; use -fastimport for "
                "warm catch-up.\n",
                our_tip, LCI_REFUSE_ABOVE_TIP);
        return false;
    }

    char idx_dir[1024], blk_dir[1024];
    snprintf(idx_dir, sizeof(idx_dir), "%s/blocks/index", legacy_datadir);
    snprintf(blk_dir, sizeof(blk_dir), "%s/blocks", legacy_datadir);
    char our_blocks[1024];
    snprintf(our_blocks, sizeof(our_blocks), "%s/blocks", our_datadir);

    int64_t t_start = lci_now_ms();

    /* ── Build height map ──────────────────────────────────── */
    struct bilr *bilr = NULL;
    if (!bilr_open(idx_dir, &bilr)) {
        fprintf(stderr,
                "[cold_import] bilr_open %s failed\n", idx_dir);
        return false;
    }
    struct legacy_block_loc *map = NULL;
    size_t map_count = 0;
    if (!bilr_load_height_map(bilr, &map, &map_count)) {
        bilr_close(bilr);
        return false;
    }
    int legacy_tip = (int)map_count - 1;
    while (legacy_tip > 0 && map[(size_t)legacy_tip].height < 0)
        legacy_tip--;
    r.legacy_tip = legacy_tip;
    fprintf(stderr, // obs-ok:pre-existing-diagnostic
            "[cold_import] legacy tip h=%d (map size=%zu)\n",
            legacy_tip, map_count);

    /* ── SHA3 spot-check ──────────────────────────────────── */
    struct blocks_mmap *bmr = NULL;
    if (!bmr_open(blk_dir, &bmr)) {
        bilr_free_height_map(map);
        bilr_close(bilr);
        return false;
    }
    bool evidence_ok = lci_spotcheck(bmr, map, map_count,
                                   legacy_tip, LCI_SPOTCHECK_K);
    bmr_close(bmr);
    if (!evidence_ok) {
        bilr_free_height_map(map);
        bilr_close(bilr);
        fprintf(stderr,
                "[cold_import] aborting due to spotcheck failure\n");
        return false;
    }
    r.evidence_armed = true;

    /* ── Hardlink blk*.dat files ─────────────────────────── */
    int64_t t_link = lci_now_ms();
    int64_t linked = lci_link_blk_files(blk_dir, our_blocks);
    if (linked < 0) {
        bilr_free_height_map(map);
        bilr_close(bilr);
        return false;
    }
    r.blk_files_linked = linked;
    fprintf(stderr, // obs-ok:pre-existing-diagnostic
            "[cold_import] blk linking took %" PRId64 " ms\n",
            lci_now_ms() - t_link);

    /* ── Bulk-copy block_index into our LevelDB ──────────── */
    int64_t t_bi = lci_now_ms();
    struct uint256 legacy_tip_hash;
    int32_t legacy_tip_h = -1;
    int64_t bi_written = lci_copy_block_index(idx_dir, btdb,
                                               &legacy_tip_hash,
                                               &legacy_tip_h);
    bilr_free_height_map(map);
    bilr_close(bilr);
    if (bi_written < 0) return false;
    r.block_index_writes = bi_written;
    fprintf(stderr, // obs-ok:pre-existing-diagnostic
            "[cold_import] block_index copy: %" PRId64 " entries "
            "in %" PRId64 " ms (best h=%d)\n",
            bi_written, lci_now_ms() - t_bi, legacy_tip_h);

    /* ── Bulk-import chainstate UTXOs ─────────────────────── */
    char cs_dir[1024];
    snprintf(cs_dir, sizeof(cs_dir), "%s/chainstate", legacy_datadir);
    void *cs = NULL;
    if (!chainstate_legacy_open(cs_dir, &cs)) {
        fprintf(stderr,
                "[cold_import] chainstate_legacy_open %s failed\n", cs_dir);
        return false;
    }
    int64_t t_cs = lci_now_ms();

    enum { BATCH = 5000 };
    struct utxo_bulk_rec *batch =
        zcl_malloc(sizeof(*batch) * BATCH, "lci.batch");
    if (!batch) {
        chainstate_legacy_close(cs);
        return false;
    }
    struct lci_chainstate_ctx ctx = {
        .batch = batch, .fill = 0, .cap = BATCH,
        .cvs = cvs, .inserted = 0, .records = 0, .errors = 0,
    };
    int64_t n = chainstate_legacy_iter(cs, lci_chainstate_cb, &ctx);
    if (n >= 0 && ctx.fill > 0) lci_chainstate_flush(&ctx);

    struct uint256 cs_best;
    bool got_best = chainstate_legacy_get_best_block(cs, &cs_best);
    chainstate_legacy_close(cs);
    free(batch);

    if (n < 0 || ctx.errors > 0) {
        fprintf(stderr,
                "[cold_import] chainstate import failed "
                "(iter=%" PRId64 " errors=%d)\n", n, ctx.errors);
        return false;
    }
    r.utxos_imported = ctx.inserted;
    fprintf(stderr, // obs-ok:pre-existing-diagnostic
            "[cold_import] chainstate: %" PRId64 " UTXOs from "
            "%" PRId64 " records in %" PRId64 " ms\n",
            ctx.inserted, ctx.records, lci_now_ms() - t_cs);

    /* ── Record an unpublished anchor for post-index CSR publication ── */
    if (got_best) {
        bool pending_ok =
            node_db_state_set(ndb, "cold_import_pending_coins_best_block",
                              cs_best.data, 32) &&
            node_db_state_set(ndb, "cold_import_pending_coins_best_height",
                              &legacy_tip_h, sizeof(legacy_tip_h)) &&
            node_db_state_set(ndb, "cold_import_pending_utxo_count",
                              &ctx.inserted, sizeof(ctx.inserted));
        if (!pending_ok) {
            fprintf(stderr,
                    "[cold_import] failed to persist pending CSR anchor\n");
            return false;
        }
        char hex[65] = {0};
        for (int i = 0; i < 32; i++)
            snprintf(hex + i*2, 3, "%02x", cs_best.data[31 - i]);
        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                "[cold_import] pending CSR anchor recorded %s h=%d\n",
                hex, legacy_tip_h);
    } else {
        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                "[cold_import] WARNING: legacy chainstate had no 'B' key; "
                "pending CSR anchor not recorded\n");
    }

    r.total_secs = (double)(lci_now_ms() - t_start) / 1000.0;
    r.ok = true;
    if (out) *out = r;
    fprintf(stderr, // obs-ok:pre-existing-diagnostic
            "[cold_import] DONE in %.1fs: block_index=%" PRId64
            " utxos=%" PRId64 " blk_files=%" PRId64 "\n",
            r.total_secs, r.block_index_writes, r.utxos_imported,
            r.blk_files_linked);
    return true;
}
