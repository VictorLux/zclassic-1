/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ActiveRecord model: Zmsg (encrypted P2P messages)
 *
 * Wires callbacks + validator for the `zmsg_messages` table.
 * Persistence SQL lives in lib/net/src/zmsg.c (which the controllers
 * and msgprocessor call); this file owns the integrity contract.
 *
 * Record type is `struct zmsg_message` from net/zmsg.h — reused
 * rather than duplicated so wire / runtime / at-rest representations
 * stay byte-aligned. */

#include "models/zmsg.h"
#include <string.h>

DEFINE_MODEL_CALLBACKS(zmsg)

bool db_zmsg_validate(const struct zmsg_message *msg,
                      struct ar_errors *errors)
{
    ar_errors_clear(errors);
    if (!msg) {
        ar_errors_add(errors, "msg", "is NULL");
        return false;
    }

    static const uint8_t zero32[32] = {0};

    validates_custom(errors,
        memcmp(msg->msg_id, zero32, 32) != 0,
        "msg_id", "can't be all zero (pre-compute via zmsg_compute_id)");
    validates_presence_of(errors, msg, sender);
    validates_presence_of(errors, msg, recipient);
    validates_presence_of(errors, msg, body);
    validates_inclusion_of(errors, msg, direction,
        ((int[]){ZMSG_INBOUND, ZMSG_OUTBOUND}), 2);
    validates_inclusion_of(errors, msg, channel,
        ((int[]){ZMSG_CHANNEL_ONCHAIN, ZMSG_CHANNEL_P2P}), 2);
    validates_non_negative(errors, msg, timestamp);
    validates_custom(errors,
        strnlen(msg->body, ZMSG_MAX_BODY + 1) <= ZMSG_MAX_BODY,
        "body", "exceeds ZMSG_MAX_BODY");

    return !ar_errors_any(errors);
}
