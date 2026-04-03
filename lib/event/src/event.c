/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "event/event.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <signal.h>
#include <unistd.h>
#include <execinfo.h>
#include <sys/time.h>

/* ── Global event log ─────────────────────────────────────
 * Single instance. Lock-free ring buffer. Every thread writes
 * here via atomic fetch-add on write_pos. Readers use the
 * sequence field as a publish marker. */

static struct event_log g_log;
static struct error_ring g_error_ring;  /* forward decl for event_log_init */

/* ── Event observers ─────────────────────────────────────── */

struct event_observer_entry {
    event_observer_fn fn;
    void *ctx;
};

static struct {
    struct event_observer_entry observers[EVENT_MAX_OBSERVERS];
    int count;
} g_observers[EV_NUM_TYPES];

bool event_observe(enum event_type type, event_observer_fn fn, void *ctx)
{
    if ((int)type < 0 || type >= EV_NUM_TYPES) return false;
    if (g_observers[type].count >= EVENT_MAX_OBSERVERS) return false;
    int idx = g_observers[type].count++;
    g_observers[type].observers[idx].fn = fn;
    g_observers[type].observers[idx].ctx = ctx;
    return true;
}

void event_clear_observers(enum event_type type)
{
    if ((int)type >= 0 && type < EV_NUM_TYPES)
        g_observers[type].count = 0;
}

void event_clear_all_observers(void)
{
    for (int i = 0; i < EV_NUM_TYPES; i++)
        g_observers[i].count = 0;
}

/* ── Async observer dispatch ─────────────────────────────── */

#include <pthread.h>

/* Async queue entry — snapshot of event data for deferred dispatch */
struct async_event {
    enum event_type type;
    uint32_t peer_id;
    uint8_t  payload[EVENT_PAYLOAD_SIZE];
    uint32_t payload_len;
};

#define ASYNC_QUEUE_SIZE 4096
#define ASYNC_QUEUE_MASK (ASYNC_QUEUE_SIZE - 1)

static struct {
    struct event_observer_entry observers[EVENT_MAX_OBSERVERS];
    int count;
} g_async_observers[EV_NUM_TYPES];

static struct {
    struct async_event ring[ASYNC_QUEUE_SIZE];
    _Atomic uint64_t   write_pos;
    _Atomic uint64_t   read_pos;
    pthread_t          thread;
    _Atomic bool       running;
    pthread_mutex_t    wake_mutex;
    pthread_cond_t     wake_cond;
} g_async;

bool event_observe_async(enum event_type type, event_observer_fn fn, void *ctx)
{
    if ((int)type < 0 || type >= EV_NUM_TYPES) return false;
    if (g_async_observers[type].count >= EVENT_MAX_OBSERVERS) return false;
    int idx = g_async_observers[type].count++;
    g_async_observers[type].observers[idx].fn = fn;
    g_async_observers[type].observers[idx].ctx = ctx;
    return true;
}

static void notify_async_observers(const struct async_event *ae)
{
    enum event_type t = ae->type;
    if ((int)t < 0 || t >= EV_NUM_TYPES) return;
    for (int i = 0; i < g_async_observers[t].count; i++)
        g_async_observers[t].observers[i].fn(
            t, ae->peer_id, ae->payload, ae->payload_len,
            g_async_observers[t].observers[i].ctx);
}

/* Enqueue event for async dispatch. Lock-free, O(1). */
static void async_enqueue(enum event_type type, uint32_t peer_id,
                          const void *payload, uint32_t payload_len)
{
    /* Check if any async observers exist for this type */
    if ((int)type < 0 || type >= EV_NUM_TYPES) return;
    if (g_async_observers[type].count == 0) return;

    uint64_t wp = atomic_fetch_add(&g_async.write_pos, 1);
    struct async_event *ae = &g_async.ring[wp & ASYNC_QUEUE_MASK];
    ae->type = type;
    ae->peer_id = peer_id;
    ae->payload_len = payload_len < EVENT_PAYLOAD_SIZE
                      ? payload_len : EVENT_PAYLOAD_SIZE;
    if (payload && ae->payload_len > 0)
        memcpy(ae->payload, payload, ae->payload_len);
    else
        ae->payload_len = 0;

    /* Wake the dispatch thread */
    pthread_mutex_lock(&g_async.wake_mutex);
    pthread_cond_signal(&g_async.wake_cond);
    pthread_mutex_unlock(&g_async.wake_mutex);
}

static void *async_dispatch_thread(void *arg)
{
    (void)arg;
    while (atomic_load(&g_async.running)) {
        uint64_t rp = atomic_load(&g_async.read_pos);
        uint64_t wp = atomic_load(&g_async.write_pos);

        if (rp >= wp) {
            /* Nothing to process — wait for signal */
            pthread_mutex_lock(&g_async.wake_mutex);
            if (atomic_load(&g_async.read_pos) >= atomic_load(&g_async.write_pos))
                pthread_cond_wait(&g_async.wake_cond, &g_async.wake_mutex);
            pthread_mutex_unlock(&g_async.wake_mutex);
            continue;
        }

        /* Process all pending events */
        while (rp < wp) {
            struct async_event *ae = &g_async.ring[rp & ASYNC_QUEUE_MASK];
            notify_async_observers(ae);
            rp++;
        }
        atomic_store(&g_async.read_pos, rp);
    }

    /* Drain remaining events on shutdown */
    uint64_t rp = atomic_load(&g_async.read_pos);
    uint64_t wp = atomic_load(&g_async.write_pos);
    while (rp < wp) {
        struct async_event *ae = &g_async.ring[rp & ASYNC_QUEUE_MASK];
        notify_async_observers(ae);
        rp++;
    }
    atomic_store(&g_async.read_pos, rp);

    return NULL;
}

