/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Load Zcash zkSNARK verification keys from params files. */

#include "sapling/params_init.h"
#include "sapling/bls12_381.h"
#include "sapling/bn254.h"
#include "sapling/sapling.h"
#include "sapling/sprout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

    uint8_t *buf = malloc((size_t)sz);
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
    bool ok = groth16_vk_read(&spend_vk, data, len);
    free(data);
    if (!ok) return false;

    /* Sapling output VK */
    snprintf(path, sizeof(path), "%s/sapling-output.params", params_dir);
    data = read_file(path, &len);
    if (!data) { free(spend_vk.ic); return false; }
    ok = groth16_vk_read(&output_vk, data, len);
    free(data);
    if (!ok) { free(spend_vk.ic); return false; }

    /* Sprout Groth16 VK */
    snprintf(path, sizeof(path), "%s/sprout-groth16.params", params_dir);
    data = read_file(path, &len);
    if (!data) { free(spend_vk.ic); free(output_vk.ic); return false; }
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
        if (phgr_data) {
            if (ppzksnark_vk_read(&phgr_vk, phgr_data, len)) {
                sprout_phgr_set_vk(&phgr_vk);
                printf("Loaded Sprout PHGR13 verification key: %zu bytes "
                       "(%zu IC points)\n", len, phgr_vk.ic_len);
            } else {
                fprintf(stderr, "WARNING: Failed to parse sprout-verifying.key\n");
            }
            free(phgr_data);
        }
        /* Non-fatal if missing — PHGR13 proofs just won't be verified */
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
            "8270785a1a0d0bc77196f000ee6d221c9c9894f55307bd9357c3f0105d31ca63991ab91324160d8f53e2bbd3c2633a6eb8bdf5205d822e7f3f73edac51b2b70c",
            (const uint8_t *)output_path2, strlen(output_path2),
            "657e3d38dbb5cb5e7dd2970e8b03d69b4787dd907285b5a7f0790dcc8072f60bf593b32cc2d1c030e00ff5ae64bf84c5c3beb84ddc841d48264b4a171744d028",
            (const uint8_t *)sprout_path, strlen(sprout_path),
            "e9b238411bd6c0ec4791e9d04245ec350c9c5744f5610dfcce4365d5ca49dfefd5054e371842b3f88fa1b9d7e8e075249b3ebabd167fa8b0f3161292d36c180a");
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
