# Pure C23 build for ZClassic
# Copyright 2026 Rhett Creighton - Apache License 2.0

CC = cc
CFLAGS = -std=c23 -Wall -Wextra -Werror -pedantic -I. -D_POSIX_C_SOURCE=200809L -I../secp256k1/include
LDFLAGS = -pthread
LIBS = -L../secp256k1/.libs -lsecp256k1 -lm

CRYPTO_SRCS = \
	crypto/sha256.c crypto/sha512.c crypto/sha1.c \
	crypto/ripemd160.c crypto/hmac_sha256.c crypto/hmac_sha512.c \
	crypto/sha3.c crypto/blake2b.c

CORE_SRCS = \
	core/uint256.c core/arith_uint256.c core/hash.c core/amount.c \
	core/serialize.c core/random.c core/utiltime.c

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
	net/p2p_message.c net/addrman.c net/net.c

CHAIN_SRCS = \
	chain/chain.c chain/pow.c chain/checkpoints.c \
	chain/chainparams.c chain/chainparamsbase.c chain/subsidy.c

KEYS_SRCS = \
	keys/pubkey.c keys/key.c keys/key_io.c

VALIDATION_SRCS = \
	validation/check_transaction.c validation/contextual_check_tx.c \
	validation/sigops.c validation/sighash.c validation/tx_verifier.c \
	validation/validationinterface.c validation/txmempool.c

SUPPORT_SRCS = \
	support/cleanse.c support/pagelocker.c

BLOOM_SRCS = \
	bloom/bloom.c bloom/merkle.c bloom/merkleblock.c

COINS_SRCS = \
	coins/coins.c coins/compressor.c

UTIL_SRCS = \
	util/util.c util/scheduler.c util/sync.c util/timedata.c \
	util/deprecation.c util/clientversion.c \
	util/ui_interface.c util/noui.c

ALL_SRCS = $(CRYPTO_SRCS) $(CORE_SRCS) $(ENCODING_SRCS) $(SCRIPT_SRCS) \
	$(CONSENSUS_SRCS) $(PRIMITIVES_SRCS) $(NET_SRCS) $(CHAIN_SRCS) \
	$(KEYS_SRCS) $(VALIDATION_SRCS) $(SUPPORT_SRCS) $(BLOOM_SRCS) \
	$(COINS_SRCS) $(UTIL_SRCS)

ALL_OBJS = $(ALL_SRCS:.c=.o)

.PHONY: all test clean

all: test_zcl

test_zcl: test/test.c $(ALL_SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

test: test_zcl
	./test_zcl

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f test_zcl $(ALL_OBJS)