void event_async_start(void)
{
    atomic_store(&g_async.write_pos, 0);
    atomic_store(&g_async.read_pos, 0);
    pthread_mutex_init(&g_async.wake_mutex, NULL);
    pthread_cond_init(&g_async.wake_cond, NULL);
    atomic_store(&g_async.running, true);
    pthread_create(&g_async.thread, NULL, async_dispatch_thread, NULL);
}

void event_async_stop(void)
{
    atomic_store(&g_async.running, false);
    pthread_mutex_lock(&g_async.wake_mutex);
    pthread_cond_signal(&g_async.wake_cond);
    pthread_mutex_unlock(&g_async.wake_mutex);
    pthread_join(g_async.thread, NULL);
    pthread_mutex_destroy(&g_async.wake_mutex);
    pthread_cond_destroy(&g_async.wake_cond);
}

static void notify_observers(enum event_type type, uint32_t peer_id,
                              const void *payload, uint32_t payload_len)
{
    if ((int)type < 0 || type >= EV_NUM_TYPES) return;
    for (int i = 0; i < g_observers[type].count; i++)
        g_observers[type].observers[i].fn(type, peer_id, payload, payload_len,
                                           g_observers[type].observers[i].ctx);
}

void event_log_init(void)
{
    memset(&g_log, 0, sizeof(g_log));
    atomic_store(&g_log.write_pos, 0);
    atomic_store(&g_log.initialized, true);
    event_clear_all_observers();
    error_ring_init(&g_error_ring);
}

static int64_t now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

void event_emit(enum event_type type, uint32_t peer_id,
                const void *payload, uint32_t payload_len)
{
    if (!atomic_load(&g_log.initialized))
        return;

    uint64_t seq = atomic_fetch_add(&g_log.write_pos, 1);
    struct event *ev = &g_log.ring[seq & EVENT_LOG_MASK];

    /* Write fields (sequence written last as publish barrier) */
    ev->timestamp_us = now_us();
    ev->type = type;
    ev->peer_id = peer_id;
    ev->payload_len = payload_len < EVENT_PAYLOAD_SIZE
                      ? payload_len : EVENT_PAYLOAD_SIZE;
    if (payload && ev->payload_len > 0)
        memcpy(ev->payload, payload, ev->payload_len);
    else
        ev->payload_len = 0;

    /* Publish: readers check sequence matches expected slot */
    atomic_store_explicit(&ev->sequence, seq + 1, memory_order_release);

    /* Notify sync observers immediately */
    notify_observers(type, peer_id, payload, payload_len);

    /* Enqueue for async observers (non-blocking) */
    if (atomic_load(&g_async.running))
        async_enqueue(type, peer_id, payload, payload_len);
}

void event_emitf(enum event_type type, uint32_t peer_id,
                 const char *fmt, ...)
{
    char buf[EVENT_PAYLOAD_SIZE];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    event_emit(type, peer_id, buf, (uint32_t)n);
}

/* ── Event type names ────────────────────────────────────── */

