/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * SHA3-256 window verifier. The g_sha3_windows table here is a
 * PLACEHOLDER — tools/gen_sha3_windows overwrites this file with the
 * real table after querying a reference node. The verifier below is
 * stable and independent of the table contents. */

#include "chain/sha3_windows.h"

#include "crypto/sha3.h"

#include <string.h>

const struct sha3_window g_sha3_windows[] = {
    { 0, {0} }
};

const size_t g_sha3_windows_count = 0;

bool sha3_windows_verify_window(int window_index,
                                const uint8_t *block_payloads_concat,
                                size_t total_len)
{
    if (window_index < 0)
        return false;
    if ((size_t)window_index >= g_sha3_windows_count)
        return false;
    if (block_payloads_concat == NULL && total_len != 0)
        return false;

    uint8_t digest[32];
    sha3_256(block_payloads_concat, total_len, digest);

    return memcmp(digest, g_sha3_windows[window_index].hash, 32) == 0;
}
