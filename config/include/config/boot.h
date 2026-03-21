/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_INIT_H
#define ZCL_INIT_H

#include <stdbool.h>

struct app_context {
    const char *datadir;
    const char *params_dir;
    bool testnet;
    bool regtest;
    bool daemon;
    bool gen;
    int gen_threads;
    const char *miner_address;
    int rpc_port;
    int p2p_port;
    const char *rpc_user;
    const char *rpc_password;
    bool listen;
    bool tx_index;
    bool checkpoints_enabled;
    const char *import_legacy_dir;
    bool sapling_scan;
    const char *fastsync_dir;
    const char *snapshot_dir;
    bool reindex_chainstate;
    bool tor;
    const char *assume_valid;  /* block hash: skip Groth16 at/below this height */
};

void app_context_defaults(struct app_context *ctx);

bool app_init(struct app_context *ctx);
void app_shutdown(void);

bool app_is_running(void);
void app_add_node(const char *host, int port);
void app_start_metrics(bool mining);
void app_stop_metrics(void);

#endif