const char *event_type_name(enum event_type type)
{
    static const char *names[] = {
        [EV_TCP_CONNECT_ATTEMPT]     = "tcp.connect_attempt",
        [EV_TCP_CONNECTED]           = "tcp.connected",
        [EV_TCP_CONNECT_FAILED]      = "tcp.connect_failed",
        [EV_TCP_ACCEPTED]            = "tcp.accepted",
        [EV_TCP_DISCONNECTED]        = "tcp.disconnected",
        [EV_TCP_TIMEOUT]             = "tcp.timeout",
        [EV_MSG_RECEIVED]            = "msg.received",
        [EV_MSG_SENT]                = "msg.sent",
        [EV_MSG_CHECKSUM_FAIL]       = "msg.checksum_fail",
        [EV_MSG_DESERIALIZATION_FAIL]= "msg.deser_fail",
        [EV_PEER_STATE_CHANGE]       = "peer.state_change",
        [EV_PEER_MISBEHAVE]          = "peer.misbehave",
        [EV_PEER_BANNED]             = "peer.banned",
        [EV_PEER_VERSION]            = "peer.version",
        [EV_SYNC_STATE_CHANGE]       = "sync.state_change",
        [EV_HEADERS_RECEIVED]        = "sync.headers_received",
        [EV_HEADERS_REJECTED]        = "sync.headers_rejected",
        [EV_BLOCK_REQUESTED]         = "sync.block_requested",
        [EV_BLOCK_CONNECTED]         = "val.block_connected",
        [EV_BLOCK_REJECTED]          = "val.block_rejected",
        [EV_TIP_UPDATED]             = "chain.tip_updated",
        [EV_REORG_START]             = "chain.reorg_start",
        [EV_REORG_DISCONNECT_FAILED] = "chain.reorg_disconnect_failed",
        [EV_REORG_RECOVERY_COMPLETE] = "chain.reorg_recovery_complete",
        [EV_COINS_FLUSH]             = "chain.coins_flush",
        [EV_COINS_FLUSH_FAILED]      = "chain.coins_flush_fail",
        [EV_TX_ACCEPTED]             = "tx.accepted",
        [EV_TX_REJECTED]             = "tx.rejected",
        [EV_SNAPSHOT_OFFER_SENT]     = "snap.offer_sent",
        [EV_SNAPSHOT_OFFER_RECEIVED] = "snap.offer_received",
        [EV_SNAPSHOT_COMPLETE]       = "snap.complete",
        /* Boot phases */
        [EV_BOOT_PHASE]              = "boot.phase",
        [EV_BOOT_DB_OPEN]            = "boot.db_open",
        [EV_BOOT_COINS_OPEN]         = "boot.coins_open",
        [EV_BOOT_UTXO_IMPORT]        = "boot.utxo_import",
        [EV_BOOT_BLOCK_INDEX]        = "boot.block_index",
        [EV_BOOT_CHAIN_RESTORED]     = "boot.chain_restored",
        [EV_BOOT_ACTIVATE]           = "boot.activate",
        [EV_BOOT_VALIDATION_FAILED]  = "boot.validation_failed",
        /* Validation pipeline (detailed) */
        [EV_BLOCK_CHECK_PASSED]      = "val.check_passed",
        [EV_BLOCK_CONNECT_START]     = "val.connect_start",
        [EV_BLOCK_CONNECT_DONE]      = "val.connect_done",
        [EV_TX_INPUTS_CHECKED]       = "val.tx_inputs",
        [EV_TURNSTILE_CHECK]         = "val.turnstile",
        [EV_SCRIPT_VERIFIED]         = "val.script_verified",
        [EV_UTXO_CHECKPOINT_PASS]    = "val.utxo_cp_pass",
        [EV_UTXO_CHECKPOINT_FAIL]    = "val.utxo_cp_fail",
        /* ActiveRecord model lifecycle */
        [EV_MODEL_SAVED]             = "model.saved",
        [EV_MODEL_DESTROYED]         = "model.destroyed",
        [EV_MODEL_VALIDATION_FAILED] = "model.validation_failed",
        /* System */
        [EV_NODE_STARTING]           = "sys.starting",
        [EV_NODE_READY]              = "sys.ready",
        [EV_NODE_SHUTDOWN]           = "sys.shutdown",
        [EV_CRASH]                   = "sys.crash",
        [EV_DB_ERROR]                = "sys.db_error",
        [EV_MMB_APPEND]              = "mmb.append",
        [EV_MMB_PROOF_VERIFIED]      = "mmb.proof_verified",
        [EV_FC_SAMPLE_VERIFIED]      = "fc.sample_verified",
        [EV_FC_CHAIN_VERIFIED]       = "fc.chain_verified",
        [EV_SNAPSYNC_STATE_CHANGE]   = "snapsync.state",
        [EV_SNAPSYNC_PROGRESS]       = "snapsync.progress",
        [EV_SNAPSYNC_VERIFIED]       = "snapsync.verified",
    };
    if (type >= 0 && type < EV_NUM_TYPES && names[type])
        return names[type];
    return "unknown";
}

/* ── Dump to stderr ──────────────────────────────────────── */

static void format_event(FILE *f, const struct event *ev)
{
    int64_t sec = ev->timestamp_us / 1000000;
    int64_t usec = ev->timestamp_us % 1000000;

    fprintf(f, "[%lld.%06lld] %-28s peer=%-4u ",
            (long long)sec, (long long)usec,
            event_type_name(ev->type), ev->peer_id);

    /* Print payload as string if it looks like text, else hex */
    if (ev->payload_len > 0) {
        bool is_text = true;
        for (uint32_t i = 0; i < ev->payload_len; i++) {
            if (ev->payload[i] < 0x20 && ev->payload[i] != 0) {
                is_text = false;
                break;
            }
        }
        if (is_text) {
            fprintf(f, "\"%.*s\"", (int)ev->payload_len,
                    (const char *)ev->payload);
        } else {
            for (uint32_t i = 0; i < ev->payload_len && i < 32; i++)
                fprintf(f, "%02x", ev->payload[i]);
            if (ev->payload_len > 32)
                fprintf(f, "...");
        }
    }
    fprintf(f, "\n");
}

void event_dump_recent(size_t count)
{
    if (!atomic_load(&g_log.initialized))
        return;

    uint64_t end = atomic_load(&g_log.write_pos);
    if (count > EVENT_LOG_SIZE) count = EVENT_LOG_SIZE;
    uint64_t start = end > count ? end - count : 0;

    fprintf(stderr, "\n═══ EVENT LOG (last %zu of %llu total) ═══\n",
            count, (unsigned long long)end);

    for (uint64_t i = start; i < end; i++) {
        const struct event *ev = &g_log.ring[i & EVENT_LOG_MASK];
        uint64_t seq = atomic_load_explicit(&ev->sequence,
                                             memory_order_acquire);
        if (seq != i + 1) {
            fprintf(stderr, "[%llu] <overwritten>\n", (unsigned long long)i);
            continue;
        }
        format_event(stderr, ev);
    }
    fprintf(stderr, "═══ END EVENT LOG ═══\n\n");
}

/* ── JSON dump ───────────────────────────────────────────── */

