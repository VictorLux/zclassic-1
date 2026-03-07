/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Load Zcash zkSNARK verification keys from params files. */

#ifndef ZCL_ZCASH_PARAMS_INIT_H
#define ZCL_ZCASH_PARAMS_INIT_H

#include <stdbool.h>
#include <stddef.h>

/* Load all Sapling and Sprout Groth16 verification keys.
 * Reads sapling-spend.params, sapling-output.params, and sprout-groth16.params
 * from the given directory path. Sets global VKs for verification.
 * Returns false if any file cannot be read or parsed. */
bool zcash_init_params(const char *params_dir);

/* Free all loaded verification keys. */
void zcash_free_params(void);

#endif
