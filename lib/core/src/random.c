/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * ─────────────────────────────────────────────────────────────────
 * root-cause fix for the GetRandBytes fail-open that was
 * flagged during (see AGENT-3.md note at end for the original
 * writeup).
 *
 * The prior implementation opened /dev/urandom and silently
 * `memset(buf, 0, num)`-ed on open() failure (chroot/container
 * without /dev mounted, fd exhaustion, SELinux denial, early boot
 * before /dev is populated, ...). Every secret in the binary —
 * Sapling note randomness (rcm / rcv / esk / ar), RedJubjub signing
 * nonces, Groth16 proof blinding factors, ephemeral DH secrets,
 * HD wallet seeds, BIP39 entropy, P2P session keys, CSRF keys — is
 * ultimately sourced here, so a silent zero-fill becomes a same-key-
 * everywhere catastrophe with no log line and no caller-visible
 * signal.
 *
 * The public signature is left unchanged (void return, per the
 * AGENT-3 scope boundary that keeps lib/core/include/ off-limits).
 * Failure semantics are now strict: if we cannot fill the output
 * buffer with real entropy, we log to stderr and abort(). No caller
 * ever observes a zero-filled "success" — the process is dead before
 * the return. abort() is appropriate here because we cannot locally
 * distinguish secret from non-secret callers; the fatal policy is
 * correct for the secret case and merely noisy for the non-secret
 * case.
 *
 * Entropy sources, in order:
 *   1. getrandom(2) (Linux ≥ 3.17, glibc ≥ 2.25). Works in chroots
 *      and containers without /dev mounted. We use the blocking
 *      default (flags=0) which waits for the kernel CSPRNG to be
 *      initialized on first call and then never blocks again.
 *   2. /dev/urandom, as a fallback when getrandom returns ENOSYS
 *      (older kernel or locked-down seccomp filter).
 *   EINTR and short-read cases are retried in both paths.
 * ───────────────────────────────────────────────────────────────── */

#include "core/random.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/random.h>
#define ZCL_HAS_GETRANDOM 1
#else
#define ZCL_HAS_GETRANDOM 0
#endif

#ifdef ZCL_TESTING
#include <stdatomic.h>
/* Test-only fault injection: when set, GetRandBytes skips both
 * syscall paths and goes straight to the fatal log+abort. A failing-
 * RNG unit test fork()s a child, flips this, calls GetRandBytes,
 * and asserts WTERMSIG == SIGABRT in the parent. Not exported from
 * core/random.h (that header is off-limits for this fix); callers
 * forward-declare with `extern void zcl_random_test_force_fail(bool);`. */
static atomic_bool g_rng_force_fail = false;
void zcl_random_test_force_fail(bool on);
void zcl_random_test_force_fail(bool on)
{
    atomic_store_explicit(&g_rng_force_fail, on, memory_order_release);
}
#endif

static bool fill_from_getrandom(unsigned char *buf, size_t num)
{
#if ZCL_HAS_GETRANDOM
    size_t got = 0;
    while (got < num) {
        ssize_t r = getrandom(buf + got, num - got, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false; /* ENOSYS / seccomp / etc → caller falls back */
        }
        if (r == 0) return false;
        got += (size_t)r;
    }
    return true;
#else
    (void)buf; (void)num;
    return false;
#endif
}

static bool fill_from_urandom(unsigned char *buf, size_t num)
{
    int fd;
    do {
        fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) return false;

    size_t got = 0;
    while (got < num) {
        ssize_t r = read(fd, buf + got, num - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return false;
        }
        if (r == 0) {
            /* EOF on /dev/urandom — should never happen. */
            close(fd);
            return false;
        }
        got += (size_t)r;
    }
    close(fd);
    return true;
}

void GetRandBytes(unsigned char *buf, size_t num)
{
    if (num == 0) return;

#ifdef ZCL_TESTING
    if (atomic_load_explicit(&g_rng_force_fail, memory_order_acquire)) {
        fprintf(stderr,  // obs-ok:helper-context-logged
            "[fatal] %s:%d GetRandBytes(): ZCL_TEST_FORCE_RNG_FAIL active "
            "— aborting to avoid zero-fill (num=%zu)\n",
            __FILE__, __LINE__, num);
        fflush(stderr);
        abort();
    }
#endif

    if (fill_from_getrandom(buf, num)) return;
    if (fill_from_urandom(buf, num))   return;

    int saved = errno;
    fprintf(stderr,  // obs-ok:helper-context-logged
        "[fatal] %s:%d GetRandBytes(): no entropy source available "
        "(getrandom + /dev/urandom both failed, errno=%d %s) for %zu bytes "
        "— aborting to avoid silent zero-fill\n",
        __FILE__, __LINE__, saved, strerror(saved), num);
    fflush(stderr);
    abort();
}

uint64_t GetRand(uint64_t nMax)
{
    if (nMax == 0) return 0;
    uint64_t nRange = (UINT64_MAX / nMax) * nMax;
    uint64_t nRand = 0;
    do {
        GetRandBytes((unsigned char *)&nRand, sizeof(nRand));
    } while (nRand >= nRange);
    return nRand % nMax;
}

int GetRandInt(int nMax)
{
    return (int)GetRand((uint64_t)nMax);
}

void GetRandHash(struct uint256 *hash)
{
    GetRandBytes(hash->data, 32);
}

uint32_t insecure_rand_Rz = 11;
uint32_t insecure_rand_Rw = 11;

void seed_insecure_rand(bool deterministic)
{
    if (deterministic) {
        insecure_rand_Rz = insecure_rand_Rw = 11;
    } else {
        uint32_t tmp;
        do {
            GetRandBytes((unsigned char *)&tmp, 4);
        } while (tmp == 0 || tmp == 0x9068ffffU);
        insecure_rand_Rz = tmp;
        do {
            GetRandBytes((unsigned char *)&tmp, 4);
        } while (tmp == 0 || tmp == 0x464fffffU);
        insecure_rand_Rw = tmp;
    }
}