static size_t json_escape(char *out, size_t out_size,
                           const char *s, size_t len)
{
    size_t w = 0;
    for (size_t i = 0; i < len && w + 6 < out_size; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"')       { out[w++] = '\\'; out[w++] = '"'; }
        else if (c == '\\') { out[w++] = '\\'; out[w++] = '\\'; }
        else if (c == '\n') { out[w++] = '\\'; out[w++] = 'n'; }
        else if (c == '\r') { out[w++] = '\\'; out[w++] = 'r'; }
        else if (c == '\t') { out[w++] = '\\'; out[w++] = 't'; }
        else if (c < 0x20)  { w += (size_t)snprintf(out + w, out_size - w,
                                                     "\\u%04x", c); }
        else                { out[w++] = (char)c; }
    }
    return w;
}

size_t event_dump_json(char *buf, size_t buf_size, size_t count)
{
    if (!atomic_load(&g_log.initialized))
        return 0;

    uint64_t end = atomic_load(&g_log.write_pos);
    if (count > EVENT_LOG_SIZE) count = EVENT_LOG_SIZE;
    uint64_t start = end > count ? end - count : 0;

    size_t w = 0;
    if (w < buf_size) buf[w++] = '[';

    bool first = true;
    for (uint64_t i = start; i < end && w + 256 < buf_size; i++) {
        const struct event *ev = &g_log.ring[i & EVENT_LOG_MASK];
        uint64_t seq = atomic_load_explicit(&ev->sequence,
                                             memory_order_acquire);
        if (seq != i + 1) continue;

        if (!first && w < buf_size) buf[w++] = ',';
        first = false;

        /* Payload as escaped string */
        char escaped[256];
        size_t elen = 0;
        if (ev->payload_len > 0) {
            bool is_text = true;
            for (uint32_t j = 0; j < ev->payload_len; j++) {
                if (ev->payload[j] < 0x20 && ev->payload[j] != 0) {
                    is_text = false;
                    break;
                }
            }
            if (is_text) {
                elen = json_escape(escaped, sizeof(escaped),
                                   (const char *)ev->payload,
                                   ev->payload_len);
            } else {
                for (uint32_t j = 0; j < ev->payload_len &&
                     elen + 2 < sizeof(escaped); j++)
                    elen += (size_t)snprintf(escaped + elen,
                                             sizeof(escaped) - elen,
                                             "%02x", ev->payload[j]);
            }
        }

        w += (size_t)snprintf(buf + w, buf_size - w,
            "{\"seq\":%llu,\"ts\":%lld,\"type\":\"%s\","
            "\"peer\":%u,\"data\":\"%.*s\"}",
            (unsigned long long)(i),
            (long long)ev->timestamp_us,
            event_type_name(ev->type),
            ev->peer_id,
            (int)elen, escaped);
    }

    if (w < buf_size) buf[w++] = ']';
    if (w < buf_size) buf[w] = '\0';
    return w;
}

size_t event_dump_json_filtered(char *buf, size_t buf_size, size_t count,
                                 const char *type_prefix)
{
    if (!atomic_load(&g_log.initialized) || !type_prefix)
        return event_dump_json(buf, buf_size, count);

    size_t prefix_len = strlen(type_prefix);
    if (prefix_len == 0)
        return event_dump_json(buf, buf_size, count);

    uint64_t end = atomic_load(&g_log.write_pos);
    /* Scan more events than requested to find `count` matches */
    uint64_t scan = count * 10;
    if (scan > EVENT_LOG_SIZE) scan = EVENT_LOG_SIZE;
    uint64_t start = end > scan ? end - scan : 0;

    size_t w = 0;
    if (w < buf_size) buf[w++] = '[';

    bool first = true;
    size_t matched = 0;
    for (uint64_t i = start; i < end && w + 256 < buf_size && matched < count; i++) {
        const struct event *ev = &g_log.ring[i & EVENT_LOG_MASK];
        uint64_t seq = atomic_load_explicit(&ev->sequence,
                                             memory_order_acquire);
        if (seq != i + 1) continue;

        const char *name = event_type_name(ev->type);
        if (strncmp(name, type_prefix, prefix_len) != 0)
            continue;

        if (!first && w < buf_size) buf[w++] = ',';
        first = false;
        matched++;

        char escaped[256];
        size_t elen = 0;
        if (ev->payload_len > 0) {
            bool is_text = true;
            for (uint32_t j = 0; j < ev->payload_len; j++) {
                if (ev->payload[j] < 0x20 && ev->payload[j] != 0) {
                    is_text = false;
                    break;
                }
            }
            if (is_text)
                elen = json_escape(escaped, sizeof(escaped),
                                   (const char *)ev->payload, ev->payload_len);
            else
                for (uint32_t j = 0; j < ev->payload_len &&
                     elen + 2 < sizeof(escaped); j++)
                    elen += (size_t)snprintf(escaped + elen,
                                             sizeof(escaped) - elen,
                                             "%02x", ev->payload[j]);
        }

        w += (size_t)snprintf(buf + w, buf_size - w,
            "{\"seq\":%llu,\"ts\":%lld,\"type\":\"%s\","
            "\"peer\":%u,\"data\":\"%.*s\"}",
            (unsigned long long)(i),
            (long long)ev->timestamp_us,
            name, ev->peer_id,
            (int)elen, escaped);
    }

    if (w < buf_size) buf[w++] = ']';
    if (w < buf_size) buf[w] = '\0';
    return w;
}

/* ── Crash handler ───────────────────────────────────────── */

