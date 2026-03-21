/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_EVENT_H
#define ZCL_EVENT_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stddef.h>

/* ── Event types ─────────────────────────────────────────────
 * Every observable event in the system. Grouped by subsystem.
 * Adding a new event: add the enum, add the name string in
 * event.c event_type_name(), done. */

enum event_type {
    /* ── Network / TCP ──────────────────────────────── */
    EV_TCP_CONNECT_ATTEMPT = 0,  /* reserved (connected/failed cover this) */
    EV_TCP_CONNECTED,            /* payload: ip[16] + port(u16) */
    EV_TCP_CONNECT_FAILED,       /* payload: ip[16] + port(u16) + errno(i32) */
    EV_TCP_ACCEPTED,             /* payload: ip[16] + port(u16) */
    EV_TCP_DISCONNECTED,         /* payload: reason string */
    EV_TCP_TIMEOUT,              /* payload: seconds(i64) */

    /* ── P2P Messages ───────────────────────────────── */
    EV_MSG_RECEIVED,             /* payload: cmd[12] + size(u32) */
    EV_MSG_SENT,                 /* payload: cmd[12] + size(u32) */
    EV_MSG_CHECKSUM_FAIL,        /* payload: cmd[12] + expected(u32) + got(u32) */
    EV_MSG_DESERIALIZATION_FAIL, /* payload: cmd[12] */

    /* ── Peer state machine ─────────────────────────── */
    EV_PEER_STATE_CHANGE,        /* payload: from(u8) + to(u8) + reason string */
    EV_PEER_MISBEHAVE,           /* payload: score(i32) + total(i32) + reason */
    EV_PEER_BANNED,              /* payload: duration(i64) */
    EV_PEER_VERSION,             /* payload: proto(i32) + height(i32) + subver */

    /* ── Sync state machine ─────────────────────────── */
    EV_SYNC_STATE_CHANGE,        /* payload: from(u8) + to(u8) + reason string */
    EV_HEADERS_RECEIVED,         /* payload: count(u32) + from_h(i32) + to_h(i32) */
    EV_HEADERS_REJECTED,         /* payload: count(u32) + reason string */
    EV_BLOCK_REQUESTED,          /* payload: queued/assigned/timeout string */

    /* ── Validation pipeline ────────────────────────── */
    EV_BLOCK_CONNECTED,          /* payload: height string */
    EV_BLOCK_REJECTED,           /* payload: dos + reason string */

    /* ── Chain ──────────────────────────────────────── */
    EV_TIP_UPDATED,              /* payload: hash[32] + height(i32) */
    EV_REORG_START,              /* payload: fork_height(i32) + new_height(i32) */
    /* EV_REORG_COMPLETE reserved for future use */
    EV_COINS_FLUSH,              /* payload: entries(u64) + blocks_batched(u32) */
    EV_COINS_FLUSH_FAILED,       /* payload: reason string */

    /* ── Transaction ────────────────────────────────── */
    EV_TX_ACCEPTED,              /* payload: txid[32] */
    EV_TX_REJECTED,              /* payload: txid[32] */

    /* ── Fast sync / snapshot ───────────────────────── */
    EV_SNAPSHOT_OFFER_SENT,      /* payload: height(i32) + utxos(u64) */
    EV_SNAPSHOT_OFFER_RECEIVED,  /* payload: height(i32) + utxos(u64) */
    EV_SNAPSHOT_COMPLETE,        /* payload: total_utxos string */

    /* (Wallet and RPC events reserved for future use) */

    /* ── System ─────────────────────────────────────── */
    EV_NODE_STARTING,            /* payload: version string */
    EV_NODE_READY,               /* payload: height(i32) + peers(u32) */
    EV_NODE_SHUTDOWN,            /* payload: reason string */
    EV_CRASH,                    /* payload: signal(i32) */
    EV_DB_ERROR,                 /* payload: operation + errmsg */

    EV_NUM_TYPES                 /* sentinel — must be last */
};

/* ── Peer state machine ─────────────────────────────────── */

