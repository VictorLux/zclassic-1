/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Load Zcash zkSNARK verification keys from params files. */

#include "sapling/params_init.h"
#include "sapling/bls12_381.h"
#include "sapling/bn254.h"
#include "sapling/sapling.h"
#include "sapling/sprout.h"
#include "chain/chainparams.h"
#include "crypto/sha512.h"
#include "util/log_macros.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util/safe_alloc.h"

/* Expected SHA-512 digests of the full Zcash parameter files, as produced
 * by Zcash's canonical fetch-params.sh distribution (MPC-ceremony output,
 * deterministic across downloads — every correctly-fetched file has these
 * hashes). Baked in here so the integrity check cannot be disabled at
 * runtime; if they fail to match, the param files on disk were tampered
 * with and the node refuses to start rather than feed unknown params into
 * Groth16/PHGR13 verification. */
#define SAPLING_SPEND_PARAMS_SHA512                                          \
    "320fbb5754c8b3f4ecb3dd4c2e3dbe1d138c4b343f073c7e31612d26fe769cdb"       \
    "a8edab426a7f2c42e317bebdb2ca6f73af1312affa1fcf599a31d499ad4cb4e5"
#define SAPLING_OUTPUT_PARAMS_SHA512                                         \
    "a7c87f52c0c1eb05b4da1e1e70d986bde95a2a74047b0679d0163ade7ced3cbf"       \
    "93c1c7e3954416e5baacff614e1d0d5dfaed7b9b7d3141fa595259b23c32f152"
#define SPROUT_GROTH16_PARAMS_SHA512                                         \
    "20bc1f6bd89d0321b90a3f1b7e2050a7dafb427e86e7ef33b0b7a5c06077f5bf"       \
    "5695846952eac2b6231222df633e258682e9b6e2545f732c30fd76ae230ac65d"

/* Parse a 128-char lowercase hex string into 64 bytes. Returns false on
 * any malformed input. Only used at startup on static strings, so no
 * constant-time requirement. */
static bool hex_to_bytes64(const char *hex, uint8_t out[64])
{
    for (size_t i = 0; i < 64; i++) {
        int hi = hex[2 * i];
        int lo = hex[2 * i + 1];
        int hv = (hi >= '0' && hi <= '9') ? hi - '0'
               : (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10
               : (hi >= 'A' && hi <= 'F') ? hi - 'A' + 10 : -1;
        int lv = (lo >= '0' && lo <= '9') ? lo - '0'
               : (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10
               : (lo >= 'A' && lo <= 'F') ? lo - 'A' + 10 : -1;
        if (hv < 0 || lv < 0) return false;
        out[i] = (uint8_t)((hv << 4) | lv);
    }
    return true;
}

/* Compute SHA-512 of a buffer and compare against the expected hex digest
 * in constant time. On mismatch, print expected/actual and return false so
 * startup fails loud — parameter-file tampering is consensus-critical. */
static bool params_sha512_matches(const uint8_t *data, size_t len,
                                   const char *expected_hex,
                                   const char *path)
{
    uint8_t got[64];
    struct sha512_ctx ctx;
    sha512_init(&ctx);
    sha512_write(&ctx, data, len);
    sha512_finalize(&ctx, got);

    uint8_t want[64];
    if (!hex_to_bytes64(expected_hex, want)) {
        fprintf(stderr, "[sapling] %s:%d %s(): "
                "internal: malformed expected SHA-512 literal for %s\n",
                __FILE__, __LINE__, __func__, path);
        return false;
    }

    /* Constant-time comparison — not strictly needed here (no timing
     * oracle on param files) but cheap insurance. */
    uint32_t diff = 0;
    for (int i = 0; i < 64; i++) diff |= (uint32_t)(got[i] ^ want[i]);
    if (diff != 0) {
        char got_hex[129], want_hex[129];
        for (int i = 0; i < 64; i++) {
            snprintf(got_hex + 2 * i, 3, "%02x", got[i]);
            snprintf(want_hex + 2 * i, 3, "%02x", want[i]);
        }
        fprintf(stderr, "[sapling] %s:%d %s(): "
                "params file SHA-512 mismatch: path=%s\n"
                "  expected=%s\n  actual  =%s\n",
                __FILE__, __LINE__, __func__, path, want_hex, got_hex);
        return false;
    }
    return true;
}

static struct groth16_vk spend_vk;
static struct groth16_vk output_vk;
static struct groth16_vk sprout_groth16_vk;
static bool params_loaded = false;

static uint8_t *spend_pk_data = NULL;
static size_t spend_pk_len = 0;
static uint8_t *output_pk_data = NULL;
static size_t output_pk_len = 0;

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);

    uint8_t *buf = zcl_malloc((size_t)sz, "params_file_buf");
    if (!buf) { fclose(f); return NULL; }

    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);

    if (rd != (size_t)sz) { free(buf); return NULL; }
    *len = (size_t)sz;
    return buf;
}