static void crash_signal_handler(int sig)
{
    /* Async-signal-safe: fprintf to stderr, then _exit.
     * event_dump_recent uses fprintf which is technically not
     * async-signal-safe, but on Linux it works in practice for
     * crash diagnostics. This is a best-effort dump. */
    fprintf(stderr, "\n\n*** FATAL SIGNAL %d ***\n", sig);

    /* Print stack backtrace — requires -rdynamic for symbol names */
    {
        void *frames[64];
        int nframes = backtrace(frames, 64);
        fprintf(stderr, "\n=== STACK BACKTRACE (%d frames) ===\n", nframes);
        backtrace_symbols_fd(frames, nframes, STDERR_FILENO);
        fprintf(stderr, "=== END BACKTRACE ===\n\n");
    }

    event_emitf(EV_CRASH, 0, "signal %d", sig);
    event_dump_recent(200);
    _exit(128 + sig);
}

void event_install_crash_handler(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = crash_signal_handler;
    sa.sa_flags = SA_RESETHAND; /* one-shot: avoid infinite recursion */
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
}

/* ── Peer state machine ──────────────────────────────────── */

const char *peer_state_name(enum peer_state state)
{
    static const char *names[] = {
        [PEER_DISCONNECTED]      = "disconnected",
        [PEER_CONNECTING]        = "connecting",
        [PEER_CONNECTED]         = "connected",
        [PEER_VERSION_SENT]      = "version_sent",
        [PEER_VERSION_RECEIVED]  = "version_received",
        [PEER_HANDSHAKE_COMPLETE]= "handshake_complete",
        [PEER_ACTIVE]            = "active",
        [PEER_SYNCING_HEADERS]   = "syncing_headers",
        [PEER_SYNCING_BLOCKS]    = "syncing_blocks",
        [PEER_SNAPSHOT_SERVING]  = "snapshot_serving",
        [PEER_SNAPSHOT_RECEIVING]= "snapshot_receiving",
        [PEER_STALE]             = "stale",
        [PEER_DISCONNECTING]     = "disconnecting",
        [PEER_BANNED]            = "banned",
    };
    if (state >= 0 && state < PEER_NUM_STATES)
        return names[state];
    return "unknown";
}

/* Transition table: [from][to] = legal.
 * Only transitions explicitly listed here are allowed.
 * Anything else is a bug that gets caught immediately. */
static const bool g_peer_transitions[PEER_NUM_STATES][PEER_NUM_STATES] = {
    /* DISCONNECTED can go to CONNECTING or CONNECTED (inbound) */
    [PEER_DISCONNECTED][PEER_CONNECTING]         = true,
    [PEER_DISCONNECTED][PEER_CONNECTED]          = true,

    /* CONNECTING goes to CONNECTED or VERSION_SENT (outbound shortcut) */
    [PEER_CONNECTING][PEER_CONNECTED]            = true,
    [PEER_CONNECTING][PEER_VERSION_SENT]         = true,
    [PEER_CONNECTING][PEER_DISCONNECTED]         = true,
    [PEER_CONNECTING][PEER_DISCONNECTING]        = true,

    /* CONNECTED: send or receive version */
    [PEER_CONNECTED][PEER_VERSION_SENT]          = true,
    [PEER_CONNECTED][PEER_VERSION_RECEIVED]      = true,
    [PEER_CONNECTED][PEER_DISCONNECTING]         = true,

    /* VERSION_SENT: receive their version */
    [PEER_VERSION_SENT][PEER_VERSION_RECEIVED]   = true,
    [PEER_VERSION_SENT][PEER_HANDSHAKE_COMPLETE] = true,
    [PEER_VERSION_SENT][PEER_DISCONNECTING]      = true,

    /* VERSION_RECEIVED: handshake completes */
    [PEER_VERSION_RECEIVED][PEER_VERSION_SENT]   = true,
    [PEER_VERSION_RECEIVED][PEER_HANDSHAKE_COMPLETE] = true,
    [PEER_VERSION_RECEIVED][PEER_DISCONNECTING]  = true,

    /* HANDSHAKE_COMPLETE: transition to active or sync */
    [PEER_HANDSHAKE_COMPLETE][PEER_ACTIVE]       = true,
    [PEER_HANDSHAKE_COMPLETE][PEER_SYNCING_HEADERS] = true,
    [PEER_HANDSHAKE_COMPLETE][PEER_SYNCING_BLOCKS]  = true,
    [PEER_HANDSHAKE_COMPLETE][PEER_SNAPSHOT_SERVING] = true,
    [PEER_HANDSHAKE_COMPLETE][PEER_SNAPSHOT_RECEIVING] = true,
    [PEER_HANDSHAKE_COMPLETE][PEER_DISCONNECTING]= true,

    /* ACTIVE: can start syncing or snapshot */
    [PEER_ACTIVE][PEER_SYNCING_HEADERS]          = true,
    [PEER_ACTIVE][PEER_SYNCING_BLOCKS]           = true,
    [PEER_ACTIVE][PEER_SNAPSHOT_SERVING]          = true,
    [PEER_ACTIVE][PEER_SNAPSHOT_RECEIVING]        = true,
    [PEER_ACTIVE][PEER_STALE]                    = true,
    [PEER_ACTIVE][PEER_DISCONNECTING]            = true,

    /* SYNCING_HEADERS: done → blocks or active, or fail */
    [PEER_SYNCING_HEADERS][PEER_SYNCING_BLOCKS]  = true,
    [PEER_SYNCING_HEADERS][PEER_ACTIVE]          = true,
    [PEER_SYNCING_HEADERS][PEER_STALE]           = true,
    [PEER_SYNCING_HEADERS][PEER_DISCONNECTING]   = true,

    /* SYNCING_BLOCKS: done → active, or fail */
    [PEER_SYNCING_BLOCKS][PEER_ACTIVE]           = true,
    [PEER_SYNCING_BLOCKS][PEER_SYNCING_HEADERS]  = true,
    [PEER_SYNCING_BLOCKS][PEER_STALE]            = true,
    [PEER_SYNCING_BLOCKS][PEER_DISCONNECTING]    = true,

    /* SNAPSHOT states: complete → active, or fail */
    [PEER_SNAPSHOT_SERVING][PEER_ACTIVE]          = true,
    [PEER_SNAPSHOT_SERVING][PEER_DISCONNECTING]   = true,
    [PEER_SNAPSHOT_RECEIVING][PEER_ACTIVE]         = true,
    [PEER_SNAPSHOT_RECEIVING][PEER_DISCONNECTING]  = true,

    /* STALE: can recover or disconnect */
    [PEER_STALE][PEER_ACTIVE]                    = true,
    [PEER_STALE][PEER_SYNCING_HEADERS]           = true,
    [PEER_STALE][PEER_DISCONNECTING]             = true,

    /* DISCONNECTING always goes to DISCONNECTED */
    [PEER_DISCONNECTING][PEER_DISCONNECTED]      = true,

    /* BANNED always goes to DISCONNECTED */
    [PEER_BANNED][PEER_DISCONNECTED]             = true,

    /* Any state can be banned or disconnect */
    [PEER_ACTIVE][PEER_BANNED]                   = true,
    [PEER_SYNCING_HEADERS][PEER_BANNED]          = true,
    [PEER_SYNCING_BLOCKS][PEER_BANNED]           = true,
    [PEER_HANDSHAKE_COMPLETE][PEER_BANNED]       = true,
    [PEER_SNAPSHOT_SERVING][PEER_BANNED]          = true,
    [PEER_SNAPSHOT_RECEIVING][PEER_BANNED]        = true,
};

