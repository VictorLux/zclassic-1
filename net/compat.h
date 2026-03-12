/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_COMPAT_H
#define ZCL_COMPAT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifdef FD_SETSIZE
#undef FD_SETSIZE
#endif
#define FD_SETSIZE 1024

#include <winsock2.h>
#include <mswsock.h>
#include <windows.h>
#include <ws2tcpip.h>
#define MSG_DONTWAIT 0
#ifndef S_IRUSR
#define S_IRUSR 0400
#define S_IWUSR 0200
#endif
#else
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <limits.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>

typedef unsigned int SOCKET;
#define WSAGetLastError()   errno
#define WSAEINVAL           EINVAL
#define WSAEALREADY         EALREADY
#define WSAEWOULDBLOCK      EWOULDBLOCK
#define WSAEMSGSIZE         EMSGSIZE
#define WSAEINTR            EINTR
#define WSAEINPROGRESS      EINPROGRESS
#define WSAEADDRINUSE       EADDRINUSE
#define WSAENOTSOCK         EBADF
#define INVALID_SOCKET      ((SOCKET)(~0))
#define SOCKET_ERROR        (-1)
#define MAX_PATH            1024
#endif

#if !defined(MSG_NOSIGNAL)
#define MSG_NOSIGNAL 0
#endif

static inline bool is_selectable_socket(SOCKET s)
{
#ifdef _WIN32
    (void)s;
    return true;
#else
    return s < FD_SETSIZE;
#endif
}

#endif
