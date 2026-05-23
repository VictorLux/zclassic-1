/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Framework mailbox — thin re-export of util/mailbox with typed-message
 * macros for in-tree adopters. See docs/FRAMEWORK.md section 1. */

#ifndef ZCL_FRAMEWORK_MAILBOX_H
#define ZCL_FRAMEWORK_MAILBOX_H

#include "util/mailbox.h"

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

/* MAILBOX_DECLARE(name, T)
 *   declares a typed inbox for messages of type T.
 *
 * MAILBOX_DEFINE(name, T, capacity)
 *   defines storage and bookkeeping in one .c file.
 *
 * mailbox_<name>_push(const T *msg) -> bool
 *   non-blocking push; returns false on full.
 *
 * mailbox_<name>_drain(void (*handler)(const T *)) -> size_t
 *   drains all queued messages, calls handler per message, returns count. */
#define MAILBOX_DECLARE(name, T) \
    bool mailbox_##name##_push(const T *msg); \
    size_t mailbox_##name##_drain(void (*handler)(const T *msg))

#define MAILBOX_DEFINE(name, T, capacity) \
    static mailbox_t *g_mbox_##name; \
    static pthread_mutex_t g_mbox_##name##_init_lock = PTHREAD_MUTEX_INITIALIZER; \
    static bool mailbox_##name##_init_once(void) { \
        pthread_mutex_lock(&g_mbox_##name##_init_lock); \
        if (!g_mbox_##name) \
            g_mbox_##name = mailbox_create((capacity), sizeof(T)); \
        bool ok = (g_mbox_##name != NULL); \
        pthread_mutex_unlock(&g_mbox_##name##_init_lock); \
        return ok; \
    } \
    bool mailbox_##name##_push(const T *msg) { \
        if (!msg || !mailbox_##name##_init_once()) return false; \
        return mailbox_try_send(g_mbox_##name, msg); \
    } \
    size_t mailbox_##name##_drain(void (*handler)(const T *msg)) { \
        if (!handler || !mailbox_##name##_init_once()) return 0; \
        T tmp; \
        size_t n = 0; \
        size_t limit = mailbox_depth(g_mbox_##name); \
        while (n < limit && mailbox_try_recv(g_mbox_##name, &tmp)) { \
            handler(&tmp); \
            n++; \
        } \
        return n; \
    }

#endif /* ZCL_FRAMEWORK_MAILBOX_H */