bool peer_transition_valid(enum peer_state from, enum peer_state to)
{
    if (from >= PEER_NUM_STATES || to >= PEER_NUM_STATES)
        return false;
    return g_peer_transitions[from][to];
}

bool peer_set_state_checked(uint32_t peer_id, enum peer_state *current,
                            enum peer_state new_state, const char *reason)
{
    enum peer_state old = *current;

    if (!peer_transition_valid(old, new_state)) {
        /* Illegal transition — this is always a bug */
        char buf[EVENT_PAYLOAD_SIZE];
        int n = snprintf(buf, sizeof(buf), "ILLEGAL %s->%s: %s",
                         peer_state_name(old), peer_state_name(new_state),
                         reason ? reason : "");
        event_emit(EV_PEER_STATE_CHANGE, peer_id, buf, (uint32_t)(n > 0 ? n : 0));
        fprintf(stderr, "BUG: peer %u illegal transition %s -> %s (%s)\n",
                peer_id, peer_state_name(old),
                peer_state_name(new_state),
                reason ? reason : "");
        return false;
    }

    *current = new_state;

    char buf[EVENT_PAYLOAD_SIZE];
    int n = snprintf(buf, sizeof(buf), "%s->%s: %s",
                     peer_state_name(old), peer_state_name(new_state),
                     reason ? reason : "");
    event_emit(EV_PEER_STATE_CHANGE, peer_id, buf, (uint32_t)(n > 0 ? n : 0));
    return true;
}

/* ── Sync state machine ──────────────────────────────────── */

static _Atomic int g_sync_state = SYNC_IDLE;

const char *sync_state_name(enum sync_state state)
{
    static const char *names[] = {
        [SYNC_IDLE]              = "idle",
        [SYNC_FINDING_PEERS]     = "finding_peers",
        [SYNC_HEADERS_DOWNLOAD]  = "headers_download",
        [SYNC_BLOCKS_DOWNLOAD]   = "blocks_download",
        [SYNC_CONNECTING_BLOCKS] = "connecting_blocks",
        [SYNC_AT_TIP]            = "at_tip",
        [SYNC_REORG]             = "reorg",
        [SYNC_REORG_RECOVERY]    = "reorg_recovery",
        [SYNC_SNAPSHOT_RECEIVE]  = "snapshot_receive",
        [SYNC_FAILED]            = "failed",
    };
    if (state >= 0 && state < SYNC_NUM_STATES)
        return names[state];
    return "unknown";
}

