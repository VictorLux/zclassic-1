/* Quick end-to-end test of the SHA3 file service.
 * Connects to localhost:18034, downloads all chunks, measures speed. */

#include "net/file_service.h"
#include "crypto/sha3.h"
#include "core/random.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

int main(void)
{
    printf("=== SHA3 File Service Test ===\n");

    /* Use zero UTXO root (server also uses zero for now) */
    uint8_t utxo_root[32];
    memset(utxo_root, 0, 32);

    const char *datadir = "/tmp/fs_test_output";
    mkdir(datadir, 0755);
    char blocks_dir[256];
    snprintf(blocks_dir, sizeof(blocks_dir), "%s/blocks", datadir);
    mkdir(blocks_dir, 0755);

    int64_t start = (int64_t)time(NULL);

    bool ok = fs_client_sync("127.0.0.1", FS_PORT, datadir, utxo_root);

    int64_t elapsed = (int64_t)time(NULL) - start;
    if (elapsed < 1) elapsed = 1;

    if (ok) {
        /* Check what we got */
        struct stat st;
        char path[512];
        snprintf(path, sizeof(path), "%s/blocks/blk00000.dat", datadir);
        if (stat(path, &st) == 0) {
            double mb = (double)st.st_size / (1024.0 * 1024.0);
            double mbps = mb / (double)elapsed;
            printf("\n=== RESULT ===\n");
            printf("Downloaded: %.1f MB\n", mb);
            printf("Time: %lld seconds\n", (long long)elapsed);
            printf("Speed: %.1f MB/s\n", mbps);
            printf("Status: SUCCESS\n");
        } else {
            printf("No block file created\n");
        }
    } else {
        printf("Status: FAILED\n");
    }

    return ok ? 0 : 1;
}
