# Pure C23 build for ZClassic
# Copyright 2026 Rhett Creighton - Apache License 2.0

CC = cc
CFLAGS = -std=c23 -Wall -Wextra -Werror -pedantic -I. -D_POSIX_C_SOURCE=200809L -I../secp256k1/include -I../leveldb/include
LDFLAGS = -pthread
LIBS = -L../secp256k1/.libs -lsecp256k1 -L../leveldb -lleveldb -lstdc++ -lm

CRYPTO_SRCS = \
	crypto/sha256.c crypto/sha512.c crypto/sha1.c \
	crypto/ripemd160.c crypto/hmac_sha256.c crypto/hmac_sha512.c \
	crypto/sha3.c crypto/blake2b.c crypto/equihash.c \
	crypto/chacha20poly1305.c crypto/curve25519.c crypto/blake2s.c \
	crypto/aes256.c crypto/ed25519.c

CORE_SRCS = \
	core/uint256.c core/arith_uint256.c core/hash.c core/amount.c \
	core/serialize.c core/random.c core/utiltime.c core/core_io.c

ENCODING_SRCS = \
	encoding/base58.c encoding/bech32.c \
	encoding/utilstrencodings.c encoding/utilmoneystr.c

SCRIPT_SRCS = \
	script/script_error.c script/script.c script/standard.c \
	script/sigencoding.c script/interpreter.c script/sigcache.c \
	script/zcashconsensus.c

CONSENSUS_SRCS = \
	consensus/params.c consensus/upgrades.c

PRIMITIVES_SRCS = \
	primitives/transaction.c primitives/block.c

NET_SRCS = \
	net/netaddr.c net/netbase.c net/protocol.c \
	net/p2p_message.c net/addrman.c net/net.c net/msgprocessor.c

CHAIN_SRCS = \
	chain/chain.c chain/pow.c chain/checkpoints.c \
	chain/chainparams.c chain/chainparamsbase.c chain/subsidy.c \
	chain/equihash.c

KEYS_SRCS = \
	keys/pubkey.c keys/key.c keys/key_io.c

VALIDATION_SRCS = \
	validation/check_transaction.c validation/contextual_check_tx.c \
	validation/sigops.c validation/sighash.c validation/tx_verifier.c \
	validation/check_block.c \
	validation/validationinterface.c validation/txmempool.c \
	validation/chainstate.c validation/checkqueue.c \
	validation/update_coins.c validation/connect_block.c \
	validation/process_block.c

SUPPORT_SRCS = \
	support/cleanse.c support/pagelocker.c

BLOOM_SRCS = \
	bloom/bloom.c bloom/merkle.c bloom/merkleblock.c

COINS_SRCS = \
	coins/coins.c coins/compressor.c coins/coins_view.c

UTIL_SRCS = \
	util/util.c util/scheduler.c util/sync.c util/timedata.c \
	util/deprecation.c util/clientversion.c \
	util/ui_interface.c util/noui.c

JSON_SRCS = \
	json/json.c

RPC_SRCS = \
	rpc/protocol.c rpc/server.c rpc/client.c \
	rpc/async_rpc_operation.c rpc/async_rpc_queue.c \
	rpc/blockchain.c rpc/rawtransaction.c rpc/mining_rpc.c

STORAGE_SRCS = \
	storage/dbwrapper.c storage/txdb.c storage/disk_block_io.c \
	storage/coins_db.c storage/block_index_db.c

POLICY_SRCS = \
	policy/fees.c

MINING_SRCS = \
	mining/miner.c

INIT_SRCS = \
	init/init.c

ZCASH_SRCS = \
	zcash/jubjub.c zcash/prf.c zcash/incremental_merkle_tree.c \
	zcash/address.c zcash/note.c zcash/note_encryption.c \
	zcash/fr.c zcash/pedersen_hash.c zcash/sapling.c zcash/bls12_381.c \
	zcash/ff1.c zcash/zip32.c zcash/sprout.c zcash/params_init.c

ALL_SRCS = $(CRYPTO_SRCS) $(CORE_SRCS) $(ENCODING_SRCS) $(SCRIPT_SRCS) \
	$(CONSENSUS_SRCS) $(PRIMITIVES_SRCS) $(NET_SRCS) $(CHAIN_SRCS) \
	$(KEYS_SRCS) $(VALIDATION_SRCS) $(SUPPORT_SRCS) $(BLOOM_SRCS) \
	$(COINS_SRCS) $(UTIL_SRCS) $(POLICY_SRCS) $(JSON_SRCS) $(RPC_SRCS) \
	$(STORAGE_SRCS) $(MINING_SRCS) $(INIT_SRCS) $(ZCASH_SRCS)

ALL_OBJS = $(ALL_SRCS:.c=.o)

.PHONY: all test clean

all: test_zcl zcld

test_zcl: test/test.c $(ALL_SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

zcld: main.c $(ALL_SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

test: test_zcl
	./test_zcl

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f test_zcl $(ALL_OBJS)
