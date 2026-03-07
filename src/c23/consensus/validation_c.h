/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_CONSENSUS_VALIDATION_C_H
#define ZCL_CONSENSUS_VALIDATION_C_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define REJECT_MALFORMED        0x01
#define REJECT_INVALID          0x10
#define REJECT_OBSOLETE         0x11
#define REJECT_DUPLICATE        0x12
#define REJECT_NONSTANDARD      0x40
#define REJECT_DUST             0x41
#define REJECT_INSUFFICIENTFEE  0x42
#define REJECT_CHECKPOINT       0x43

enum validation_mode {
    MODE_VALID,
    MODE_INVALID,
    MODE_ERROR,
};

#define MAX_REJECT_REASON 256

struct validation_state {
    enum validation_mode mode;
    int dos;
    char reject_reason[MAX_REJECT_REASON];
    unsigned int reject_code;
    bool corruption_possible;
    char debug_message[MAX_REJECT_REASON];
};

static inline void validation_state_init(struct validation_state *s)
{
    s->mode = MODE_VALID;
    s->dos = 0;
    s->reject_code = 0;
    s->corruption_possible = false;
    s->reject_reason[0] = '\0';
    s->debug_message[0] = '\0';
}

static inline bool validation_state_dos(struct validation_state *s,
                                        int level, bool ret,
                                        unsigned int code,
                                        const char *reason,
                                        bool corruption,
                                        const char *debug)
{
    s->reject_code = code;
    if (reason) {
        strncpy(s->reject_reason, reason, MAX_REJECT_REASON - 1);
        s->reject_reason[MAX_REJECT_REASON - 1] = '\0';
    }
    s->corruption_possible = corruption;
    if (debug) {
        strncpy(s->debug_message, debug, MAX_REJECT_REASON - 1);
        s->debug_message[MAX_REJECT_REASON - 1] = '\0';
    }
    if (s->mode == MODE_ERROR)
        return ret;
    s->dos += level;
    s->mode = MODE_INVALID;
    return ret;
}

static inline bool validation_state_invalid(struct validation_state *s,
                                            bool ret, unsigned char code,
                                            const char *reason,
                                            const char *debug)
{
    return validation_state_dos(s, 0, ret, code, reason, false, debug);
}

static inline bool validation_state_error(struct validation_state *s,
                                          const char *reason)
{
    if (s->mode == MODE_VALID && reason) {
        strncpy(s->reject_reason, reason, MAX_REJECT_REASON - 1);
        s->reject_reason[MAX_REJECT_REASON - 1] = '\0';
    }
    s->mode = MODE_ERROR;
    return false;
}

static inline bool validation_state_is_valid(const struct validation_state *s)
{
    return s->mode == MODE_VALID;
}

static inline bool validation_state_is_invalid(const struct validation_state *s)
{
    return s->mode == MODE_INVALID;
}

static inline bool validation_state_is_error(const struct validation_state *s)
{
    return s->mode == MODE_ERROR;
}

static inline bool validation_state_get_dos(const struct validation_state *s,
                                            int *dos_out)
{
    if (s->mode == MODE_INVALID) {
        *dos_out = s->dos;
        return true;
    }
    return false;
}

#endif
