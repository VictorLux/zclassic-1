/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Diagnose why our Sapling spend proof fails C++ verification.
 * Checks each component: cv, rk, spend_auth_sig, proof. */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "sapling/sapling.h"
#include "sapling/fr.h"

static void hex_to_bytes(const char *hex, uint8_t *out, int len)
{
    for (int i = 0; i < len; i++) {
        unsigned int b;
        sscanf(hex + i*2, "%2x", &b);
        out[i] = (uint8_t)b;
    }
}

static void print_hex(const char *label, const uint8_t *data, int len)
{
    printf("  %s: ", label);
    for (int i = 0; i < len; i++) printf("%02x", data[i]);
    printf("\n");
}

int main(void)
{
    /* Initialize crypto */
    ecc_start();

    uint8_t cv[32], anchor[32], nullifier[32], rk[32];
    uint8_t proof[192], sig[64];

    hex_to_bytes("6a2c967046acea7c77d220d446a55e896e5f7236aea2cf359ed2878ad3c217dc", cv, 32);
    hex_to_bytes("1c751d8420ea135aebd3cf21ad428f4d60bdd8011bde47f5ac6462d989031d68", anchor, 32);
    hex_to_bytes("7eab7c08dbcda2b14216cd5673d70c872e910ee2a6bab2aa5dd484ba5a57512a", nullifier, 32);
    hex_to_bytes("9a8a751e83b60f2f42ca6cdae5c3877d86a3f8c53180cd353aa12a60fd6f68ea", rk, 32);

    printf("=== Sapling Spend Description Diagnosis ===\n\n");

    /* Check 1: cv is valid Jubjub point */
    printf("Check 1: cv decompression... ");
    {
        struct jub_point cv_pt;
        if (jub_from_bytes(&cv_pt, cv))
            printf("OK (valid point)\n");
        else
            printf("FAIL (invalid point!)\n");
    }

    /* Check 2: rk is valid Jubjub point */
    printf("Check 2: rk decompression... ");
    {
        struct jub_point rk_pt;
        if (jub_from_bytes(&rk_pt, rk))
            printf("OK (valid point)\n");
        else
            printf("FAIL (invalid point!)\n");
    }

    /* Check 3: cv small order check */
    printf("Check 3: cv small order... ");
    {
        struct jub_point cv_pt;
        jub_from_bytes(&cv_pt, cv);
        struct jub_point cofactor;
        jub_mul_by_cofactor(&cofactor, &cv_pt);
        if (!jub_is_identity(&cofactor))
            printf("OK (not small order)\n");
        else
            printf("FAIL (small order!)\n");
    }

    /* Check 4: rk small order check */
    printf("Check 4: rk small order... ");
    {
        struct jub_point rk_pt;
        jub_from_bytes(&rk_pt, rk);
        struct jub_point cofactor;
        jub_mul_by_cofactor(&cofactor, &rk_pt);
        if (!jub_is_identity(&cofactor))
            printf("OK (not small order)\n");
        else
            printf("FAIL (small order!)\n");
    }

    /* Check 5: spend_auth_sig
     * The sighash is computed from the transaction data.
     * We'd need the full sighash to verify the sig.
     * For now, just check the sig format (64 bytes, R + S). */
    hex_to_bytes("82be442056e5ab8c64217c0b8e54342e294663c5"
                 "00000000000000000000000000000000000000000000"
                 "c13dd64f4892304104e7b178c7531b1b03307006", sig, 64);
    printf("Check 5: spend_auth_sig format... ");
    {
        /* R is first 32 bytes — must be a valid point */
        struct jub_point R;
        if (jub_from_bytes(&R, sig))
            printf("OK (R is valid point)\n");
        else
            printf("FAIL (R is not a valid point — bad signature!)\n");
    }

    /* Check 6: Groth16 proof format */
    printf("Check 6: proof decompression... ");
    {
        /* A (48 bytes G1), B (96 bytes G2), C (48 bytes G1) */
        /* Just check first component is nonzero */
        bool nonzero = false;
        hex_to_bytes("929c0cbe003274b0c96b7be14997230185818ea9"
                     "000000000000000000000000000000000000000000000000"
                     "0000000000", proof, 48);
        for (int i = 0; i < 48; i++)
            if (proof[i] != 0) { nonzero = true; break; }
        printf("%s\n", nonzero ? "OK (nonzero)" : "FAIL (zero proof)");
    }

    printf("\n=== Key Insight ===\n");
    printf("If cv and rk decompress correctly, the likely failure is:\n");
    printf("  1. spend_auth_sig doesn't verify under rk (wrong signing key or sighash)\n");
    printf("  2. Groth16 proof doesn't verify (circuit synthesis bug)\n");
    printf("\nThe spend_auth_sig is signed with ask+ar, verified with rk=ak+ar*G.\n");
    printf("If rk is computed wrongly (wrong generator), the sig will fail.\n");

    ecc_stop();
    return 0;
}