/* Sync transition table */
static const bool g_sync_transitions[SYNC_NUM_STATES][SYNC_NUM_STATES] = {
    [SYNC_IDLE][SYNC_FINDING_PEERS]       = true,
    [SYNC_IDLE][SYNC_HEADERS_DOWNLOAD]    = true,
    [SYNC_IDLE][SYNC_SNAPSHOT_RECEIVE]    = true,

    [SYNC_FINDING_PEERS][SYNC_HEADERS_DOWNLOAD]  = true,
    [SYNC_FINDING_PEERS][SYNC_SNAPSHOT_RECEIVE]   = true,
    [SYNC_FINDING_PEERS][SYNC_IDLE]               = true,
    [SYNC_FINDING_PEERS][SYNC_FAILED]             = true,

    [SYNC_HEADERS_DOWNLOAD][SYNC_BLOCKS_DOWNLOAD]   = true,
    [SYNC_HEADERS_DOWNLOAD][SYNC_CONNECTING_BLOCKS]  = true,
    [SYNC_HEADERS_DOWNLOAD][SYNC_AT_TIP]             = true,
    [SYNC_HEADERS_DOWNLOAD][SYNC_IDLE]               = true,
    [SYNC_HEADERS_DOWNLOAD][SYNC_FAILED]             = true,

    [SYNC_BLOCKS_DOWNLOAD][SYNC_CONNECTING_BLOCKS]   = true,
    [SYNC_BLOCKS_DOWNLOAD][SYNC_AT_TIP]              = true,
    [SYNC_BLOCKS_DOWNLOAD][SYNC_HEADERS_DOWNLOAD]    = true,
    [SYNC_BLOCKS_DOWNLOAD][SYNC_IDLE]                = true,
    [SYNC_BLOCKS_DOWNLOAD][SYNC_FAILED]              = true,

    [SYNC_CONNECTING_BLOCKS][SYNC_AT_TIP]            = true,
    [SYNC_CONNECTING_BLOCKS][SYNC_HEADERS_DOWNLOAD]  = true,
    [SYNC_CONNECTING_BLOCKS][SYNC_BLOCKS_DOWNLOAD]   = true,
    [SYNC_CONNECTING_BLOCKS][SYNC_IDLE]              = true,
    [SYNC_CONNECTING_BLOCKS][SYNC_FAILED]            = true,

    [SYNC_AT_TIP][SYNC_HEADERS_DOWNLOAD]   = true,
    [SYNC_AT_TIP][SYNC_REORG]              = true,
    [SYNC_AT_TIP][SYNC_IDLE]               = true,

    [SYNC_REORG][SYNC_AT_TIP]              = true,
    [SYNC_REORG][SYNC_CONNECTING_BLOCKS]   = true,
    [SYNC_REORG][SYNC_REORG_RECOVERY]      = true,

    /* Recovery can trigger from any block-processing state */
    [SYNC_BLOCKS_DOWNLOAD][SYNC_REORG_RECOVERY]    = true,
    [SYNC_CONNECTING_BLOCKS][SYNC_REORG_RECOVERY]  = true,
    [SYNC_IDLE][SYNC_REORG_RECOVERY]               = true,
    [SYNC_REORG][SYNC_FAILED]              = true,

    [SYNC_REORG_RECOVERY][SYNC_CONNECTING_BLOCKS] = true,
    [SYNC_REORG_RECOVERY][SYNC_BLOCKS_DOWNLOAD]   = true,
    [SYNC_REORG_RECOVERY][SYNC_IDLE]              = true,
    [SYNC_REORG_RECOVERY][SYNC_FAILED]             = true,

    [SYNC_SNAPSHOT_RECEIVE][SYNC_CONNECTING_BLOCKS] = true,
    [SYNC_SNAPSHOT_RECEIVE][SYNC_HEADERS_DOWNLOAD]  = true,
    [SYNC_SNAPSHOT_RECEIVE][SYNC_AT_TIP]            = true,
    [SYNC_SNAPSHOT_RECEIVE][SYNC_IDLE]              = true,
    [SYNC_SNAPSHOT_RECEIVE][SYNC_FAILED]            = true,

    [SYNC_FAILED][SYNC_IDLE]               = true,
    [SYNC_FAILED][SYNC_FINDING_PEERS]      = true,
};

enum sync_state sync_get_state(void)
{
    return (enum sync_state)atomic_load(&g_sync_state);
}

bool sync_set_state(enum sync_state new_state, const char *reason)
{
    enum sync_state old = (enum sync_state)atomic_load(&g_sync_state);

    if (old == new_state)
        return true; /* no-op */

    if (!g_sync_transitions[old][new_state]) {
        char buf[EVENT_PAYLOAD_SIZE];
        int n = snprintf(buf, sizeof(buf), "ILLEGAL %s->%s: %s",
                         sync_state_name(old), sync_state_name(new_state),
                         reason ? reason : "");
        event_emit(EV_SYNC_STATE_CHANGE, 0, buf, (uint32_t)(n > 0 ? n : 0));
        fprintf(stderr, "BUG: sync illegal transition %s -> %s (%s)\n",
                sync_state_name(old), sync_state_name(new_state),
                reason ? reason : "");
        return false;
    }

    atomic_store(&g_sync_state, (int)new_state);

    char buf[EVENT_PAYLOAD_SIZE];
    int n = snprintf(buf, sizeof(buf), "%s->%s: %s",
                     sync_state_name(old), sync_state_name(new_state),
                     reason ? reason : "");
    event_emit(EV_SYNC_STATE_CHANGE, 0, buf, (uint32_t)(n > 0 ? n : 0));

    printf("Sync: %s -> %s (%s)\n",
           sync_state_name(old), sync_state_name(new_state),
           reason ? reason : "");

    return true;
}

/* ── Snapshot sync state machine ────────────────────────── */

static _Atomic int g_snapsync_state = SNAPSYNC_IDLE;

