# ZClassic C23 Full Node
# Copyright 2026 Rhett Creighton - Apache License 2.0

CC = cc

MODULES = bloom chain coins consensus core crypto db encoding init json \
	keys metrics mining net policy primitives rpc script storage \
	support util validation wallet zcash

MOD_INCLUDES = $(foreach m,$(MODULES),-Imodules/$(m)/include)

CFLAGS = -std=c23 -Wall -Wextra -Werror -pedantic $(MOD_INCLUDES) \
	-D_POSIX_C_SOURCE=200809L -Ivendor/include
LDFLAGS = -pthread
LIBS = -Lvendor/lib -lsecp256k1 -lleveldb -lrustzcash \
	-lstdc++ -lm -lsqlite3 -ldl -lpthread

ALL_SRCS = $(foreach m,$(MODULES),$(wildcard modules/$(m)/src/*.c))
ALL_OBJS = $(ALL_SRCS:.c=.o)

.PHONY: all test clean

all: test_zcl zcld

test_zcl: modules/test/src/test.c $(ALL_SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

zcld: main.c $(ALL_SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

test: test_zcl
	./test_zcl

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f test_zcl zcld $(ALL_OBJS)