bool sapling_init_params(const char *params_dir)
{
    if (params_loaded) return true;

    char path[1024];
    size_t len;
    uint8_t *data;

    /* Sapling spend VK */
    snprintf(path, sizeof(path), "%s/sapling-spend.params", params_dir);
    data = read_file(path, &len);
    if (!data) return false;
    if (!params_sha512_matches(data, len, SAPLING_SPEND_PARAMS_SHA512, path)) {
        free(data);
        return false;
    }
    bool ok = groth16_vk_read(&spend_vk, data, len);
    free(data);
    if (!ok) return false;

    /* Sapling output VK */
    snprintf(path, sizeof(path), "%s/sapling-output.params", params_dir);
    data = read_file(path, &len);
    if (!data) { free(spend_vk.ic); return false; }
    if (!params_sha512_matches(data, len, SAPLING_OUTPUT_PARAMS_SHA512, path)) {
        free(data); free(spend_vk.ic); return false;
    }
    ok = groth16_vk_read(&output_vk, data, len);
    free(data);
    if (!ok) { free(spend_vk.ic); return false; }

    /* Sprout Groth16 VK */
    snprintf(path, sizeof(path), "%s/sprout-groth16.params", params_dir);
    data = read_file(path, &len);
    if (!data) { free(spend_vk.ic); free(output_vk.ic); return false; }
    if (!params_sha512_matches(data, len, SPROUT_GROTH16_PARAMS_SHA512, path)) {
        free(data); free(spend_vk.ic); free(output_vk.ic); return false;
    }
    ok = groth16_vk_read(&sprout_groth16_vk, data, len);
    free(data);
    if (!ok) { free(spend_vk.ic); free(output_vk.ic); return false; }

    sapling_set_spend_vk(&spend_vk);
    sapling_set_output_vk(&output_vk);
    sprout_set_vk(&sprout_groth16_vk);

    /* Sprout PHGR13 VK (pre-Sapling proofs, blocks 0-581876) */
    {
        static struct ppzksnark_vk phgr_vk;
        char phgr_path[1024];
        snprintf(phgr_path, sizeof(phgr_path),
                 "%s/sprout-verifying.key", params_dir);
        uint8_t *phgr_data = read_file(phgr_path, &len);
        bool phgr_ok = false;
        if (phgr_data) {
            if (ppzksnark_vk_read(&phgr_vk, phgr_data, len)) {
                sprout_phgr_set_vk(&phgr_vk);
                printf("Loaded Sprout PHGR13 verification key: %zu bytes "
                       "(%zu IC points)\n", len, phgr_vk.ic_len);
                phgr_ok = true;
            } else {
                fprintf(stderr,
                    "ERROR: Failed to parse sprout-verifying.key\n");
            }
            free(phgr_data);
        } else {
            fprintf(stderr,
                "ERROR: sprout-verifying.key not found at %s\n", phgr_path);
        }

        /* Hard-fail on mainnet — PHGR13 proofs are consensus-critical for
         * pre-Sapling blocks. The silent non-fatal path let this bug survive
         * for months (see PHGR13_INVESTIGATION.md). */
        if (!phgr_ok) {
            const struct chain_params *cp = chain_params_get();
            if (cp && strcmp(cp->strNetworkID, "main") == 0) {
                fprintf(stderr,
                    "FATAL: Sprout PHGR13 verification key failed to load.\n"
                    "Mainnet requires this key to validate pre-Sapling blocks.\n"
                    "Ensure sprout-verifying.key exists in: %s\n", params_dir);
                free(spend_vk.ic); free(output_vk.ic);
                free(sprout_groth16_vk.ic);
                return false;
            }
            fprintf(stderr,
                "WARNING: PHGR13 VK not loaded (non-mainnet, continuing)\n");
        }
    }

    /* Keep raw PK data for proving (VK is a subset of PK data) */
    snprintf(path, sizeof(path), "%s/sapling-spend.params", params_dir);
    spend_pk_data = read_file(path, &spend_pk_len);

    snprintf(path, sizeof(path), "%s/sapling-output.params", params_dir);
    output_pk_data = read_file(path, &output_pk_len);

    if (output_pk_data)
        printf("Loaded sapling-output proving key: %zu bytes\n", output_pk_len);
    if (spend_pk_data)
        printf("Loaded sapling-spend proving key: %zu bytes\n", spend_pk_len);

    /* Initialize native C23 prover with params paths for Groth16 proving */
    {
        extern void zclassic_init_zksnark_params(
            const uint8_t *spend_path, size_t spend_path_len,
            const char *spend_hash,
            const uint8_t *output_path, size_t output_path_len,
            const char *output_hash,
            const uint8_t *sprout_path, size_t sprout_path_len,
            const char *sprout_hash);

        char spend_path[1024], output_path2[1024], sprout_path[1024];
        snprintf(spend_path, sizeof(spend_path),
                 "%s/sapling-spend.params", params_dir);
        snprintf(output_path2, sizeof(output_path2),
                 "%s/sapling-output.params", params_dir);
        snprintf(sprout_path, sizeof(sprout_path),
                 "%s/sprout-groth16.params", params_dir);

        zclassic_init_zksnark_params(
            (const uint8_t *)spend_path, strlen(spend_path),
            SAPLING_SPEND_PARAMS_SHA512,
            (const uint8_t *)output_path2, strlen(output_path2),
            SAPLING_OUTPUT_PARAMS_SHA512,
            (const uint8_t *)sprout_path, strlen(sprout_path),
            SPROUT_GROTH16_PARAMS_SHA512);
        printf("native C23 prover zkSNARK params initialized.\n");
    }

    params_loaded = true;
    return true;
}

const uint8_t *sapling_get_output_pk(size_t *len)
{
    if (len) *len = output_pk_len;
    return output_pk_data;
}

const uint8_t *sapling_get_spend_pk(size_t *len)
{
    if (len) *len = spend_pk_len;
    return spend_pk_data;
}

void sapling_free_params(void)
{
    if (!params_loaded) return;
    free(spend_vk.ic);
    free(output_vk.ic);
    free(sprout_groth16_vk.ic);
    free(spend_pk_data);
    free(output_pk_data);
    memset(&spend_vk, 0, sizeof(spend_vk));
    memset(&output_vk, 0, sizeof(output_vk));
    memset(&sprout_groth16_vk, 0, sizeof(sprout_groth16_vk));
    spend_pk_data = NULL; spend_pk_len = 0;
    output_pk_data = NULL; output_pk_len = 0;
    sapling_set_spend_vk(NULL);
    sapling_set_output_vk(NULL);
    sprout_set_vk(NULL);
    params_loaded = false;
}