const char *snapsync_state_name(enum snapshot_sync_state state)
{
    static const char *names[] = {
        [SNAPSYNC_IDLE]        = "idle",
        [SNAPSYNC_NEGOTIATING] = "negotiating",
        [SNAPSYNC_RECEIVING]   = "receiving",
        [SNAPSYNC_VERIFYING]   = "verifying",
        [SNAPSYNC_COMPLETE]    = "complete",
        [SNAPSYNC_FAILED]      = "failed",
    };
    if (state >= 0 && state < SNAPSYNC_NUM_STATES)
        return names[state];
    return "unknown";
}

static const bool g_snapsync_transitions[SNAPSYNC_NUM_STATES][SNAPSYNC_NUM_STATES] = {
    [SNAPSYNC_IDLE][SNAPSYNC_NEGOTIATING]       = true,
    [SNAPSYNC_NEGOTIATING][SNAPSYNC_RECEIVING]   = true,
    [SNAPSYNC_NEGOTIATING][SNAPSYNC_FAILED]      = true,
    [SNAPSYNC_NEGOTIATING][SNAPSYNC_IDLE]        = true,
    [SNAPSYNC_RECEIVING][SNAPSYNC_VERIFYING]      = true,
    [SNAPSYNC_RECEIVING][SNAPSYNC_FAILED]         = true,
    [SNAPSYNC_VERIFYING][SNAPSYNC_COMPLETE]       = true,
    [SNAPSYNC_VERIFYING][SNAPSYNC_FAILED]         = true,
    [SNAPSYNC_COMPLETE][SNAPSYNC_IDLE]            = true,
    [SNAPSYNC_FAILED][SNAPSYNC_IDLE]              = true,
};

enum snapshot_sync_state snapsync_get_state(void)
{
    return (enum snapshot_sync_state)atomic_load(&g_snapsync_state);
}

bool snapsync_set_state(enum snapshot_sync_state new_state, const char *reason)
{
    enum snapshot_sync_state old =
        (enum snapshot_sync_state)atomic_load(&g_snapsync_state);

    if (old == new_state) return true;

    if (!g_snapsync_transitions[old][new_state]) {
        fprintf(stderr, "BUG: snapsync illegal %s -> %s (%s)\n",
                snapsync_state_name(old), snapsync_state_name(new_state),
                reason ? reason : "");
        return false;
    }

    atomic_store(&g_snapsync_state, (int)new_state);

    char buf[EVENT_PAYLOAD_SIZE];
    snprintf(buf, sizeof(buf), "%s->%s: %s",
             snapsync_state_name(old), snapsync_state_name(new_state),
             reason ? reason : "");
    event_emit(EV_SNAPSYNC_STATE_CHANGE, 0, buf, (uint32_t)strlen(buf));

    printf("[snapsync] %s -> %s (%s)\n",
           snapsync_state_name(old), snapsync_state_name(new_state),
           reason ? reason : "");
    return true;
}

/* ── Error accumulator ──────────────────────────────────── */

void error_ring_init(struct error_ring *r)
{
    memset(r, 0, sizeof(*r));
    atomic_store(&r->write_pos, 0);
    atomic_store(&r->total_count, 0);
}

void error_ring_observer(enum event_type type, uint32_t peer_id,
                         const void *payload, uint32_t payload_len, void *ctx)
{
    (void)peer_id;
    struct error_ring *r = (struct error_ring *)ctx;
    int pos = atomic_fetch_add(&r->write_pos, 1) % ERROR_RING_SIZE;
    struct error_entry *e = &r->entries[pos];

    struct timeval tv;
    gettimeofday(&tv, NULL);
    e->timestamp_us = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
    e->type = type;
    if (payload && payload_len > 0) {
        size_t copy = payload_len < sizeof(e->message) - 1
                      ? payload_len : sizeof(e->message) - 1;
        memcpy(e->message, payload, copy);
        e->message[copy] = '\0';
    } else {
        e->message[0] = '\0';
    }
    atomic_fetch_add(&r->total_count, 1);
}

int error_ring_total(const struct error_ring *r)
{
    return atomic_load(&r->total_count);
}

const struct error_entry *error_ring_last(const struct error_ring *r)
{
    int total = atomic_load(&r->total_count);
    if (total == 0) return NULL;
    int pos = (atomic_load(&r->write_pos) - 1 + ERROR_RING_SIZE) % ERROR_RING_SIZE;
    return &r->entries[pos];
}

size_t error_ring_dump_json(const struct error_ring *r, char *buf, size_t sz)
{
    int total = atomic_load(&r->total_count);
    int count = total < ERROR_RING_SIZE ? total : ERROR_RING_SIZE;
    int wp = atomic_load(&r->write_pos);

    size_t off = 0;
    off += (size_t)snprintf(buf + off, sz - off,
        "{\"total\":%d,\"errors\":[", total);

    for (int i = 0; i < count && off < sz - 2; i++) {
        int idx = (wp - count + i + ERROR_RING_SIZE) % ERROR_RING_SIZE;
        const struct error_entry *e = &r->entries[idx];
        if (i > 0) off += (size_t)snprintf(buf + off, sz - off, ",");
        off += (size_t)snprintf(buf + off, sz - off,
            "{\"type\":\"%s\",\"time\":%lld,\"msg\":\"%s\"}",
            event_type_name(e->type),
            (long long)(e->timestamp_us / 1000000),
            e->message);
    }
    off += (size_t)snprintf(buf + off, sz - off, "]}");
    return off;
}

struct error_ring *error_ring_global(void)
{
    return &g_error_ring;
}
