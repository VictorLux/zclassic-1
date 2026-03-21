/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Shared test infrastructure for ZClassic C23 test suite. */

#ifndef TEST_HELPERS_H
#define TEST_HELPERS_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/time.h>
#include <sys/stat.h>

#include "crypto/sha256.h"
#include "crypto/sha512.h"
#include "crypto/sha1.h"
#include "crypto/ripemd160.h"
#include "crypto/hmac_sha256.h"
#include "crypto/hmac_sha512.h"
#include "crypto/sha3.h"
#include "net/secure_channel.h"
#include "crypto/blake2b.h"
#include "core/uint256.h"
#include "core/hash.h"
#include "encoding/base58.h"
#include "encoding/bech32.h"
#include "core/arith_uint256.h"
#include "core/random.h"
#include "core/utiltime.h"
#include "consensus/params.h"
#include "consensus/upgrades.h"
#include "encoding/utilmoneystr.h"
#include "encoding/utilstrencodings.h"
#include "util/clientversion.h"
#include "chain/chainparamsbase.h"
#include "util/util.h"
#include "util/ui_interface.h"
#include "util/noui.h"
#include "util/deprecation.h"
#include "util/timedata.h"
#include "net/netaddr.h"
#include "net/protocol.h"
#include "chain/pow.h"
#include "chain/checkpoints.h"
#include "keys/pubkey.h"
#include "keys/key.h"
#include "script/script.h"
#include "coins/compressor.h"
#include "script/standard.h"
#include "primitives/transaction.h"
#include "bloom/bloom.h"
#include "bloom/merkle.h"
#include "script/sighashtype.h"
#include "coins/coins.h"
#include "core/serialize.h"
#include "primitives/block.h"
#include "script/sigencoding.h"
#include "support/pagelocker.h"
#include "script/interpreter.h"
#include "script/sigcache.h"
#include "consensus/validation.h"
#include "keys/key_io.h"
#include "chain/chainparams.h"
#include "chain/subsidy.h"
#include "validation/sighash.h"
#include "validation/check_transaction.h"
#include "validation/tx_verifier.h"
#include "validation/sigops.h"
#include "validation/contextual_check_tx.h"
#include "coins/undo.h"
#include "net/p2p_message.h"
#include "net/netbase.h"
#include "bloom/merkleblock.h"
#include "script/zcashconsensus.h"
#include "validation/validationinterface.h"
#include "net/addrman.h"
#include "net/net.h"
#include "validation/txmempool.h"
#include "policy/fees.h"
#include "json/json.h"
#include "rpc/server.h"
#include "rpc/client.h"
#include "storage/dbwrapper.h"
#include "core/core_io.h"
#include "rpc/async_rpc_operation.h"
#include "rpc/async_rpc_queue.h"
#include "validation/chainstate.h"
#include "validation/main_constants.h"
#include "storage/txdb.h"
#include "storage/disk_block_io.h"
#include "validation/main_state.h"
#include "validation/main_logic.h"
#include "validation/checkqueue.h"
#include "coins/coins_view.h"
#include "storage/coins_db.h"
#include "validation/update_coins.h"
#include "storage/block_index_db.h"
#include "crypto/equihash.h"
#include "crypto/equihash_solver.h"
#include "sapling/constants.h"
#include "sapling/jubjub.h"
#include "sapling/prf.h"
#include "sapling/incremental_merkle_tree.h"
#include "sapling/pedersen_hash.h"
#include "chain/equihash.h"
#include "validation/check_block.h"
#include "sapling/address.h"
#include "sapling/note.h"
#include "crypto/chacha20poly1305.h"
#include "crypto/curve25519.h"
#include "sapling/note_encryption.h"
#include "sapling/fr.h"
#include "crypto/blake2s.h"
#include "sapling/pedersen_hash.h"
#include "sapling/sapling.h"
#include "sapling/bls12_381.h"
#include "crypto/aes256.h"
#include "sapling/ff1.h"
#include "sapling/zip32.h"
#include "sapling/sprout.h"
#include "sapling/params_init.h"
#include "crypto/ed25519.h"
#include "wallet/wallet.h"
#include "models/database.h"
#include "models/block.h"
#include "models/tx_index.h"
#include "models/utxo.h"
#include "models/wallet_key.h"
#include "models/wallet_tx.h"
#include "models/mempool_entry.h"
#include "models/peer.h"
#include "models/block_data.h"
#include "models/block_index_store.h"
#include "models/chainstate_store.h"
#include "models/chain_snapshot.h"
#include "net/connman.h"
#include "net/tor_integration.h"

/* Shared helper functions */
int check_hex(const unsigned char *data, size_t len, const char *expected);
void test_hex_to_bytes(const char *hex, uint8_t *out, int len);
void test_hex_to_bytes_rev(const char *hex, uint8_t *out, int len);

/* Test group functions — each returns failure count */
int test_crypto(void);
int test_encoding(void);
int test_sapling(void);
int test_script(void);
int test_chain(void);
int test_sqlite(void);
int test_keys(void);
int test_mempool(void);
int test_rpc(void);
int test_transaction(void);
int test_net(void);
int test_activerecord(void);
int test_sapling_crypto(void);
int test_merkle_tree(void);
int test_slp(void);
int test_models(void);
int test_core(void);
int test_json(void);
int test_validation(void);
int test_wallet(void);
int test_primitives(void);
int test_bloom(void);
int test_coins(void);
int test_tor(void);
int test_load_balancer(void);
int test_game(void);
int test_store(void);
int test_blog(void);
int test_robustness(void);
int test_api(void);
int test_explorer(void);
int test_mining(void);
int test_utxo_commitment(void);
int test_scan_util(void);
int test_event(void);

/* ── DRY test macros ─────────────────────────────────────── */

/* Run a named test. Usage:
 *   TEST("my test name") {
 *       bool ok = (1 + 1 == 2);
 *       ASSERT(ok);
 *   }
 * Automatically prints name, OK/FAIL, tracks failure count.
 * Requires `int failures = 0;` in scope. */

#define TEST(name) \
    for (int _t_once = (printf("%s... ", name), 1); _t_once; _t_once = 0)

#define ASSERT(cond) do { \
    if (!(cond)) { printf("FAIL (%s)\n", #cond); failures++; goto _test_next; } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { printf("FAIL (%s != %s)\n", #a, #b); failures++; goto _test_next; } \
} while(0)

#define ASSERT_STR_EQ(a, b) do { \
    if (strcmp((a), (b)) != 0) { printf("FAIL (\"%s\" != \"%s\")\n", (a), (b)); failures++; goto _test_next; } \
} while(0)

#define PASS() do { printf("OK\n"); } while(0)

/* Block-scoped test with automatic PASS at end and goto label.
 * Usage:
 *   TEST_CASE("name") {
 *       ASSERT(condition);
 *       // if we reach here, test passed
 *   } TEST_END */
#define TEST_CASE(name) \
    printf("%s... ", name); \
    {

#define TEST_END \
        printf("OK\n"); \
    } \
    if (0) { _test_next: ; }

#endif /* TEST_HELPERS_H */
