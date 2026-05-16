/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ActiveRecord model: FileOffer (ZCL Market gossip)
 *
 * Wires callbacks + validator for the `file_offers` table. Persistence
 * SQL still lives in lib/net/src/file_market.c (which the controllers
 * and gossip layer call); this file owns the integrity contract those
 * writes go through.
 *
 * The record type is `struct file_offer` from net/file_market.h —
 * deliberately reused (rather than a parallel `struct db_file_offer`)
 * so the gossip layer and persistence layer agree byte-for-byte on
 * the on-the-wire / at-rest representation. */

#include "models/file_offer.h"
#include <string.h>

DEFINE_MODEL_CALLBACKS(file_offer)

bool db_file_offer_validate(const struct file_offer *offer,
                            struct ar_errors *errors)
{
    ar_errors_clear(errors);
    if (!offer) {
        ar_errors_add(errors, "offer", "is NULL");
        return false;
    }

    static const uint8_t zero32[32] = {0};
    static const uint8_t zero43[43] = {0};

    validates_custom(errors,
        memcmp(offer->root_hash, zero32, 32) != 0,
        "root_hash", "can't be all zero");
    validates_presence_of(errors, offer, filename);
    validates_positive(errors, offer, size_bytes);
    validates_positive(errors, offer, num_chunks);
    validates_non_negative(errors, offer, price_per_mb);
    validates_custom(errors,
        memcmp(offer->z_addr, zero43, 43) != 0,
        "z_addr", "can't be all zero");
    validates_not_zero(errors, offer, peer_port);
    validates_non_negative(errors, offer, last_seen);
    validates_range(errors, offer, ttl, 1, FILE_MARKET_MAX_TTL);

    return !ar_errors_any(errors);
}
