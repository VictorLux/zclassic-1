/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * File Transfer Controller — SHA3-verified chunk service.
 * Each block file is split into 50MB chunks, SHA3-256 hashed.
 * Chunks are served by hash over REST, RPC, and P2P. */

#include "controllers/file_controller.h"
#include "controllers/strong_params.h"
#include "chain/mmr.h"
#include "crypto/sha3.h"
#include "json/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <sqlite3.h>

struct file_context {
    const char *datadir;
    struct file_manifest manifest;
    bool manifest_valid;
};

static struct file_context g_file_ctx = {0};

static struct file_context *file_ctx(void)
{
    return &g_file_ctx;
}

void file_controller_init(const char *datadir)
{
    struct file_context *ctx = file_ctx();
    ctx->datadir = datadir;
    memset(&ctx->manifest, 0, sizeof(ctx->manifest));
    ctx->manifest_valid = false;
}

const struct file_manifest *file_controller_get_manifest(void)
{
    struct file_context *ctx = file_ctx();
    return ctx->manifest_valid ? &ctx->manifest : NULL;
}

/* ── Build manifest from block files ───────────────────────────── */

static bool hash_file_chunks(const char *path, uint8_t file_index,
                              struct file_manifest *fm)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0) { fclose(f); return true; }

    uint8_t *buf = malloc(FILE_CHUNK_SIZE);
    if (!buf) { fclose(f); return false; }

    uint64_t offset = 0;
    while (offset < (uint64_t)file_size &&
           fm->num_chunks < FILE_MAX_CHUNKS) {
        uint32_t to_read = FILE_CHUNK_SIZE;
        if (offset + to_read > (uint64_t)file_size)
            to_read = (uint32_t)((uint64_t)file_size - offset);

        size_t got = fread(buf, 1, to_read, f);
        if (got != to_read) { free(buf); fclose(f); return false; }

        struct file_chunk *chunk = &fm->chunks[fm->num_chunks];
        chunk->offset = offset;
        chunk->size = to_read;
        chunk->file_index = file_index;

        /* SHA3-256 hash of chunk data */
        sha3_256(buf, to_read, chunk->sha3);

        fm->total_bytes += to_read;
        fm->num_chunks++;
        offset += to_read;
    }

    free(buf);
    fclose(f);
    return true;
}

/* Save manifest to disk for instant reload on restart */
static bool file_manifest_save(const struct file_manifest *fm,
                                const char *datadir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/file_manifest.bin", datadir);
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    /* Write header: magic + num_chunks + total_bytes + root_hash */
    uint32_t magic = 0x464D414E; /* "FMAN" */
    fwrite(&magic, 4, 1, f);
    fwrite(&fm->num_chunks, 4, 1, f);
    fwrite(&fm->total_bytes, 8, 1, f);
    fwrite(fm->root_hash, 32, 1, f);
    /* Write chunks */
    for (uint32_t i = 0; i < fm->num_chunks; i++) {
        fwrite(fm->chunks[i].sha3, 32, 1, f);
        fwrite(&fm->chunks[i].offset, 8, 1, f);
        fwrite(&fm->chunks[i].size, 4, 1, f);
        fwrite(&fm->chunks[i].file_index, 1, 1, f);
    }
    fflush(f);
    fdatasync(fileno(f));
    fclose(f);
    return true;
}

