/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"

#include "adapters/outbound/persistence/block_log_file.h"
#include "application/operations/shadow_replay_proof.h"
#include "ports/block_log_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SRP_CHECK(name, expr) do {                         \
    printf("shadow_replay_proof: %s... ", (name));         \
    if ((expr)) { printf("OK\n"); }                        \
    else { printf("FAIL\n"); failures++; }                 \
} while (0)

static void srp_tmpdir(char *buf, size_t cap)
{
    snprintf(buf, cap, "/tmp/zcl_srp_XXXXXX");
    if (!mkdtemp(buf)) {
        perror("mkdtemp");
        buf[0] = '\0';
    }
}

static void srp_rm_rf(const char *dir)
{
    if (!dir || !dir[0]) return;
    char cmd[4200];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    (void)!system(cmd);
}

static void srp_append(struct block_log_port *p,
                       uint32_t h,
                       uint8_t seed,
                       const char *bytes)
{
    struct block_hash hash;
    memset(hash.bytes, 0, sizeof(hash.bytes));
    hash.bytes[0] = seed;
    (void)p->append(p->self, h, &hash,
                    (const uint8_t *)bytes, strlen(bytes) + 1);
}

int test_shadow_replay_proof(void)
{
    int failures = 0;

    {
        char primary_dir[64], shadow_dir[64];
        srp_tmpdir(primary_dir, sizeof(primary_dir));
        srp_tmpdir(shadow_dir, sizeof(shadow_dir));
        struct block_log_file *primary_h = NULL, *shadow_h = NULL;
        struct block_log_port primary = {0}, shadow = {0};
        block_log_file_open(primary_dir, &primary_h, &primary);
        block_log_file_open(shadow_dir, &shadow_h, &shadow);

        srp_append(&primary, 0, 0xa0, "block-0");
        srp_append(&primary, 1, 0xa1, "block-1");
        srp_append(&primary, 2, 0xa2, "block-2");

        struct shadow_replay_proof_inputs in = {
            .primary = &primary,
            .shadow = &shadow,
            .start_height = 0,
            .end_height = UINT32_MAX,
        };
        struct shadow_replay_proof_report rep;
        struct zcl_result r = shadow_replay_proof_run(&in, &rep);
        SRP_CHECK("replay converges",
                  r.ok && rep.proof_ok &&
                  rep.status == SHADOW_REPLAY_PROOF_OK);
        SRP_CHECK("fed count equals diffed count",
                  rep.blocks_fed == 3 && rep.blocks_diffed == 3);
        SRP_CHECK("shadow tip after replay",
                  rep.shadow_tip_after == 2);

        block_log_file_close(primary_h);
        block_log_file_close(shadow_h);
        srp_rm_rf(primary_dir);
        srp_rm_rf(shadow_dir);
    }

    {
        char primary_dir[64], shadow_dir[64];
        srp_tmpdir(primary_dir, sizeof(primary_dir));
        srp_tmpdir(shadow_dir, sizeof(shadow_dir));
        struct block_log_file *primary_h = NULL, *shadow_h = NULL;
        struct block_log_port primary = {0}, shadow = {0};
        block_log_file_open(primary_dir, &primary_h, &primary);
        block_log_file_open(shadow_dir, &shadow_h, &shadow);

        srp_append(&primary, 0, 0xb0, "block-0");
        srp_append(&shadow, 0, 0xc0, "old-shadow-block");

        struct shadow_replay_proof_inputs in = {
            .primary = &primary,
            .shadow = &shadow,
            .start_height = 0,
            .end_height = UINT32_MAX,
        };
        struct shadow_replay_proof_report rep;
        struct zcl_result r = shadow_replay_proof_run(&in, &rep);
        SRP_CHECK("refuses to overwrite proof range",
                  r.ok && !rep.proof_ok &&
                  rep.status == SHADOW_REPLAY_PROOF_SHADOW_NOT_EMPTY);
        SRP_CHECK("refusal feeds nothing", rep.blocks_fed == 0);

        block_log_file_close(primary_h);
        block_log_file_close(shadow_h);
        srp_rm_rf(primary_dir);
        srp_rm_rf(shadow_dir);
    }

    {
        char primary_dir[64], shadow_dir[64];
        srp_tmpdir(primary_dir, sizeof(primary_dir));
        srp_tmpdir(shadow_dir, sizeof(shadow_dir));
        struct block_log_file *primary_h = NULL, *shadow_h = NULL;
        struct block_log_port primary = {0}, shadow = {0};
        block_log_file_open(primary_dir, &primary_h, &primary);
        block_log_file_open(shadow_dir, &shadow_h, &shadow);

        struct shadow_replay_proof_inputs in = {
            .primary = &primary,
            .shadow = &shadow,
            .start_height = 0,
            .end_height = UINT32_MAX,
        };
        struct shadow_replay_proof_report rep;
        struct zcl_result r = shadow_replay_proof_run(&in, &rep);
        SRP_CHECK("empty primary is not a proof",
                  r.ok && !rep.proof_ok &&
                  rep.status == SHADOW_REPLAY_PROOF_EMPTY);

        block_log_file_close(primary_h);
        block_log_file_close(shadow_h);
        srp_rm_rf(primary_dir);
        srp_rm_rf(shadow_dir);
    }

    return failures;
}
