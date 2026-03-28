/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * File Transfer Controller — SHA3-verified chunk service.
 * Each block file is split into 50MB chunks, SHA3-256 hashed.
 * Chunks are served by hash over REST, RPC, and P2P. */

#include "controllers/file_controller.h"
#include "controllers/strong_params.h"
#include "crypto/sha3.h"
#include "json/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

static const char *g_file_datadir = NULL;
static struct file_manifest g_manifest;
static bool g_manifest_valid = false;

void file_controller_init(const char *datadir)
{
    g_file_datadir = datadir;
    memset(&g_manifest, 0, sizeof(g_manifest));
    g_manifest_valid = false;
}

const struct file_manifest *file_controller_get_manifest(void)
{
    return g_manifest_valid ? &g_manifest : NULL;
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

bool file_manifest_build(struct file_manifest *fm, const char *datadir)
{
    memset(fm, 0, sizeof(*fm));

    char blocks_dir[512];
    snprintf(blocks_dir, sizeof(blocks_dir), "%s/blocks", datadir);

    /* Scan block files: blk00000.dat, blk00001.dat, ... */
    for (int i = 0; i < 256; i++) {
        char path[576];
        snprintf(path, sizeof(path), "%s/blk%05d.dat", blocks_dir, i);

        struct stat st;
        if (stat(path, &st) != 0) break; /* no more files */
        if (st.st_size == 0) continue;

        if (!hash_file_chunks(path, (uint8_t)i, fm))
            return false;
    }

    if (fm->num_chunks == 0) return false;

    /* Compute root hash: SHA3-256 of all chunk hashes concatenated */
    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);
    for (uint32_t i = 0; i < fm->num_chunks; i++)
        sha3_256_write(&ctx, fm->chunks[i].sha3, 32);
    sha3_256_finalize(&ctx, fm->root_hash);

    printf("File manifest: %u chunks, %llu bytes, root=",
           fm->num_chunks, (unsigned long long)fm->total_bytes);
    for (int i = 0; i < 8; i++) printf("%02x", fm->root_hash[i]);
    printf("...\n");

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

/* ── RPC handlers ──────────────────────────────────────────────── */

static bool rpc_getfilemanifest(const struct json_value *params, bool help,
                                 struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "getfilemanifest\n"
        "\nReturn SHA3-verified file manifest for blockchain data.\n"
        "\nResult:\n"
        "  { root_hash, chain_height, total_bytes, num_chunks, chunks[] }\n");

    /* Build manifest on first call if not cached */
    if (!g_manifest_valid && g_file_datadir) {
        g_manifest_valid = file_manifest_build(&g_manifest, g_file_datadir);
    }

    if (!g_manifest_valid) {
        json_set_str(result, "error: no block files found");
        return true;
    }

    json_set_object(result);

    char root_hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(root_hex + i * 2, 3, "%02x", g_manifest.root_hash[i]);
    json_push_kv_str(result, "root_hash", root_hex);
    json_push_kv_int(result, "num_chunks", (int64_t)g_manifest.num_chunks);
    json_push_kv_int(result, "total_bytes", (int64_t)g_manifest.total_bytes);

    /* Chunk list */
    struct json_value chunks_arr;
    json_set_array(&chunks_arr);
    for (uint32_t i = 0; i < g_manifest.num_chunks; i++) {
        struct json_value chunk_obj;
        json_set_object(&chunk_obj);

        char hex[65];
        for (int j = 0; j < 32; j++)
            snprintf(hex + j * 2, 3, "%02x", g_manifest.chunks[i].sha3[j]);
        json_push_kv_str(&chunk_obj, "sha3", hex);
        json_push_kv_int(&chunk_obj, "size",
                          (int64_t)g_manifest.chunks[i].size);
        json_push_kv_int(&chunk_obj, "file_index",
                          (int64_t)g_manifest.chunks[i].file_index);
        json_push_kv_int(&chunk_obj, "offset",
                          (int64_t)g_manifest.chunks[i].offset);

        json_push_back(&chunks_arr, &chunk_obj);
    }
    json_push_kv(result, "chunks", &chunks_arr);

    return true;
}

static bool rpc_getfilechunk(const struct json_value *params, bool help,
                              struct json_value *result)
{
    RPC_HELP(help, result,
        "getfilechunk \"sha3hash\"\n"
        "\nReturn a file chunk by its SHA3-256 hash.\n"
        "\nArguments:\n"
        "  1. sha3hash (string, required) — 64-char hex SHA3 hash\n"
        "\nResult: { size, sha3, data_hex } or error\n");

    if (!g_manifest_valid || !params)
        return false;

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

    const struct file_chunk *chunk = file_manifest_find(&g_manifest, sha3);
    if (!chunk) {
        json_set_str(result, "error: chunk not found");
        return true;
    }

    uint8_t *data = NULL;
    uint32_t data_size = 0;
    if (!file_chunk_read(chunk, g_file_datadir, &data, &data_size)) {
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
