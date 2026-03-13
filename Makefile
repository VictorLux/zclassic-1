# ZClassic C23 Full Node
# Copyright 2026 Rhett Creighton - Apache License 2.0

CC = cc

# App layer (MVC)
APP_DIRS = models controllers views
APP_INCLUDES = $(foreach d,$(APP_DIRS),-Iapp/$(d)/include)
APP_SRCS = $(foreach d,$(APP_DIRS),$(wildcard app/$(d)/src/*.c))

# Config layer
CONFIG_INCLUDES = -Iconfig/include
CONFIG_SRCS = $(wildcard config/src/*.c)

# Library layer
LIB_MODULES = bloom chain coins consensus core crypto encoding json \
	keys metrics mining net policy primitives rpc script storage \
	support util validation wallet zcash
LIB_INCLUDES = $(foreach m,$(LIB_MODULES),-Ilib/$(m)/include)
LIB_SRCS = $(foreach m,$(LIB_MODULES),$(wildcard lib/$(m)/src/*.c))

ALL_SRCS = $(APP_SRCS) $(CONFIG_SRCS) $(LIB_SRCS)
ALL_OBJS = $(ALL_SRCS:.c=.o)

CFLAGS = -std=c23 -Wall -Wextra -Werror -pedantic \
	$(APP_INCLUDES) $(CONFIG_INCLUDES) $(LIB_INCLUDES) \
	-D_POSIX_C_SOURCE=200809L -Ivendor/include
LDFLAGS = -pthread
LIBS = -Lvendor/lib -lsecp256k1 -lleveldb -lrustzcash \
	-lstdc++ -lm -lsqlite3 -ldl -lpthread

.PHONY: all test clean

all: test_zcl zcld

test_zcl: lib/test/src/test.c $(ALL_SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

zcld: main.c $(ALL_SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

test: test_zcl
	./test_zcl

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f test_zcl zcld $(ALL_OBJS)
