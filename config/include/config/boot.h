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
    int https_port;
    int fs_port;
    const char *rpc_user;
    const char *rpc_password;
    bool listen;
    bool tx_index;
    bool checkpoints_enabled;
    const char *import_legacy_dir;
    bool sapling_scan;
    const char *legacy_import_dir;
    /* -importfromlegacy=PATH : read-only ingest of a co-located zclassicd
     * datadir via local_chain_ingest (FS4). Validates every block against
     * the hardcoded SHA3 windows + UTXO checkpoint instead of trusting
     * the source. Doesn't require zclassicd to be stopped (read-only).
     * Different from -import-from= (legacy_import_dir above) which byte-
     * copies the entire datadir and needs zclassicd offline. */
    const char *ingest_from_legacy;
    /* -bodypull-from-legacy=PATH : run JUST the legacy body-pull post-
     * boot, skipping local_chain_ingest's phase 1 (SHA3 verify) and
     * phase 2 (chainstate import — would clobber our UTXOs). Useful
     * when the local tip lags the sibling zclassicd and you want a
     * one-shot catch-up without touching SHA3 anchors. Bare form
     * (-bodypull-from-legacy) defaults to ~/.zclassic. The path is
     * only used to confirm the legacy datadir exists; RPC creds come
     * from ~/.zclassic/zclassic.conf in either case. */
    const char *bodypull_from_legacy;
    /* -fastimport[=PATH] : direct LevelDB+mmap import from a sibling
     * zclassicd's blocks/. Bypasses JSON-RPC entirely — reads the
     * legacy node's blocks/index/ LevelDB to build a height-ordered
     * map, then mmap()s the blk*.dat files and feeds payloads to
     * process_new_block. Requires the legacy LevelDB to be unlocked
     * (stop zclassicd first). Bare form defaults to ~/.zclassic. */
    const char *fastimport_from;
    const char *snapshot_dir;
    bool reindex_chainstate;
    bool reimport_utxos;
    bool tor;
    const char *assume_valid;  /* block hash: skip Groth16 at/below this height */
    bool no_services;          /* skip P2P, RPC, Tor — boot only (speedrun) */
    const char *file_service_peer; /* -fileservice=addr : download from this peer */
    bool connect_only;         /* -connect= mode: only connect to addnodes, no seeds */
    bool no_file_sync;         /* -nofilesync : skip file service download, use P2P only */
    bool no_bg_validation;     /* -nobgvalidation : skip background proof verification */
    const char *external_ip;   /* -externalip=IP : advertise this address to peers */
    bool allow_degraded;       /* -allow-degraded : continue past failed post-restore integrity check
                                 * (default false → boot FATALs on broken chain state). */
};

void app_context_defaults(struct app_context *ctx);

bool app_init(struct app_context *ctx);
void app_shutdown(void);

bool app_is_running(void);
void app_add_node(const char *host, int port);
void app_start_metrics(bool mining);
void app_stop_metrics(void);

/* Background UTXO replay status (after fast file sync).
 * Node is usable immediately; replay builds UTXO set in background. */
#include <stdatomic.h>
extern _Atomic bool g_utxo_replay_active;
extern _Atomic int  g_utxo_replay_height;

#endif
