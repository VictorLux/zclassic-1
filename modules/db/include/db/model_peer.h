/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_DB_MODEL_PEER_H
#define ZCL_DB_MODEL_PEER_H

#include "db/db.h"
#include "db/activerecord.h"
#include <stdbool.h>
#include <stdint.h>

struct db_peer {
    int64_t id;
    uint8_t ip[16];
    uint16_t port;
    uint64_t services;
    int64_t last_seen;
    int64_t last_try;
    int attempts;
    uint8_t source[16];
    bool has_source;
};

/* Callbacks and validation */
struct ar_callbacks *db_peer_callbacks(void);
bool db_peer_validate(const struct db_peer *p, struct ar_errors *errors);

bool db_peer_save(struct node_db *ndb, const struct db_peer *p);
bool db_peer_find_by_addr(struct node_db *ndb,
                          const uint8_t ip[16], uint16_t port,
                          struct db_peer *out);
bool db_peer_delete(struct node_db *ndb, const uint8_t ip[16], uint16_t port);
int db_peer_count(struct node_db *ndb);

/* Get recently-seen peers for addr relay. Returns count. */
int db_peer_recent(struct node_db *ndb, struct db_peer *out, size_t max);

/* Get peers to try connecting to (ordered by last_try ASC). */
int db_peer_to_try(struct node_db *ndb, struct db_peer *out, size_t max);

/* Update last_try and increment attempts for a peer. */
bool db_peer_mark_tried(struct node_db *ndb,
                        const uint8_t ip[16], uint16_t port);

/* Update last_seen on successful connection. */
bool db_peer_mark_seen(struct node_db *ndb,
                       const uint8_t ip[16], uint16_t port,
                       int64_t now);

#endif