/* Load cached manifest from disk — instant startup */
static bool file_manifest_load(struct file_manifest *fm,
                                const char *datadir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/file_manifest.bin", datadir);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    memset(fm, 0, sizeof(*fm));
    uint32_t magic = 0;
    if (fread(&magic, 4, 1, f) != 1 || magic != 0x464D414E) {
        fclose(f); return false;
    }
    fread(&fm->num_chunks, 4, 1, f);
    fread(&fm->total_bytes, 8, 1, f);
    fread(fm->root_hash, 32, 1, f);
    if (fm->num_chunks > FILE_MAX_CHUNKS) { fclose(f); return false; }
    for (uint32_t i = 0; i < fm->num_chunks; i++) {
        fread(fm->chunks[i].sha3, 32, 1, f);
        fread(&fm->chunks[i].offset, 8, 1, f);
        fread(&fm->chunks[i].size, 4, 1, f);
        fread(&fm->chunks[i].file_index, 1, 1, f);
    }
    fclose(f);
    /* Verify block files still match — check first AND last chunk.
     * The last chunk is most likely to be stale (active block file). */
    if (fm->num_chunks > 0) {
        uint8_t *data = NULL;
        uint32_t sz = 0;
        if (!file_chunk_read(&fm->chunks[0], datadir, &data, &sz)) {
            printf("file_manifest: first chunk stale, rebuilding\n");
            return false;
        }
        free(data);
        /* Also check last chunk — most likely to be stale */
        if (fm->num_chunks > 1) {
            data = NULL; sz = 0;
            if (!file_chunk_read(&fm->chunks[fm->num_chunks - 1],
                                  datadir, &data, &sz)) {
                printf("file_manifest: last chunk stale, rebuilding\n");
                return false;
            }
            free(data);
        }
        /* Check if any block file was modified after manifest cache.
         * If so, the manifest is stale. */
        struct stat cache_st;
        char cache_path[512];
        snprintf(cache_path, sizeof(cache_path),
                 "%s/file_manifest.bin", datadir);
        if (stat(cache_path, &cache_st) == 0) {
            uint8_t last_fi = fm->chunks[fm->num_chunks - 1].file_index;
            char blk_path[576];
            snprintf(blk_path, sizeof(blk_path),
                     "%s/blocks/blk%05d.dat", datadir, last_fi);
            struct stat blk_st;
            if (stat(blk_path, &blk_st) == 0 &&
                blk_st.st_mtime > cache_st.st_mtime) {
                printf("file_manifest: blk%05d.dat modified after cache, "
                       "rebuilding\n", last_fi);
                return false;
            }
        }
    }
    printf("file_manifest: loaded cached manifest (%u chunks, %.1f GB)\n",
           fm->num_chunks,
           (double)fm->total_bytes / (1024.0*1024.0*1024.0));
    return true;
}

bool file_manifest_build(struct file_manifest *fm, const char *datadir)
{
    /* Try loading cached manifest first — instant startup */
    if (file_manifest_load(fm, datadir))
        return true;

    memset(fm, 0, sizeof(*fm));

    char blocks_dir[512];
    snprintf(blocks_dir, sizeof(blocks_dir), "%s/blocks", datadir);

    /* Count block files first to find the last one */
    int num_files = 0;
    for (int i = 0; i < 256; i++) {
        char path[576];
        snprintf(path, sizeof(path), "%s/blk%05d.dat", blocks_dir, i);
        struct stat st;
        if (stat(path, &st) != 0) break;
        num_files++;
    }

    /* Scan block files. Skip any file modified in the last hour —
     * the node may be actively appending blocks via P2P, which
     * changes the file after we hash it, causing SHA3 mismatches.
     * Only serve stable, immutable files. */
    int64_t cutoff = (int64_t)time(NULL) - 3600;
    for (int i = 0; i < num_files; i++) {
        char path[576];
        snprintf(path, sizeof(path), "%s/blk%05d.dat", blocks_dir, i);

        struct stat st;
        if (stat(path, &st) != 0) break;
        if (st.st_size == 0) continue;
        if ((int64_t)st.st_mtime > cutoff) {
            printf("file_manifest: skipping %s (modified %llds ago)\n",
                   path, (long long)((int64_t)time(NULL) - (int64_t)st.st_mtime));
            continue;
        }

        if (!hash_file_chunks(path, (uint8_t)i, fm))
            return false;
    }

    /* Serve block_index.bin (flat file, ~409MB) as file_index=253.
     * Saves 116s LevelDB scan on first boot. Block index is safe to
     * transfer — it only contains PoW-verified header metadata. */
    {
        char flat_path[576];
        snprintf(flat_path, sizeof(flat_path), "%s/block_index.bin", datadir);
        struct stat flat_st;
        if (stat(flat_path, &flat_st) == 0 && flat_st.st_size > 1000000) {
            printf("file_manifest: adding block_index.bin (%.0f MB)\n",
                   (double)flat_st.st_size / (1024.0*1024.0));
            hash_file_chunks(flat_path, 253, fm);
        }
    }

    /* Serve consensus snapshot as file_index=254.
     * SECURITY: node.db contains private wallet keys/txns.
     * We export ONLY public consensus tables (blocks, utxos,
     * addresses, chain_stats) into a separate snapshot file.
     * This file is SHA3-verified and safe to share with any peer. */
    {
        char snap_path[576];
        snprintf(snap_path, sizeof(snap_path),
                 "%s/consensus_snapshot.db", datadir);
        struct stat snap_st;
        if (stat(snap_path, &snap_st) == 0 && snap_st.st_size > 1000000) {
            printf("file_manifest: adding consensus_snapshot.db (%.0f MB)\n",
                   (double)snap_st.st_size / (1024.0*1024.0));
            hash_file_chunks(snap_path, 254, fm);
        }
    }

    if (fm->num_chunks == 0) return false;

    /* Compute root hash: SHA3-256 of all chunk hashes concatenated */
    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);
    for (uint32_t i = 0; i < fm->num_chunks; i++)
        sha3_256_write(&ctx, fm->chunks[i].sha3, 32);
    sha3_256_finalize(&ctx, fm->root_hash);

    /* Embed current MMR root as trust anchor.
     * Receivers verify: MMR root matches expected value for this chain.
     * This binds the file data to the PoW-secured block hash chain. */
    {
        extern struct mmr *rpc_blockchain_get_mmr(void);
        struct mmr *m = rpc_blockchain_get_mmr();
        if (m && m->num_leaves > 0) {
            mmr_root(m, fm->mmr_root);
            fm->chain_height = (int32_t)m->num_leaves;
        } else {
            memset(fm->mmr_root, 0, 32);
        }
    }

    printf("File manifest: %u chunks, %llu bytes, root=",
           fm->num_chunks, (unsigned long long)fm->total_bytes);
    for (int i = 0; i < 8; i++) printf("%02x", fm->root_hash[i]);
    printf("...\n");

    /* Cache to disk for instant reload on next restart */
    file_manifest_save(fm, datadir);

    return true;
}

