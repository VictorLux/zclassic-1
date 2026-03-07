/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Load Zcash zkSNARK verification keys from params files. */

#include "zcash/params_init.h"
#include "zcash/bls12_381.h"
#include "zcash/sapling.h"
#include "zcash/sprout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct groth16_vk spend_vk;
static struct groth16_vk output_vk;
static struct groth16_vk sprout_groth16_vk;
static bool params_loaded = false;

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

bool zcash_init_params(const char *params_dir)
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

    params_loaded = true;
    return true;
}

void zcash_free_params(void)
{
    if (!params_loaded) return;
    free(spend_vk.ic);
    free(output_vk.ic);
    free(sprout_groth16_vk.ic);
    memset(&spend_vk, 0, sizeof(spend_vk));
    memset(&output_vk, 0, sizeof(output_vk));
    memset(&sprout_groth16_vk, 0, sizeof(sprout_groth16_vk));
    sapling_set_spend_vk(NULL);
    sapling_set_output_vk(NULL);
    sprout_set_vk(NULL);
    params_loaded = false;
}
