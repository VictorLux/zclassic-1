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
	support util validation wallet sapling
LIB_INCLUDES = $(foreach m,$(LIB_MODULES),-Ilib/$(m)/include)
LIB_SRCS = $(foreach m,$(LIB_MODULES),$(wildcard lib/$(m)/src/*.c))

ALL_SRCS = $(APP_SRCS) $(CONFIG_SRCS) $(LIB_SRCS)
ALL_OBJS = $(ALL_SRCS:.c=.o)

CFLAGS = -std=c23 -O3 -march=native -flto -Wall -Wextra -Werror -pedantic \
	-Wno-stringop-overflow \
	$(APP_INCLUDES) $(CONFIG_INCLUDES) $(LIB_INCLUDES) \
	-Ilib/test/include \
	-D_POSIX_C_SOURCE=200809L -Ivendor/include
LDFLAGS = -pthread -flto
LIBS = -Lvendor/lib -lsecp256k1 -lleveldb \
	-lstdc++ -lm -lsqlite3 -ldl -lpthread

.PHONY: all test clean

CLI_SRCS = lib/rpc/src/client.c lib/json/src/json.c
all: test_zcl zclassic23 zclassic-cli

TEST_SRCS = $(wildcard lib/test/src/*.c)

test_zcl: $(TEST_SRCS) $(ALL_SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

zclassic23: main.c $(ALL_SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

zclassic-cli: cli.c $(CLI_SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ -lm

zcl-blog: tools/zcl-blog
	$(CC) -std=c23 -O2 -x c $$(pkg-config --cflags webkit2gtk-4.1) -o $@ $< $$(pkg-config --libs webkit2gtk-4.1)

test: test_zcl
	./test_zcl

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f test_zcl zclassic23 zclassic-cli $(ALL_OBJS)

hodl_chart: tools/hodl_chart.c lib/util/src/png_writer.c lib/util/src/bitmap_font.c
	$(CC) -std=c23 -O2 -Wall -Wextra -Wno-pedantic -Ilib/util/include -o $@ $^ -lm