const struct file_chunk *file_manifest_find(const struct file_manifest *fm,
                                             const uint8_t sha3[32])
{
    for (uint32_t i = 0; i < fm->num_chunks; i++) {
        if (memcmp(fm->chunks[i].sha3, sha3, 32) == 0)
            return &fm->chunks[i];
    }
    return NULL;
}

bool file_chunk_read(const struct file_chunk *chunk, const char *datadir,
                     uint8_t **out, uint32_t *out_size)
{
    char path[576];
    if (chunk->file_index == 254)
        snprintf(path, sizeof(path), "%s/consensus_snapshot.db", datadir);
    else if (chunk->file_index == 253)
        snprintf(path, sizeof(path), "%s/block_index.bin", datadir);
    else
        snprintf(path, sizeof(path), "%s/blocks/blk%05d.dat",
                 datadir, chunk->file_index);

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    if (fseek(f, (long)chunk->offset, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }

    uint8_t *buf = malloc(chunk->size);
    if (!buf) { fclose(f); return false; }

    size_t got = fread(buf, 1, chunk->size, f);
    fclose(f);

    if (got != chunk->size) { free(buf); return false; }

    /* Verify SHA3 hash matches manifest */
    uint8_t hash[32];
    sha3_256(buf, chunk->size, hash);
    if (memcmp(hash, chunk->sha3, 32) != 0) {
        fprintf(stderr, "file_chunk_read: SHA3 mismatch for chunk at "
                "file=%d offset=%llu\n",
                chunk->file_index, (unsigned long long)chunk->offset);
        free(buf);
        return false;
    }

    *out = buf;
    *out_size = chunk->size;
    return true;
}

/* ── Consensus snapshot export ─────────────────────────────────── */
/* Exports ONLY public consensus tables from node.db.
 * NEVER exports: wallet_keys, wallet_utxos, wallet_sapling_keys,
 * wallet_sapling_notes, wallet_sapling_witnesses, node_state.
 * The exported file is safe to share with any peer. */

bool file_export_consensus_snapshot(const char *datadir)
{
    char src_path[576], dst_path[576];
    snprintf(src_path, sizeof(src_path), "%s/node.db", datadir);
    snprintf(dst_path, sizeof(dst_path), "%s/consensus_snapshot.db", datadir);

    struct stat src_st;
    if (stat(src_path, &src_st) != 0 || src_st.st_size < 1000000)
        return false;

    /* Remove old snapshot */
    unlink(dst_path);

    /* Open source read-only */
    sqlite3 *src_db = NULL;
    if (sqlite3_open_v2(src_path, &src_db, SQLITE_OPEN_READONLY, NULL)
        != SQLITE_OK) return false;

    /* Create destination and copy only consensus tables via ATTACH */
    sqlite3 *dst_db = NULL;
    if (sqlite3_open_v2(dst_path, &dst_db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) {
        sqlite3_close(src_db);
        return false;
    }

    /* Performance: batch everything in one transaction */
    sqlite3_exec(dst_db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(dst_db, "PRAGMA synchronous=OFF", NULL, NULL, NULL);
    sqlite3_exec(dst_db, "PRAGMA cache_size=-262144", NULL, NULL, NULL);

    /* Attach source database */
    char attach_sql[600];
    snprintf(attach_sql, sizeof(attach_sql),
             "ATTACH DATABASE '%s' AS src", src_path);
    if (sqlite3_exec(dst_db, attach_sql, NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "consensus_snapshot: ATTACH failed: %s\n",
                sqlite3_errmsg(dst_db));
        sqlite3_close(dst_db);
        sqlite3_close(src_db);
        unlink(dst_path);
        return false;
    }

    /* Copy ONLY public consensus tables.
     * This whitelist approach is safer than a blacklist — new private
     * tables added later won't accidentally get exported. */
    static const char *safe_tables[] = {
        "blocks", "transactions", "utxos", "addresses",
        "chain_stats", "zslp_tokens", "zslp_balances",
        NULL
    };

    sqlite3_exec(dst_db, "BEGIN", NULL, NULL, NULL);
    int tables_copied = 0;
    for (int i = 0; safe_tables[i]; i++) {
        /* Create table with same schema */
        char create_sql[512];
        snprintf(create_sql, sizeof(create_sql),
            "CREATE TABLE IF NOT EXISTS %s AS SELECT * FROM src.%s",
            safe_tables[i], safe_tables[i]);
        int rc = sqlite3_exec(dst_db, create_sql, NULL, NULL, NULL);
        if (rc == SQLITE_OK) {
            tables_copied++;
        }
        /* Silently skip tables that don't exist in source */
    }

    /* Store snapshot metadata */
    sqlite3_exec(dst_db,
        "CREATE TABLE IF NOT EXISTS _snapshot_meta "
        "(key TEXT PRIMARY KEY, value TEXT)", NULL, NULL, NULL);

    /* Get source chain height */
    sqlite3_stmt *sel = NULL;
    sqlite3_prepare_v2(dst_db,
        "SELECT MAX(height) FROM blocks", -1, &sel, NULL);
    int snap_height = 0;
    if (sel && sqlite3_step(sel) == SQLITE_ROW)
        snap_height = sqlite3_column_int(sel, 0);
    if (sel) sqlite3_finalize(sel);

    char meta_sql[256];
    snprintf(meta_sql, sizeof(meta_sql),
        "INSERT INTO _snapshot_meta VALUES('height','%d')", snap_height);
    sqlite3_exec(dst_db, meta_sql, NULL, NULL, NULL);
    snprintf(meta_sql, sizeof(meta_sql),
        "INSERT INTO _snapshot_meta VALUES('tables','%d')", tables_copied);
    sqlite3_exec(dst_db, meta_sql, NULL, NULL, NULL);

    sqlite3_exec(dst_db, "COMMIT", NULL, NULL, NULL);
    sqlite3_exec(dst_db, "DETACH DATABASE src", NULL, NULL, NULL);

    /* Compact */
    sqlite3_exec(dst_db, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);
    sqlite3_exec(dst_db, "VACUUM", NULL, NULL, NULL);

    sqlite3_close(dst_db);
    sqlite3_close(src_db);

    struct stat dst_st;
    stat(dst_path, &dst_st);
    printf("Consensus snapshot: %d tables, height %d, %.0f MB\n",
           tables_copied, snap_height,
           (double)dst_st.st_size / (1024.0*1024.0));

    return tables_copied > 0;
}

/* ── RPC handlers ──────────────────────────────────────────────── */

static bool rpc_getfilemanifest(const struct json_value *params, bool help,
                                 struct json_value *result)
{
    struct file_context *ctx = file_ctx();
    (void)params;
    RPC_HELP(help, result,
        "getfilemanifest\n"
        "\nReturn SHA3-verified file manifest for blockchain data.\n"
        "\nResult:\n"
        "  { root_hash, chain_height, total_bytes, num_chunks, chunks[] }\n");

    /* Build manifest on first call if not cached */
    if (!ctx->manifest_valid && ctx->datadir) {
        ctx->manifest_valid = file_manifest_build(&ctx->manifest, ctx->datadir);
    }

    if (!ctx->manifest_valid) {
        json_set_str(result, "error: no block files found");
        return true;
    }

    json_set_object(result);

    char root_hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(root_hex + i * 2, 3, "%02x", ctx->manifest.root_hash[i]);
    json_push_kv_str(result, "root_hash", root_hex);
    json_push_kv_int(result, "num_chunks", (int64_t)ctx->manifest.num_chunks);
    json_push_kv_int(result, "total_bytes", (int64_t)ctx->manifest.total_bytes);

    /* Chunk list */
    struct json_value chunks_arr;
    json_set_array(&chunks_arr);
    for (uint32_t i = 0; i < ctx->manifest.num_chunks; i++) {
        struct json_value chunk_obj;
        json_set_object(&chunk_obj);

        char hex[65];
        for (int j = 0; j < 32; j++)
            snprintf(hex + j * 2, 3, "%02x", ctx->manifest.chunks[i].sha3[j]);
        json_push_kv_str(&chunk_obj, "sha3", hex);
        json_push_kv_int(&chunk_obj, "size",
                          (int64_t)ctx->manifest.chunks[i].size);
        json_push_kv_int(&chunk_obj, "file_index",
                          (int64_t)ctx->manifest.chunks[i].file_index);
        json_push_kv_int(&chunk_obj, "offset",
                          (int64_t)ctx->manifest.chunks[i].offset);

        json_push_back(&chunks_arr, &chunk_obj);
    }
    json_push_kv(result, "chunks", &chunks_arr);

    return true;
}

static bool rpc_getfilechunk(const struct json_value *params, bool help,
                              struct json_value *result)
{
    struct file_context *ctx = file_ctx();
    RPC_HELP(help, result,
        "getfilechunk \"sha3hash\"\n"
        "\nReturn a file chunk by its SHA3-256 hash.\n"
        "\nArguments:\n"
        "  1. sha3hash (string, required) — 64-char hex SHA3 hash\n"
        "\nResult: { size, sha3, data_hex } or error\n");

    if (!ctx->manifest_valid || !params) {
        json_set_str(result, "error: file manifest not initialized");
        return true;
    }

    const struct json_value *arg0 = json_at(params, 0);
    const char *hex = arg0 ? json_get_str(arg0) : NULL;
    if (!hex || strlen(hex) != 64) {
        json_set_str(result, "error: sha3hash must be 64 hex chars");
        return true;
    }

    /* Parse hex → bytes */
    uint8_t sha3[32];
    for (int i = 0; i < 32; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) {
            json_set_str(result, "error: invalid hex");
            return true;
        }
        sha3[i] = (uint8_t)byte;
    }

    const struct file_chunk *chunk = file_manifest_find(&ctx->manifest, sha3);
    if (!chunk) {
        json_set_str(result, "error: chunk not found");
        return true;
    }

    uint8_t *data = NULL;
    uint32_t data_size = 0;
    if (!ctx->datadir || !file_chunk_read(chunk, ctx->datadir, &data, &data_size)) {
        json_set_str(result, "error: failed to read chunk from disk");
        return true;
    }

    json_set_object(result);
    json_push_kv_int(result, "size", (int64_t)data_size);
    json_push_kv_str(result, "sha3", hex);
    /* For large chunks, return size only — use REST API for data */
    json_push_kv_str(result, "download",
                      "use GET /api/files/<sha3hash> for raw data");

    free(data);
    return true;
}

void register_file_rpc_commands(struct rpc_table *t)
{
    struct rpc_command cmds[] = {
        { "files", "getfilemanifest", rpc_getfilemanifest, true },
        { "files", "getfilechunk",    rpc_getfilechunk,    true },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_append(t, &cmds[i]);
}