enum peer_state {
    PEER_DISCONNECTED = 0,
    PEER_CONNECTING,           /* TCP SYN sent (outbound) */
    PEER_CONNECTED,            /* TCP established, no version yet */
    PEER_VERSION_SENT,         /* we sent our version message */
    PEER_VERSION_RECEIVED,     /* we got their version message */
    PEER_HANDSHAKE_COMPLETE,   /* version+verack exchanged both ways */
    PEER_ACTIVE,               /* fully operational, relay mode */
    PEER_SYNCING_HEADERS,      /* downloading headers from this peer */
    PEER_SYNCING_BLOCKS,       /* downloading blocks from this peer */
    PEER_SNAPSHOT_SERVING,     /* streaming UTXO snapshot to this peer */
    PEER_SNAPSHOT_RECEIVING,   /* receiving UTXO snapshot from this peer */
    PEER_STALE,                /* no useful data for a while */
    PEER_DISCONNECTING,        /* graceful disconnect in progress */
    PEER_BANNED,               /* IP banned */
    PEER_NUM_STATES            /* sentinel */
};

/* ── Sync state machine ─────────────────────────────────── */

enum sync_state {
    SYNC_IDLE = 0,
    SYNC_FINDING_PEERS,
    SYNC_HEADERS_DOWNLOAD,     /* IBD phase 1: accumulating headers */
    SYNC_BLOCKS_DOWNLOAD,      /* IBD phase 2: downloading block data */
    SYNC_CONNECTING_BLOCKS,    /* IBD phase 3: validating + connecting */
    SYNC_AT_TIP,               /* caught up, normal relay */
    SYNC_REORG,                /* processing a chain reorganization */
    SYNC_SNAPSHOT_RECEIVE,     /* fast sync from ZCL23 peer */
    SYNC_FAILED,               /* unrecoverable error */
    SYNC_NUM_STATES            /* sentinel */
};

/* ── Event structure ────────────────────────────────────── */

#define EVENT_PAYLOAD_SIZE 120

struct event {
    _Atomic uint64_t  sequence;      /* monotonic, publish marker */
    int64_t           timestamp_us;  /* microseconds since epoch */
    enum event_type   type;
    uint32_t          peer_id;       /* 0 if not peer-related */
    uint32_t          payload_len;
    uint8_t           payload[EVENT_PAYLOAD_SIZE];
};

/* ── Ring buffer ────────────────────────────────────────── */

#define EVENT_LOG_SIZE 65536         /* must be power of 2 */
#define EVENT_LOG_MASK (EVENT_LOG_SIZE - 1)

struct event_log {
    _Atomic uint64_t  write_pos;
    struct event      ring[EVENT_LOG_SIZE];
    _Atomic bool      initialized;
};

/* ── API ────────────────────────────────────────────────── */

/* Initialize the global event log. Call once at startup. */
void event_log_init(void);

/* Emit an event. Lock-free, O(1). Safe from any thread.
 * peer_id = 0 for non-peer events. payload can be NULL. */
void event_emit(enum event_type type, uint32_t peer_id,
                const void *payload, uint32_t payload_len);

/* Convenience: emit with a format string as payload. */
void event_emitf(enum event_type type, uint32_t peer_id,
                 const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Dump last `count` events to stderr. For crash handlers. */
void event_dump_recent(size_t count);

/* Dump last `count` events as JSON to a buffer.
 * Returns bytes written, or required size if buf is too small. */
size_t event_dump_json(char *buf, size_t buf_size, size_t count);

/* Get event type name as string. */
const char *event_type_name(enum event_type type);

/* Install crash signal handlers (SIGSEGV, SIGABRT, SIGBUS).
 * On crash, dumps last 200 events to stderr before exit. */
void event_install_crash_handler(void);

/* ── Peer state machine API ─────────────────────────────── */

/* Validate and execute a peer state transition.
 * Returns false if the transition is illegal (bug). */
bool peer_set_state_checked(uint32_t peer_id, enum peer_state *current,
                            enum peer_state new_state, const char *reason);

/* Get peer state name as string. */
const char *peer_state_name(enum peer_state state);

/* Check if a transition is legal without executing it. */
bool peer_transition_valid(enum peer_state from, enum peer_state to);

/* ── Sync state machine API ─────────────────────────────── */

/* Global sync state — atomic read/write. */
enum sync_state sync_get_state(void);
bool sync_set_state(enum sync_state new_state, const char *reason);
const char *sync_state_name(enum sync_state state);

#endif /* ZCL_EVENT_H */
