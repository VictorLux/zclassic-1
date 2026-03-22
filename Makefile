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
LIB_MODULES = bloom chain coins consensus core crypto encoding event json \
	keys metrics mining net policy primitives rpc script storage \
	support util validation wallet sapling zslp
LIB_INCLUDES = $(foreach m,$(LIB_MODULES),-Ilib/$(m)/include)
LIB_SRCS = $(foreach m,$(LIB_MODULES),$(wildcard lib/$(m)/src/*.c))

ALL_SRCS = $(APP_SRCS) $(CONFIG_SRCS) $(LIB_SRCS)
ALL_OBJS = $(ALL_SRCS:.c=.o)

GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 2>/dev/null)
GTK_LIBS   := $(shell pkg-config --libs gtk+-3.0 2>/dev/null)
GTK_DEF    := $(if $(GTK_CFLAGS),-DHAVE_GTK,)
WEBKIT_CFLAGS := $(shell pkg-config --cflags webkit2gtk-4.1 2>/dev/null)
WEBKIT_LIBS   := $(shell pkg-config --libs webkit2gtk-4.1 2>/dev/null)
WEBKIT_DEF    := $(if $(WEBKIT_CFLAGS),-DHAVE_WEBKIT,)

CFLAGS = -std=c23 -O3 -march=native -flto -Wall -Wextra -Werror -pedantic \
	-Wno-stringop-overflow -Wno-unused-result \
	$(APP_INCLUDES) $(CONFIG_INCLUDES) $(LIB_INCLUDES) \
	-Ilib/test/include \
	-D_POSIX_C_SOURCE=200809L -Ivendor/include $(GTK_DEF) $(GTK_CFLAGS) \
	$(WEBKIT_DEF) $(WEBKIT_CFLAGS)
LDFLAGS = -pthread -flto
# Use vendor/tor/libtor.a when Tor is built from source.
# Tor: use full Tor if built, otherwise fall back to stub.
TOR_FULL = $(wildcard vendor/tor/libtor.a \
	vendor/tor/src/ext/ed25519/donna/libed25519_donna.a \
	vendor/tor/src/ext/ed25519/ref10/libed25519_ref10.a \
	vendor/tor/src/ext/keccak-tiny/libkeccak-tiny.a)
TOR_LIBS = $(if $(TOR_FULL),$(TOR_FULL),-Lvendor/lib -ltor_stub)
# All dependencies bundled in vendor/lib as static archives.
# Zero system library requirements beyond libc.
# OpenSSL 3.0 (Apache 2.0), libevent, zlib — all vendored.
LIBS = -Lvendor/lib -lsecp256k1 -lleveldb \
	-lstdc++ -lm -lsqlite3 -ldl -lpthread \
	-levent -levent_openssl -levent_pthreads \
	-lssl -lcrypto -lz \
	-Wl,--allow-multiple-definition

.PHONY: all test clean deploy

CLI_SRCS = lib/rpc/src/client.c lib/json/src/json.c
all: test_zcl zclassic23 zclassic-cli

TEST_SRCS = $(wildcard lib/test/src/*.c)

test_zcl: $(TEST_SRCS) $(ALL_SRCS)
	$(CC) $(CFLAGS) -Wno-deprecated-declarations $(LDFLAGS) -o $@ $^ $(TOR_LIBS) $(LIBS)

zclassic23: main.c $(ALL_SRCS)
	$(CC) $(CFLAGS) -Wno-deprecated-declarations $(LDFLAGS) -o $@ $^ $(TOR_LIBS) $(LIBS) $(GTK_LIBS) $(WEBKIT_LIBS)

zclassic-cli: cli.c $(CLI_SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ -lm

zcl-rpc: tools/zcl-rpc.c
	$(CC) -std=c23 -O2 -Wall -o $@ $<

zcl-browser: tools/zcl-browser.c $(ALL_SRCS)
	$(CC) $(CFLAGS) -Wno-deprecated-declarations $$(pkg-config --cflags webkit2gtk-4.1) -o $@ $^ $(TOR_LIBS) $(LIBS) $$(pkg-config --libs webkit2gtk-4.1)

zcl-blog: tools/zcl-blog
	$(CC) -std=c23 -O2 -x c $$(pkg-config --cflags webkit2gtk-4.1) -o $@ $< $$(pkg-config --libs webkit2gtk-4.1)

explorer-css: app/views/src/explorer_css.css
	python3 -c "\
	import re; f=open('app/views/src/explorer_css.css'); css=f.read(); f.close(); \
	css=re.sub(r'/\*.*?\*/', '', css, flags=re.DOTALL); \
	css=re.sub(r'\s+', ' ', css).strip(); css=re.sub(r'\s*([{}:;,])\s*', r'\1', css); \
	css=css.replace('\\\\','\\\\\\\\').replace('\"','\\\\\"'); \
	lines=[]; i=0; \
	exec('while i<len(css): lines.append(chr(32)*4+chr(34)+css[i:min(i+100,len(css))]+chr(34)); i+=100'); \
	o=open('app/views/include/views/explorer_css.h','w'); \
	o.write('/* Auto-generated from app/views/src/explorer_css.css */\n'); \
	o.write('#ifndef EXPLORER_CSS_H\n#define EXPLORER_CSS_H\n\n'); \
	o.write('static const char explorer_css[] =\n'+'\n'.join(lines)+';\n\n#endif\n'); o.close()"

test: test_zcl
	ulimit -s unlimited && ./test_zcl

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Deploy: setcap (passwordless after setup.sh), install service, restart
deploy: zclassic23
	sudo /usr/sbin/setcap 'cap_net_bind_service=+ep' $(CURDIR)/zclassic23
	@install -m 644 deploy/zclassic23.service $(HOME)/.config/systemd/user/zclassic23.service
	@systemctl --user daemon-reload
	systemctl --user restart zclassic23
	@sleep 2 && systemctl --user is-active zclassic23 && echo "Deployed."

clean:
	rm -f test_zcl zclassic23 zclassic-cli $(ALL_OBJS)
