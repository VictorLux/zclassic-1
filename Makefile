# ZClassic C23 Full Node
# Copyright 2026 Rhett Creighton - Apache License 2.0

CC = cc
BUILD_COMMIT := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)

# App layer (MVC)
APP_DIRS = models controllers views services supervisors conditions jobs
APP_INCLUDES = $(foreach d,$(APP_DIRS),-Iapp/$(d)/include)
APP_SRCS = $(foreach d,$(APP_DIRS),$(wildcard app/$(d)/src/*.c))

# Config layer
CONFIG_INCLUDES = -Iconfig/include
CONFIG_SRCS = $(wildcard config/src/*.c)

# Library layer
LIB_MODULES = bloom chain coins consensus core crypto crypto_registry encoding event framework health kernel \
	json keys metrics mining net platform policy primitives rpc script sim storage \
	support sync util validation wallet sapling zslp znam
LIB_INCLUDES = $(foreach m,$(LIB_MODULES),-Ilib/$(m)/include)
LIB_SRCS = $(foreach m,$(LIB_MODULES),$(wildcard lib/$(m)/src/*.c))

# Ports layer (Clean Architecture / Hexagonal interface headers).
# Headers only — adapters that implement these interfaces live elsewhere.
# See ports/include/ports/README.md for the convention.
PORTS_INCLUDES = -Iports/include

# Domain layer (pure, framework-free, no I/O).
# Bounded contexts under domain/<context>/ each expose include/domain/<context>/.
DOMAIN_CONTEXTS = consensus wallet encoding
DOMAIN_INCLUDES = $(foreach c,$(DOMAIN_CONTEXTS),-Idomain/$(c)/include)
DOMAIN_SRCS = $(foreach c,$(DOMAIN_CONTEXTS),$(wildcard domain/$(c)/src/*.c))

# Application layer (use cases / service objects).
# May depend on domain/, ports/, primitives, util — never on adapters or I/O.
APPLICATION_CONTEXTS = consensus
APPLICATION_INCLUDES = $(foreach c,$(APPLICATION_CONTEXTS),-Iapplication/$(c)/include)
APPLICATION_SRCS = $(foreach c,$(APPLICATION_CONTEXTS),$(wildcard application/$(c)/src/*.c))

# Adapters layer (port implementations).
# Outbound adapters implement the port interfaces. Inbound surfaces currently
# live in app/controllers, tools/mcp, and tools/cli until a real adapter shape
# is introduced.
ADAPTERS_INCLUDES = -Iadapters/outbound/persistence/include
ADAPTERS_SRCS = $(wildcard adapters/outbound/persistence/src/*.c)

# MCP router + future controllers (schema-driven tool dispatch)
MCP_INCLUDES = -Itools
MCP_SRCS = $(wildcard tools/mcp/*.c) $(wildcard tools/mcp/controllers/*.c) \
	$(wildcard tools/mcp/views/*.c)

ALL_SRCS = $(APP_SRCS) $(CONFIG_SRCS) $(LIB_SRCS) $(DOMAIN_SRCS) $(APPLICATION_SRCS) $(ADAPTERS_SRCS) $(MCP_SRCS)
ALL_OBJS = $(ALL_SRCS:.c=.o)

GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 2>/dev/null)
GTK_LIBS   := $(shell pkg-config --libs gtk+-3.0 2>/dev/null)
GTK_DEF    := $(if $(GTK_CFLAGS),-DHAVE_GTK,)
WEBKIT_CFLAGS := $(shell pkg-config --cflags webkit2gtk-4.1 2>/dev/null)
WEBKIT_LIBS   := $(shell pkg-config --libs webkit2gtk-4.1 2>/dev/null)
WEBKIT_DEF    := $(if $(WEBKIT_CFLAGS),-DHAVE_WEBKIT,)

CFLAGS = -std=c23 -O3 -march=native -flto=auto -Wall -Wextra -Werror -pedantic \
	-Wno-stringop-overflow -Wno-unused-result \
	$(APP_INCLUDES) $(CONFIG_INCLUDES) $(LIB_INCLUDES) $(PORTS_INCLUDES) $(DOMAIN_INCLUDES) $(APPLICATION_INCLUDES) $(ADAPTERS_INCLUDES) $(MCP_INCLUDES) \
	-Ilib/test/include \
	-D_POSIX_C_SOURCE=200809L -DZCL_AR_ENFORCE -DZCL_BUILD_COMMIT=\"$(BUILD_COMMIT)\" -Ivendor/include $(GTK_DEF) $(GTK_CFLAGS) \
	$(WEBKIT_DEF) $(WEBKIT_CFLAGS)
LDFLAGS = -pthread -flto=auto -rdynamic
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
	-lssl -lcrypto -lz

.PHONY: all test test-e2e test-shielded-payment test-store-e2e clean deploy check-restart-follow \
        coverage coverage-clean docs-mcp docs-mcp-check ci audit release \
        bench bench-regress \
        lint check-malloc check-silent-errors check-raw-sqlite check-raw-malloc \
        check-coins-lookup-nullcheck check-observability-pairing \
        check-silent-errors-services check-silent-errors-controllers \
        check-before-save-hooks check-pthread-create check-model-validation \
        check-long-functions check-rpc-registrar check-lag-slo-observable \
        check-file-size-ceiling check-operator-needed-sink check-doc-accuracy \
        fuzz-ci-leaks \
        soak-smoke soak-7day chaos chaos-clean

CLI_SRCS = lib/rpc/src/client.c lib/json/src/json.c
all: test_zcl zclassic23 zclassic-cli

TEST_SRCS = $(wildcard lib/test/src/*.c)
SPEC_SRCS = $(wildcard lib/test/spec/*.c)

# test.c and test_parallel.c each own their own main() — never both in
# one binary. test_parallel_zcl uses the latter + the same test/spec
# helpers as sequential test_zcl.
TEST_SRCS_NO_MAIN = $(filter-out lib/test/src/test.c lib/test/src/test_parallel.c, $(TEST_SRCS))

# Generate templates from .chtml and .ccss files
TMPL_GEN = app/views/include/views/wallet_templates_gen.h
TMPL_SRC = $(wildcard app/views/templates/*.chtml) $(wildcard app/views/css/*.ccss)
TMPL_TOOL = tools/gen_templates

$(TMPL_TOOL): tools/gen_templates.c lib/util/src/safe_alloc.c
	$(CC) -std=c23 -O2 -Wall -Wextra -Ilib/util/include -o $@ $^

tools/inspect_html: tools/inspect_html.c
	$(CC) -std=c23 -O2 -Wall -Wextra -o $@ $<

$(TMPL_GEN): $(TMPL_SRC) $(TMPL_TOOL)
	./$(TMPL_TOOL) app/views/templates $@ app/views/css

.PHONY: templates
templates: $(TMPL_GEN)

# Build a tool/test binary that links against the full node library stack
# (Tor, OpenSSL, libevent, GTK, WebKit). Used by 8 binaries to keep the
# recipe in one place — a new tool becomes one $(eval $(call ...)) line and
# cannot drift on flags.
#   $(1) = target name (e.g., wallet_dump)
#   $(2) = entry source(s) — single file or whitespace-separated list
#   $(3) = extra link libs (e.g., -lm); empty by default
#   $(4) = extra CFLAGS (e.g., -DZCL_TESTING); empty by default
define BUILD_NODE_TOOL
$(1): $$(TMPL_GEN) $(2) $$(ALL_SRCS)
	$$(CC) $$(CFLAGS) $(4) -Wno-deprecated-declarations $$(LDFLAGS) -o $$@ $$(filter-out $$(TMPL_GEN),$$^) $$(TOR_LIBS) $$(LIBS) $$(GTK_LIBS) $$(WEBKIT_LIBS) $(3)
endef

CHAOS_SIM_SRCS = tools/sim/sim_peer.c

$(eval $(call BUILD_NODE_TOOL,test_zcl,$(TEST_SRCS_NO_MAIN) lib/test/src/test.c $(SPEC_SRCS) $(CHAOS_SIM_SRCS),,-DZCL_TESTING))
$(eval $(call BUILD_NODE_TOOL,test_parallel,$(TEST_SRCS_NO_MAIN) lib/test/src/test_parallel.c $(SPEC_SRCS) $(CHAOS_SIM_SRCS),,-DZCL_TESTING))

.PHONY: test-parallel
test-parallel: test_parallel
	ulimit -s unlimited && ./test_parallel

# ── Fast inner loop ──────────────────────────────────────────────────────
# The edit -> check -> test loop runs dozens of times per session. Use these,
# NOT `make` + `./test_zcl` (8-15 min) and NOT a bare `./test_parallel`.
#
# THE REBUILD TRAP: plain `make` does NOT rebuild test_parallel (it is not in
# the default `all`), so running ./test_parallel directly after editing a test
# can false-green an old binary or report "matched no groups" for a new test.
# `make t ONLY=<group>` always rebuilds the harness first, closing that trap.
.PHONY: t syntax-check build-only lint-fast

# Run ONE test group, always rebuilding the harness first:
#   make t ONLY=service_state_driver
t: test_parallel
	@if [ -z "$(ONLY)" ]; then \
	  echo "usage: make t ONLY=<group-substr>   (e.g. make t ONLY=stage_reducer_unwedge)"; \
	  exit 2; fi
	ulimit -s unlimited && ./test_parallel --only=$(ONLY)

# Incremental compile-check of the whole node (no link). Only changed TUs
# recompile — the fastest "does my change still build" signal.
build-only: $(TMPL_GEN) $(ALL_OBJS)
	@echo "build-only: all node objects compiled"

# Full no-link syntax check across every TU in one shot (no incremental state).
syntax-check: $(TMPL_GEN)
	@$(CC) $(CFLAGS) -fsyntax-only $(ALL_SRCS) main.c && echo "syntax-check: OK"

# The highest-signal lint gates for the inner loop. Run full `make lint` at
# sub-wave boundaries / before commit.
lint-fast: check-raw-sqlite check-malloc check-silent-errors check-model-validation check-one-write-path
	@echo "lint-fast: OK"

# ── Live-truth diagnosis + safe reproduction ─────────────────────────────
# diagnose-gap: one-shot three-orthogonal-views dump + root-cause verdict over
#   the RUNNING node (live or a repro copy). LIVE TRUTH BEFORE DESIGN.
#     make diagnose-gap SLUG=mystall
#     ZCL_RPCPORT=18299 ZCL_DATADIR=<copy> make diagnose-gap SLUG=onacopy
# repro-on-copy: snapshot the live datadir to a throwaway COPY and run the node
#   against it on an isolated port — validate consensus/recovery fixes BEFORE
#   they can touch the live chain; FAILS LOUD on a tip regression.
#     make repro-on-copy SLUG=import-reset ARGS='-nobgvalidation'
.PHONY: diagnose-gap repro-on-copy
diagnose-gap:
	@tools/diagnose_gap.sh $(SLUG)

repro-on-copy:
	@tools/repro_on_copy.sh $(SLUG) $(if $(ARGS),-- $(ARGS),)

$(eval $(call BUILD_NODE_TOOL,spec_zcl,lib/test/spec_main.c $(SPEC_SRCS) lib/test/src/test_helpers.c))
$(eval $(call BUILD_NODE_TOOL,wallet_dump,tools/wallet_dump.c))

session: $(TMPL_GEN) tools/session.c $(ALL_SRCS)
	$(CC) $(CFLAGS) -Wno-deprecated-declarations $(LDFLAGS) -o $@ $(filter-out $(TMPL_GEN),$^) $(TOR_LIBS) $(LIBS) $(GTK_LIBS) $(WEBKIT_LIBS) -lm
	./session

bot: $(TMPL_GEN) tools/bot.c $(ALL_SRCS)
	$(CC) $(CFLAGS) -Wno-deprecated-declarations $(LDFLAGS) -o $@ $(filter-out $(TMPL_GEN),$^) $(TOR_LIBS) $(LIBS) $(GTK_LIBS) $(WEBKIT_LIBS) -lm
	./bot

mock_rpc: tools/mock_rpc.c
	$(CC) -std=c23 -O2 -Wall -Wextra -Werror -pthread -o $@ $<

$(eval $(call BUILD_NODE_TOOL,wallet_sim,tools/wallet_sim.c))
$(eval $(call BUILD_NODE_TOOL,wallet_check,tools/wallet_check.c,-lm))
$(eval $(call BUILD_NODE_TOOL,rebuild_recent,tools/rebuild_recent.c,-lm,-fopenmp))

.PHONY: sim dump check-wallet
sim: wallet_sim
	./wallet_sim
dump: wallet_dump
	./wallet_dump

check-wallet: wallet_check
	./wallet_check

.PHONY: spec
spec: spec_zcl
	ulimit -s unlimited && ./spec_zcl

zclassic23: $(TMPL_GEN) main.c tools/mcp_server.c $(ALL_SRCS)
	$(CC) $(CFLAGS) -Wno-deprecated-declarations $(LDFLAGS) -o $@ $(filter-out $(TMPL_GEN),$^) $(TOR_LIBS) $(LIBS) $(GTK_LIBS) $(WEBKIT_LIBS)
	strip -s $@

zclassic-cli: cli.c $(CLI_SRCS) lib/util/src/safe_alloc.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ -lm

# In-tree WAL checkpoint tool used by `deploy`.  Replaces a dependency on
# the sqlite3(1) CLI that isn't installed by default on stock Ubuntu/Debian
# (only libsqlite3-0) — was P12.4 in AGENT.md.  Calls
# sqlite3_wal_checkpoint_v2(TRUNCATE) on the open DB and exits non-zero on
# failure so `make deploy` halts loudly instead of silently skipping the
# checkpoint.
tools/wal_checkpoint: tools/wal_checkpoint.c
	$(CC) -std=c23 -O2 -Wall -Wextra -Werror -Ivendor/include -o $@ $< \
	    -Lvendor/lib -l:libsqlite3.a -lpthread -ldl -lm

$(eval $(call BUILD_NODE_TOOL,wallet-wireframes,tools/wallet_wireframes.c))
$(eval $(call BUILD_NODE_TOOL,speedrun,tools/speedrun.c))

zcl-rpc: tools/zcl-rpc.c
	$(CC) -std=c23 -O2 -Wall -o $@ $<

# gen_sha3_windows: one-shot tool that queries a fully-synced reference
# node and overwrites lib/chain/{include/chain,src}/sha3_windows.{h,c}
# with SHA3-256 commitments over 1000-block windows. Standalone build:
# only the libs it directly uses, no DB, no Tor.
tools/gen_sha3_windows: tools/gen_sha3_windows.c \
		lib/chain/src/sha3_windows.c \
		lib/crypto/src/sha3.c lib/encoding/src/utilstrencodings.c \
		lib/json/src/json.c lib/platform/src/clock.c
	$(CC) -std=c23 -O3 -march=native -Wall -Wextra -Werror -pedantic \
	    -Wno-stringop-overflow -Wno-unused-result \
	    -Ilib/chain/include -Ilib/crypto/include -Ilib/encoding/include \
	    -Ilib/json/include -Ilib/platform/include -Ilib/util/include \
	    -D_POSIX_C_SOURCE=200809L \
	    -o $@ $^ -pthread

# gen_utxo_snapshot: build-time tool that walks a legacy zclassicd
# chainstate LevelDB and emits a canonical UTXO sidecar file ready
# for runtime mmap+SHA3-verify+bulk-INSERT (Stage J of fast-sync
# plan). Implemented as a `--gen-utxo-snapshot` mode of zclassic23
# itself (avoids duplicating the dep tree); invoke via:
#   zclassic23 --gen-utxo-snapshot <legacy_datadir> <out_path>

zcl-nodectl: tools/zcl-nodectl.c lib/util/include/util/rpc_paths.h
	$(CC) -std=c23 -O2 -Wall -Wextra -Werror -Ilib/util/include -o $@ $<

export_snapshot: tools/export_snapshot.c
	$(CC) -std=c23 -O2 -Wall -Wextra -Werror -Ivendor/include -o $@ $< -Lvendor/lib -l:libsqlite3.a -lpthread

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

# Default `make test` = the fast fork-based parallel suite (~1min, 282 groups).
# The slow single-process binary is still available as `make test-full`.
# Doctrine: never run test_zcl in the inner loop — use `make t ONLY=<group>`.
test: test_parallel
	ulimit -s unlimited && ./test_parallel

test-full: test_zcl
	ulimit -s unlimited && ./test_zcl

zclassic23-chaos: tools/sim/chaos.c tools/sim/sim_peer.c \
	lib/util/src/safe_alloc.c \
	lib/util/include/util/safe_alloc.h lib/net/src/net_fault.c \
	lib/net/include/net/net_fault.h lib/platform/src/clock.c \
	lib/platform/include/platform/clock.h lib/platform/include/platform/time_compat.h
	$(CC) -std=c23 -O2 -Wall -Wextra -Werror -pedantic \
	    -D_POSIX_C_SOURCE=200809L -Ilib/util/include -Ilib/net/include \
	    -Ilib/platform/include -Itools \
	    -o $@ tools/sim/chaos.c tools/sim/sim_peer.c \
	    lib/util/src/safe_alloc.c lib/net/src/net_fault.c \
	    lib/platform/src/clock.c

chaos: zclassic23-chaos
	@set -eu; \
	for s in tools/sim/scenarios/*.scenario; do \
	    echo "==> $$s"; \
	    ./zclassic23-chaos --scenario="$$s"; \
	done; \
	echo "==> All chaos scenarios PASSED"

chaos-clean:
	rm -f zclassic23-chaos
	rm -rf chaos-output/

# Crash recovery harness: fork zclassic23, SIGKILL at random points,
# restart, and assert data-integrity invariants. Needs a pre-seeded
# datadir (skips trivially if none exists — see tool header). Build
# depends on the node binary and the CLI RPC helper.
crash_recovery_test: tools/crash_recovery_test.c
	$(CC) -std=c23 -O2 -Wall -Wextra -Werror -pthread -o $@ $<

.PHONY: test-crash
# CI entry point for the crash recovery harness.
#
# The harness skips (exit 0) when its datadir does NOT exist, and keeps
# CI green on clean hosts. Don't pre-create the dir — an empty dir
# makes the harness try to start the node on a blank chainstate, which
# then reports "RPC never came up" as a harness error (false negative).
#
# When a fully seeded datadir is available (a CI worker with a pinned
# snapshot), point ZCL_CRASH_DATADIR at it and this target runs the
# full 10-iteration kill/restart cycle against real data. Otherwise
# the target runs through to the SKIP path.
test-crash: crash_recovery_test zclassic23 zcl-rpc
	@set -eu; \
	 dd="$${ZCL_CRASH_DATADIR:-/tmp/zcl-crashtest-ci.absent}"; \
	 if [ -n "$${ZCL_CRASH_DATADIR:-}" ] && [ ! -d "$$dd" ]; then \
	     echo "test-crash: ZCL_CRASH_DATADIR=$$dd does not exist — harness will SKIP"; \
	 fi; \
	 ZCL_CRASH_DATADIR="$$dd" ./crash_recovery_test --iterations=10

# Always-fresh end-to-end MCP test.
#
# `test_mcp_e2e` forks the real `./zclassic23 -mcp` binary and asserts
# wire-level envelope shapes.  If the binary is older than the MCP
# source files the in-suite test SKIPs with a clear message rather
# than failing with a confusing tool-count mismatch — but that means a
# bare `./test_zcl` after editing MCP code can silently skip the e2e
# coverage.  Use `make test-e2e` to force a rebuild of zclassic23 (and
# test_zcl) before running, so the e2e suite always runs against the
# current source.
test-e2e: zclassic23 test_zcl
	ulimit -s unlimited && ./test_zcl

# P11.4 shielded-payment gate.
#
# Runs the real transparent->shielded wallet path inside test_zcl with
# Sapling proving params loaded from ~/.zcash-params. The target skips on
# hosts that do not have the proving/verifying params installed so CI can
# call it unconditionally without creating false negatives on clean workers.
test-shielded-payment: test_zcl
	@set -eu; \
	params_dir="$$HOME/.zcash-params"; \
	for f in sapling-spend.params sapling-output.params sprout-groth16.params sprout-verifying.key; do \
		if [ ! -r "$$params_dir/$$f" ]; then \
			echo "test-shielded-payment: SKIP ($$params_dir/$$f missing)"; \
			exit 0; \
		fi; \
	done; \
	ZCL_STRESS_TESTS=1 ZCL_TEST_ONLY=shielded_payment ./test_zcl

# P11.5 store end-to-end gate.
#
# Runs the store order -> payment reconciliation -> token access path inside
# test_zcl. This is deterministic and self-contained, but remains opt-in so
# the default suite does not pay extra setup/runtime cost.
test-store-e2e: test_zcl
	ZCL_STRESS_TESTS=1 ZCL_TEST_ONLY=store_e2e ./test_zcl

# ── libFuzzer harnesses ───────────────────────────────────────
#
# Fuzz targets use clang + libFuzzer + ASan + UBSan. They compile
# the same ALL_SRCS as the main build (minus main.c), so the same
# code paths the node exercises are the code paths the fuzzer
# exercises. -O1 + -g because aggressive optimisation confuses
# sanitizer reports.
#
# `make fuzz` builds the three binaries. `make fuzz-ci` runs each
# for 60 seconds as a smoke test; CI uses this to detect already-
# latent crashes without chasing exhaustive coverage. If clang is
# unavailable, both targets print a skip message and exit 0 so
# gcc-only hosts never fail the build.
FUZZ_CC ?= clang
FUZZ_CFLAGS = -std=c23 -O1 -g -Wall -Wextra -Wno-unused-result \
	-Wno-deprecated-declarations \
	$(APP_INCLUDES) $(CONFIG_INCLUDES) $(LIB_INCLUDES) $(MCP_INCLUDES) \
	-Ilib/test/include -D_POSIX_C_SOURCE=200809L -Ivendor/include \
	-fsanitize=fuzzer,address,undefined \
	-fno-sanitize=alignment
FUZZ_LIBS = $(TOR_LIBS) $(LIBS)

FUZZ_TARGETS = fuzz_block fuzz_script fuzz_p2p

.PHONY: fuzz fuzz-ci
fuzz: $(FUZZ_TARGETS)

fuzz_block: tools/fuzz/fuzz_block.c $(TMPL_GEN) $(ALL_SRCS)
	@if ! command -v $(FUZZ_CC) >/dev/null 2>&1; then \
		echo "fuzz_block: $(FUZZ_CC) not found — SKIP (install clang for fuzzing)"; \
		touch $@; \
	else \
		echo "$(FUZZ_CC) ... -o $@"; \
		$(FUZZ_CC) $(FUZZ_CFLAGS) -o $@ tools/fuzz/fuzz_block.c $(ALL_SRCS) $(FUZZ_LIBS); \
	fi

fuzz_script: tools/fuzz/fuzz_script.c $(TMPL_GEN) $(ALL_SRCS)
	@if ! command -v $(FUZZ_CC) >/dev/null 2>&1; then \
		echo "fuzz_script: $(FUZZ_CC) not found — SKIP"; \
		touch $@; \
	else \
		echo "$(FUZZ_CC) ... -o $@"; \
		$(FUZZ_CC) $(FUZZ_CFLAGS) -o $@ tools/fuzz/fuzz_script.c $(ALL_SRCS) $(FUZZ_LIBS); \
	fi

fuzz_p2p: tools/fuzz/fuzz_p2p.c $(TMPL_GEN) $(ALL_SRCS)
	@if ! command -v $(FUZZ_CC) >/dev/null 2>&1; then \
		echo "fuzz_p2p: $(FUZZ_CC) not found — SKIP"; \
		touch $@; \
	else \
		echo "$(FUZZ_CC) ... -o $@"; \
		$(FUZZ_CC) $(FUZZ_CFLAGS) -o $@ tools/fuzz/fuzz_p2p.c $(ALL_SRCS) $(FUZZ_LIBS); \
	fi

fuzz-ci: $(FUZZ_TARGETS)
	@if ! command -v $(FUZZ_CC) >/dev/null 2>&1; then \
		echo "fuzz-ci: $(FUZZ_CC) not found — SKIP"; \
		exit 0; \
	fi; \
	set -e; \
	for t in $(FUZZ_TARGETS); do \
		echo "=== $$t (60s) ==="; \
		seed_dir="lib/test/fuzz_seeds/$${t#fuzz_}"; \
		work_dir="/tmp/zcl_fuzz_$${t#fuzz_}"; \
		rm -rf "$$work_dir"; mkdir -p "$$work_dir"; \
		ASAN_OPTIONS=detect_leaks=0 ./$$t -max_total_time=60 \
			-timeout=1 -print_final_stats=1 "$$work_dir" "$$seed_dir"; \
		rm -rf "$$work_dir"; \
	done

# Same binaries with leak detection ON. Separate target so CI stays
# green while known-pre-existing leaks are being triaged; developers
# and Wave 4+ commits that fix leaks opt into this stricter run.
fuzz-ci-leaks: $(FUZZ_TARGETS)
	@if ! command -v $(FUZZ_CC) >/dev/null 2>&1; then \
		echo "fuzz-ci-leaks: $(FUZZ_CC) not found — SKIP"; \
		exit 0; \
	fi; \
	set -e; \
	for t in $(FUZZ_TARGETS); do \
		echo "=== $$t (60s, leak detection ON) ==="; \
		seed_dir="lib/test/fuzz_seeds/$${t#fuzz_}"; \
		work_dir="/tmp/zcl_fuzz_$${t#fuzz_}_leaks"; \
		rm -rf "$$work_dir"; mkdir -p "$$work_dir"; \
		./$$t -max_total_time=60 -timeout=1 -print_final_stats=1 \
			"$$work_dir" "$$seed_dir"; \
		rm -rf "$$work_dir"; \
	done

# ── P11.6 — 7-day soak runner ─────────────────────────────────
#
# Separate binary that polls a running zclassic23 every 60 s
# against the analyzer in lib/test/src/soak_harness.c. Verdict
# failure (crash / tip-stall / RSS-walk / too-short / no-samples)
# causes exit non-zero, so systemd / CI can gate on a 7-day run
# without the operator having to read the log.
#
# `make soak-7day`   runs the full 604800 s gate against the
#                    installed zclassic23 (MVP criterion #6).
# `make soak-smoke`  runs a 5-minute smoke test of the same
#                    binary so the runner itself doesn't rot
#                    between 7-day gates — safe to hook into
#                    CI on a machine that has the node up.
#
# Neither target is wired into the default `ci` pipeline: 7 days
# is obviously out of band, and the smoke target needs a live
# node on the same host, which most CI workers don't provide.
tools/soak/soak_runner: tools/soak/main.c lib/test/src/soak_harness.c \
                        lib/test/include/test/soak_harness.h
	$(CC) -std=c23 -O2 -Wall -Wextra -Werror -pedantic \
	    -Ilib/test/include -o $@ \
	    tools/soak/main.c lib/test/src/soak_harness.c

soak-7day: tools/soak/soak_runner zcl-rpc
	./tools/soak/soak_runner \
	    --duration-sec=604800 \
	    --interval-sec=60 \
	    --service=zclassic23 \
	    --rpc=./zcl-rpc

soak-smoke: tools/soak/soak_runner zcl-rpc
	./tools/soak/soak_runner \
	    --duration-sec=300 \
	    --interval-sec=30 \
	    --service=zclassic23 \
	    --rpc=./zcl-rpc \
	    --stall-sec=600 \
	    --warmup-sec=60

.PHONY: bench-sync
bench-sync: zclassic23 bench_fresh_sync
	./bench_fresh_sync

bench_fresh_sync: tools/bench_fresh_sync.c
	$(CC) -O2 -o $@ $<

bench: zclassic23
	@./zclassic23 -bench

bench-regress: zclassic23
	@./zclassic23 -bench-regress

# CI guard: fresh datadir, must reach tip-10 in <600s against a local
# peer. Fails the build if sync regresses to the 9-hour stall the
# baked checkpoints + watchdog thread + peer-floor invariant are
# meant to prevent. Skipped automatically if no local peer is up.
# CI guard: fresh datadir + downloaded consensus_snapshot.db only,
# must reach tip > 1M with utxos > 1M in <90s. Asserts Wave 11A
# snapshot-first boot ordering didn't regress — without that fix the
# import path is silently dead. Skipped if no source snapshot is
# available locally (~/.zclassic-c23{,-test}/consensus_snapshot.db).
.PHONY: ci-coldstart
ci-coldstart: zclassic23
	@bash tools/scripts/cold_start_test.sh

.PHONY: ci-sync-smoke
ci-sync-smoke: zclassic23
	@if ! ss -tln 2>/dev/null | grep -q ':8033 '; then \
	    echo "[ci-sync-smoke] no local peer on :8033 — skipping"; \
	    exit 0; \
	fi
	@echo "[ci-sync-smoke] recording C benchmark placeholders..."
	@./zclassic23 -bench-coldstart
	@./zclassic23 -bench-mtbf
	@echo "[ci-sync-smoke] OK"

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Deploy: lint → WAL checkpoint → install service → restart → RPC verify.
#
# `make deploy` used to print "Deployed." whenever systemd held the unit
# active for >2s — false-positive friendly. The new target fails loudly on
# three distinct paths:
#   1. `lint` — untouched, but now actually FAILs on raw sqlite3_step.
#   2. `wal_checkpoint` — truncate WAL before SIGTERM so SQLite doesn't
#      recover a half-checkpointed journal on boot.
#   3. `tools/deploy_verify.sh` — poll `zclassic-cli getblockcount` until the
#      node answers and diagnostics are ready, with a startup-sized deadline.
#
# The wal_checkpoint step calls the in-tree tools/wal_checkpoint binary
# (P12.4 — was an inline `sqlite3(1)` CLI invocation before, which failed
# on stock Ubuntu/Debian hosts where the CLI isn't installed).  The tool
# issues `sqlite3_wal_checkpoint_v2(TRUNCATE)` via the library only — no
# DELETE, no unguarded statements, and safe to re-run.
deploy: lint zclassic23 zclassic-cli tools/wal_checkpoint
	@if [ -f $(HOME)/.zclassic-c23/node.db ]; then \
	    ./tools/wal_checkpoint $(HOME)/.zclassic-c23/node.db \
	        || { echo "WAL checkpoint failed"; exit 1; }; \
	fi
	@install -m 644 deploy/zclassic23.service $(HOME)/.config/systemd/user/zclassic23.service
	@systemctl --user daemon-reload
	systemctl --user restart zclassic23
	@./tools/deploy_verify.sh

release:
	@./tools/release.sh

clean:
	rm -f test_zcl zclassic23 zclassic-cli $(ALL_OBJS)

# ── Coverage (wave 5 #8) ──────────────────────────────────────
#
# Establishes a measurement path.  This is NOT targeted at a specific
# percentage yet — it exists so future commits can track whether they
# move the needle up or down.
#
# Builds a separate `test_zcl_cov` binary with gcov instrumentation
# instead of clobbering the main `test_zcl` (which uses -flto and -O3,
# both of which fight with coverage instrumentation).  Running the
# coverage binary emits .gcda files next to each translation unit;
# we then render them with either lcov+genhtml or gcovr — whichever
# is installed — and leave the tooling path permissive so a developer
# without coverage utilities still gets a useful message.
#
# Normal `make` / `make test` paths are untouched.
#
# NB: `-Werror` gets stripped alongside `-flto -O3` because -O0/-O1 +
# gcov produces a different set of lints (unused-static,
# format-truncation at different inlining thresholds) that fire
# cleanly in the main build but trip -Werror here.  Coverage is an
# observability tool, not a production build — warnings still print,
# they just don't block the binary.
#
# Two things have to be right before the numbers are meaningful:
#
# 1. Optimisation level.  -O0 + gcov drives one of the recursive
#    JSON tests into stack overflow in ~11 minutes wall-clock; -O1
#    keeps the instrumentation accurate (lcov/gcov handle it fine)
#    while cutting runtime roughly in half and eliminating the
#    regression.
#
# 2. Object-file layout.  The main `test_zcl` target compiles all
#    sources in ONE `cc` invocation — that's fast for LTO but ruinous
#    for gcov, because files like `lib/net/src/protocol.c` and
#    `lib/rpc/src/protocol.c` share a basename and collide at .gcda
#    write time ("overwriting an existing profile data with a
#    different checksum").  For the coverage build we therefore
#    compile each source into its own `build/cov/<same/path>/file.o`
#    FIRST, then link — this way each .gcda lives next to its .o and
#    the directory structure guarantees uniqueness.  Slower than the
#    single-command build, but sound.
COV_BUILD_DIR = build/cov
COV_CFLAGS = $(filter-out -flto -O3 -march=native -Werror,$(CFLAGS)) \
             --coverage -O1 -g -DCOVERAGE_BUILD -DZCL_TESTING
COV_LDFLAGS = $(filter-out -flto,$(LDFLAGS)) --coverage

COV_TEST_SRCS := $(filter-out lib/test/src/test_parallel.c, $(TEST_SRCS))
COV_OBJS := $(patsubst %.c,$(COV_BUILD_DIR)/%.o,$(COV_TEST_SRCS) $(SPEC_SRCS) $(ALL_SRCS))

$(COV_BUILD_DIR)/%.o: %.c $(TMPL_GEN)
	@mkdir -p $(dir $@)
	$(CC) $(COV_CFLAGS) -Wno-deprecated-declarations -c -o $@ $<

test_zcl_cov: $(COV_OBJS)
	$(CC) $(COV_CFLAGS) $(COV_LDFLAGS) -o $@ $(COV_OBJS) $(TOR_LIBS) $(LIBS) $(GTK_LIBS) $(WEBKIT_LIBS)

coverage: coverage-clean test_zcl_cov
	@echo "== Resetting gcov counters =="
	@find $(COV_BUILD_DIR) -name '*.gcda' -delete 2>/dev/null || true
	@echo "== Running test_zcl_cov =="
	@# Match the `test` target — some json/recursion tests need
	@# unlimited stack.  If the binary still crashes we continue
	@# anyway so the partial coverage data is still flushed for any
	@# test that ran to completion before the crash (cov_flush.c
	@# installs a SIGSEGV handler that calls __gcov_dump).
	@ulimit -s unlimited && ./test_zcl_cov || true
	@if command -v lcov >/dev/null 2>&1; then \
		echo "== Rendering coverage via lcov =="; \
		lcov --capture --directory $(COV_BUILD_DIR) --output-file coverage.info \
		     --rc geninfo_unexecuted_blocks=1 --quiet || true; \
		lcov --remove coverage.info \
		     '*/lib/test/*' '*/vendor/*' '*/tools/fuzz/*' '/usr/*' \
		     --output-file coverage.info --quiet || true; \
		lcov --summary coverage.info; \
		if command -v genhtml >/dev/null 2>&1; then \
			genhtml --quiet coverage.info --output-directory coverage_html; \
			echo "== HTML report: coverage_html/index.html =="; \
		else \
			echo "(genhtml not installed — summary only)"; \
		fi; \
	elif command -v gcovr >/dev/null 2>&1; then \
		echo "== Rendering coverage via gcovr =="; \
		gcovr --root . --filter 'app/' --filter 'lib/' --filter 'tools/' \
		      --exclude 'lib/test/.*' --exclude 'vendor/.*' \
		      --exclude 'tools/fuzz/.*' --print-summary; \
	elif command -v gcov >/dev/null 2>&1; then \
		echo "== Rendering coverage via plain gcov (install lcov or gcovr for full report) =="; \
		gcov_sum=$$(mktemp); \
		find $(COV_BUILD_DIR) -name '*.gcda' \
		    -not -path '*/lib/test/*' -not -path '*/tools/fuzz/*' \
		    -print0 2>/dev/null \
		    | xargs -0 -r gcov -n 2>/dev/null \
		    > $$gcov_sum || true; \
		awk ' \
		    /^File / { cur=$$2; gsub(/'\''/, "", cur); \
		               sysheader = (index(cur, "/usr/") == 1 || index(cur, "vendor/") > 0 || index(cur, "lib/test/") > 0); \
		               next } \
		    /^Lines executed:/ { \
		        if (sysheader) next; \
		        split($$2, p, ":"); pct = p[2]; gsub(/%.*$$/, "", pct); \
		        total = $$4 + 0; \
		        exec = total * (pct+0) / 100.0; \
		        sum_exec += exec; sum_total += total; n++ \
		    } \
		    END { \
		        if (n > 0 && sum_total > 0) { \
		            printf "coverage: %d translation units, %d / %d lines executed (%.1f%%)\n", \
		                n, sum_exec, sum_total, 100.0 * sum_exec / sum_total; \
		            printf "(install lcov or gcovr for per-file breakdown + HTML report)\n" \
		        } else { \
		            printf "coverage: no .gcda data — did the test binary fail to run?\n" \
		        } \
		    }' $$gcov_sum; \
		rm -f $$gcov_sum *.gcov 2>/dev/null || true; \
	else \
		echo "WARN: install lcov, gcovr, or gcc's gcov to render the report."; \
		echo "Raw .gcda files are left in place for manual inspection."; \
	fi

coverage-clean:
	@rm -rf $(COV_BUILD_DIR) coverage.info coverage_html test_zcl_cov
	@find . \( -name '*.gcda' -o -name '*.gcno' \) -delete 2>/dev/null || true
	@echo "Coverage artifacts removed."

# ── docs-mcp ───────────────────────────────────────────────────
# Regenerate MCP_REFERENCE.md by running the MCP server in stdio
# mode and piping the tools/list JSON response through a small
# Python formatter.  The formatter uses stdlib only — no pip install
# required.  Use `make docs-mcp-check` in CI to fail when the
# checked-in reference drifts from what the live router emits.
docs-mcp: zclassic23
	@echo "== Generating MCP_REFERENCE.md =="
	@echo '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' \
		| ./zclassic23 -mcp 2>/dev/null \
		| python3 tools/gen_mcp_reference.py > MCP_REFERENCE.md
	@wc -l MCP_REFERENCE.md

docs-mcp-check: zclassic23
	@echo "== Verifying MCP_REFERENCE.md is up to date =="
	@tmp=$$(mktemp); \
	 echo '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' \
		| ./zclassic23 -mcp 2>/dev/null \
		| python3 tools/gen_mcp_reference.py > "$$tmp"; \
	 if ! diff -q MCP_REFERENCE.md "$$tmp" >/dev/null; then \
		echo "MCP_REFERENCE.md is STALE. Run: make docs-mcp"; \
		diff -u MCP_REFERENCE.md "$$tmp" | head -40; \
		rm -f "$$tmp"; \
		exit 1; \
	 fi; \
	 rm -f "$$tmp"; \
	 echo "MCP_REFERENCE.md is up to date."

# ── ci ─────────────────────────────────────────────────────────
# Single command for full verification: build, test, fuzz (short),
# and coverage.  Fail-fast — stops at the first broken stage so
# you don't waste minutes on coverage when tests don't pass.
#
# Usage:
#   make ci                 # full pipeline
#   make ci SKIP_FUZZ=1     # skip the fuzz stage (faster)
#   make ci SKIP_COV=1      # skip coverage (faster)
check-malloc:
	@echo "══ LINT: bare malloc/calloc/realloc in app/tools code ══"
	@HITS=$$(grep -rn '[^_]malloc\s*(' app/ tools/ --include='*.c' \
	    | grep -v 'zcl_malloc\|zcl_calloc\|zcl_realloc\|raw-alloc-ok\|safe_alloc\|".*malloc\|LOG_\|fprintf'); \
	if [ -n "$$HITS" ]; then \
	    echo "$$HITS"; \
	    echo "FAIL: bare malloc in app/tools code (use zcl_malloc or mark // raw-alloc-ok)"; \
	    exit 1; \
	fi
	@HITS=$$(grep -rn '[^_]calloc\s*(' app/ tools/ --include='*.c' \
	    | grep -v 'zcl_calloc\|raw-alloc-ok\|safe_alloc\|".*calloc\|LOG_\|fprintf'); \
	if [ -n "$$HITS" ]; then \
	    echo "$$HITS"; \
	    echo "FAIL: bare calloc in app/tools code (use zcl_calloc or mark // raw-alloc-ok)"; \
	    exit 1; \
	fi
	@HITS=$$(grep -rn '[^_]realloc\s*(' app/ tools/ --include='*.c' \
	    | grep -v 'zcl_realloc\|raw-alloc-ok\|safe_alloc\|".*realloc\|LOG_\|fprintf'); \
	if [ -n "$$HITS" ]; then \
	    echo "$$HITS"; \
	    echo "FAIL: bare realloc in app/tools code (use zcl_realloc or mark // raw-alloc-ok)"; \
	    exit 1; \
	fi
	@echo "  OK: no raw allocations"

check-silent-errors:
	@echo "══ LINT: bare return -1 in MCP handlers ══"
	@HITS=$$(grep -rn 'return -1;' tools/mcp/controllers/ --include='*.c' \
	    | grep -v 'LOG_ERR\|log_json\|fprintf\|// silent-ok' \
	    | grep -vE '// raw-return-ok:[A-Za-z][A-Za-z0-9_-]+'); \
	if [ -n "$$HITS" ]; then \
	    echo "$$HITS"; \
	    echo "FAIL: bare return -1 in MCP handlers (use LOG_ERR or mark // raw-return-ok:<tag>)"; \
	    exit 1; \
	fi
	@echo "  OK: all MCP error returns logged"

check-raw-sqlite:
	@echo "══ LINT: raw sqlite3_step in app code ══"
	@tools/scripts/check_raw_sqlite.sh

check-raw-malloc:
	@echo "══ LINT: raw malloc/calloc/realloc in production code ══"
	@tools/scripts/check_raw_malloc.sh

check-coins-lookup-nullcheck:
	@echo "══ LINT: guarded controller coin lookups ══"
	@tools/scripts/check_coins_lookup_nullcheck.sh

tools/check_observability_pairing: tools/check_observability_pairing.c
	$(CC) -std=c23 -O2 -Wall -Wextra -Werror -o $@ $<

check-observability-pairing: tools/check_observability_pairing
	@echo "══ LINT: observable stderr diagnostics ══"
	@./tools/check_observability_pairing

check-silent-errors-services:
	@echo "══ LINT: silent error returns in services ══"
	@HITS=$$(grep -rn -B1 'return -1;' app/services/src/ --include='*.c' \
	    | grep 'return -1;' \
	    | grep -v 'LOG_ERR\|LOG_FAIL\|log_json\|fprintf\|printf' \
	    | grep -vE '(//|/\*) raw-return-ok:[A-Za-z][A-Za-z0-9_-]+' \
	    | while read -r line; do \
	        file=$$(echo "$$line" | cut -d: -f1); \
	        lnum=$$(echo "$$line" | cut -d: -f2); \
	        prev=$$((lnum - 1)); \
	        prev_line=$$(sed -n "$${prev}p" "$$file"); \
	        echo "$$prev_line" | grep -qE 'fprintf|LOG_ERR|LOG_FAIL|log_json|printf' || echo "$$line"; \
	    done); \
	if [ -n "$$HITS" ]; then \
	    echo "$$HITS"; \
	    echo "FAIL: silent error returns found in services"; \
	    exit 1; \
	fi
	@echo "  OK: all service error returns logged"

check-silent-errors-controllers:
	@echo "══ LINT: silent error returns in controllers ══"
	@HITS=$$(grep -rn -B1 'return -1;' app/controllers/src/ --include='*.c' \
	    | grep 'return -1;' \
	    | grep -v 'LOG_ERR\|LOG_FAIL\|log_json\|fprintf\|printf' \
	    | grep -vE '(//|/\*) raw-return-ok:[A-Za-z][A-Za-z0-9_-]+' \
	    | while read -r line; do \
	        file=$$(echo "$$line" | cut -d: -f1); \
	        lnum=$$(echo "$$line" | cut -d: -f2); \
	        prev=$$((lnum - 1)); \
	        prev_line=$$(sed -n "$${prev}p" "$$file"); \
	        echo "$$prev_line" | grep -qE 'fprintf|LOG_ERR|LOG_FAIL|log_json|printf' || echo "$$line"; \
	    done); \
	if [ -n "$$HITS" ]; then \
	    echo "$$HITS"; \
	    echo "FAIL: silent error returns found in controllers (use LOG_ERR/LOG_RETURN, prev-line fprintf, or mark // raw-return-ok:<reason>)"; \
	    exit 1; \
	fi
	@echo "  OK: all controller error returns logged"

check-before-save-hooks:
	@echo "══ LINT: critical models wire before_save hooks ══"
	@for model in utxo block wallet_key wallet_tx; do \
	    grep -q 'before_save' app/models/src/$$model.c \
	    || (echo "FAIL: app/models/src/$$model.c missing before_save hook" && exit 1); \
	done
	@echo "  OK: critical models have before_save hooks"

# Move 4: every long-running thread goes through thread_registry_spawn{,_ex}.
# Short-burst workers joined within the same function, and pthread_attr-using
# detached-helper wrappers, are explicitly opted out with a `raw-pthread-ok`
# marker on the call line or the line immediately above. The registry's own
# implementation in lib/util/src/thread_registry.c is implicitly skipped.
check-pthread-create:
	@echo "══ LINT: raw pthread_create outside thread_registry ══"
	@HITS=$$(grep -rn 'pthread_create\s*(' lib/ app/ tools/ config/ --include='*.c' \
	    | grep -v 'lib/test/' \
	    | grep -v 'lib/util/src/thread_registry.c' \
	    | grep -v 'thread_registry_spawn\|thread_registry_trampoline' \
	    | grep -v 'raw-pthread-ok' \
	    | while read -r line; do \
	        f=$$(echo "$$line" | cut -d: -f1); \
	        n=$$(echo "$$line" | cut -d: -f2); \
	        prev=$$((n - 1)); \
	        if [ "$$prev" -gt 0 ] && \
	           sed -n "$${prev}p" "$$f" | grep -q 'raw-pthread-ok'; then \
	            continue; \
	        fi; \
	        echo "$$line"; \
	    done); \
	if [ -n "$$HITS" ]; then \
	    echo "$$HITS"; \
	    echo "FAIL: raw pthread_create in production code (use thread_registry_spawn{,_ex} or mark // raw-pthread-ok: <reason>)"; \
	    exit 1; \
	fi
	@echo "  OK: all pthread_create call sites accounted for"

# Move 11: every app/models/src/*.c either invokes validates_* macros
# from app/models/include/models/activerecord.h, or carries an
# ar-validate-skip:<tag> marker explaining why the AR validation
# lifecycle does not apply (infrastructure wrapper, registry, etc.).
check-model-validation:
	@echo "══ LINT: model validation coverage ══"
	@./tools/scripts/check_model_validation.sh

# Keep top-level functions in app/controllers + app/services under 500
# lines. Single state-machines that truly belong as one function can carry
# a `// long-function-ok:<tag>` override marker explaining WHY.
check-long-functions:
	@echo "══ LINT: long function cap (500 lines) ══"
	@./tools/scripts/check_long_functions.sh

# Wave 9a: every register_*_rpc_commands callsite uses rpc_table_must_append.
# rpc_table_append returns false silently on registration failure (duplicate
# name / MAX_RPC_COMMANDS cap / table running) — that silent failure mode
# left the control-group RPCs unreachable for a release cycle. The
# must_append variant aborts at boot with a precise reason.
check-rpc-registrar:
	@echo "══ LINT: rpc_table_must_append in registrars ══"
	@./tools/scripts/check_rpc_registrar.sh

# Lag-SLO observability: the legacy_mirror_sync_service must emit
# EV_LAG_SLO_BREACH and EV_MIRROR_CONCURRENT_CATCHUP, and the
# chain_advance_coordinator must honor mirror_lag_sla_breach_blocks.
# Prevents the "silent lag" regression we shipped this gate to lock down.
check-lag-slo-observable:
	@echo "══ LINT: lag SLO observability ══"
	@./tools/scripts/check_lag_slo_observable.sh

# lib/ layer purity: no lib/ file should #include from app/ unless the
# include is in the grandfathered baseline or has a documented per-line
# override marker. Catches regressions; lets us pay down the existing
# debt incrementally.
check-lib-layering:
	@echo "══ LINT: lib/ layer purity ══"
	@./tools/scripts/check_lib_layering.sh

# Supervisor registration: every long-running service in
# app/services/src/*_service.c must register a liveness contract with
# the supervisor (Round 5) OR appear in supervisor_baseline.txt OR
# carry a per-file `// supervisor-ok:<tag>` override marker. Drives
# opt-in adoption of the supervisor primitive over Rounds 6-8.
check-supervisor-registration:
	@echo "══ LINT: supervisor registration ══"
	@./tools/scripts/check_supervisor_registration.sh

# Lint gate #16 — typed blocker primitive adoption (Round 6 C6).
# Ratchets raw `char *_blocker[]` string fields / `lms_set_blocker(`
# legacy setters / `last_blocker_code` mutations to the typed
# `blocker_set()` primitive (lib/util/blocker.h). Baseline file
# enumerates the grandfathered sites; must shrink over Rounds 7-9.
check-typed-blocker:
	@echo "══ LINT: typed blocker adoption ══"
	@./tools/scripts/check_typed_blocker.sh

# Gate #18 graduated WARN → RATCHET (E10): fails on any new off-shape
# app/ .c file (the allowlist is the baseline and is currently empty).
check-framework-shape:
	@echo "→ Gate #18: framework_shape_check"
	@ZCL_LINT_MODE=RATCHET ./tools/lint/framework_shape_check.sh

check-no-raw-clock-outside-platform:
	@echo "→ Gate #19: no_raw_clock_outside_platform"
	@./tools/lint/check_no_raw_clock_outside_platform.sh

# Gate #20 graduated WARN → RATCHET (E10): fails on any new controller
# file that uses raw sqlite. Baseline of grandfathered files lives in
# tools/lint/no_raw_sqlite_in_controllers_baseline.txt (may only shrink).
check-no-raw-sqlite-in-controllers:
	@echo "→ Gate #20: no_raw_sqlite_in_controllers"
	@ZCL_LINT_MODE=RATCHET ./tools/lint/check_no_raw_sqlite_in_controllers.sh

check-supervisor-domain:
	@echo "→ Gate #21: supervisor_domain"
	@./tools/lint/check_supervisor_domain.sh

# Gate E1 — file-size ceiling for app/ .c files (RATCHET). Mega-modules
# cannot hide behind <500-LOC functions; baseline at
# tools/scripts/file_size_ceiling_baseline.txt may only shrink.
check-file-size-ceiling:
	@echo "══ LINT: app/ file-size ceiling (E1) ══"
	@./tools/scripts/check_file_size_ceiling.sh

# Gate E9 — EV_OPERATOR_NEEDED emit must reach a registered sink (HARD).
# The silent-halt fix: the loud "human needed" signal can never be emitted
# without a subscriber in lib/util/src/alerts.c.
check-operator-needed-sink:
	@echo "══ LINT: operator-needed sink (E9) ══"
	@./tools/scripts/check_operator_needed_sink.sh

# Gate E11 — doc accuracy: the gate list in DEFENSIVE_CODING.md must match
# the actual check-* dependencies of the lint: target (count + names).
check-doc-accuracy:
	@echo "══ LINT: doc accuracy (E11) ══"
	@./tools/scripts/check_doc_accuracy.sh

# Gate E2 — new service functions return struct zcl_result, not bare
# bool/int (RATCHET at file granularity; baseline at
# tools/scripts/one_result_type_baseline.txt may only shrink).
check-one-result-type:
	@echo "══ LINT: one result type (E2) ══"
	@./tools/scripts/check_one_result_type.sh

# ── Shape-skeleton generator (Workstream A5, FRAMEWORK.md Law 3) ──────
# Emit a correct, compiling, readable skeleton for one of the four shapes
# into the right shape folder. Plain committed source (no metaprogramming),
# matching the exemplars so it passes the framework lint gates the day it
# lands. The generator never edits a registry — it prints the wiring step.
#
#   make new-condition  NAME=foo_bar   -> app/conditions/src/foo_bar.c
#   make new-model      NAME=foo        -> app/models/src/foo.c
#   make new-job        NAME=foo_stage  -> app/jobs/src/foo_stage.c
#   make new-controller NAME=foo        -> app/controllers/src/foo_controller.c
.PHONY: new-condition new-model new-job new-controller
new-condition:
	@test -n "$(NAME)" || { echo "usage: make new-condition NAME=foo_bar"; exit 1; }
	@./tools/new_shape.sh condition "$(NAME)"
new-model:
	@test -n "$(NAME)" || { echo "usage: make new-model NAME=foo"; exit 1; }
	@./tools/new_shape.sh model "$(NAME)"
new-job:
	@test -n "$(NAME)" || { echo "usage: make new-job NAME=foo_stage"; exit 1; }
	@./tools/new_shape.sh job "$(NAME)"
new-controller:
	@test -n "$(NAME)" || { echo "usage: make new-controller NAME=foo"; exit 1; }
	@./tools/new_shape.sh controller "$(NAME)"

# Gate E3 — shape source files include their shape contract header
# (conditions -> framework/condition.h, models -> models/ header,
# supervisors -> supervisor header). HARD: the tree already complies.
check-shape-includes-header:
	@echo "══ LINT: shape includes header (E3) ══"
	@./tools/scripts/check_shape_includes_header.sh

# Gate E4 — projections are pure folds: no app-layer (services/controllers)
# includes and no AR model saves. HARD: the projection set already complies.
check-projections-pure:
	@echo "══ LINT: projections pure (E4) ══"
	@./tools/scripts/check_projections_pure.sh

# Gate E6 — one chain-state write path (RATCHET). Legacy writer surfaces
# are grandfathered in tools/scripts/one_write_path_baseline.txt and shrink
# as B8 deletes them; new write surfaces fail.
check-one-write-path:
	@echo "══ LINT: one write path (E6) ══"
	@./tools/scripts/check_one_write_path.sh

# Gate E7 — no authoritative RAM state (RATCHET). Direct active_chain
# internals/global active_chain state are forbidden outside the baseline.
check-no-authoritative-ram-state:
	@echo "══ LINT: no authoritative RAM state (E7) ══"
	@./tools/scripts/check_no_authoritative_ram_state.sh

# Gate E5 — Job stages advance OR block (HARD). Every app/jobs/src/*_stage.c
# step must surface JOB_BLOCKED/JOB_IDLE on non-progress AND reference a cursor
# (cursor_out / c->cursor_in / stage_cursor) — no silent forward spin. The 8
# stages already comply, so the gate runs HARD.
check-stage-advances-or-blocks:
	@echo "══ LINT: stage advances-or-blocks (E5) ══"
	@./tools/scripts/check_stage_advances_or_blocks.sh

check-no-silent-ready:
	@echo "══ LINT: no-silent-ready (E8) ══"
	@./tools/scripts/check_no_silent_ready.sh

lint: check-malloc check-silent-errors check-raw-sqlite check-raw-malloc check-coins-lookup-nullcheck check-observability-pairing check-silent-errors-services check-silent-errors-controllers check-before-save-hooks check-pthread-create check-model-validation check-long-functions check-rpc-registrar check-lag-slo-observable check-lib-layering check-supervisor-registration check-typed-blocker check-framework-shape check-no-raw-clock-outside-platform check-no-raw-sqlite-in-controllers check-supervisor-domain check-file-size-ceiling check-operator-needed-sink check-doc-accuracy check-one-result-type check-shape-includes-header check-projections-pure check-one-write-path check-no-authoritative-ram-state check-stage-advances-or-blocks check-no-silent-ready
	@echo "══ LINT: all checks passed ══"

ci: lint bench-regress zclassic23 test_zcl
	@echo "══ CI: test ══"
	ulimit -s unlimited && ./test_zcl
	@echo ""
	@echo "══ CI: test-crash ══"
	$(MAKE) test-crash
	@echo ""
	@if [ "$(SKIP_FUZZ)" != "1" ]; then \
		echo "══ CI: fuzz-ci ══"; \
		$(MAKE) fuzz-ci || exit 1; \
		echo ""; \
	else \
		echo "══ CI: fuzz-ci (SKIPPED — SKIP_FUZZ=1) ══"; \
	fi
	@if [ "$(SKIP_COV)" != "1" ]; then \
		echo "══ CI: coverage ══"; \
		$(MAKE) coverage || exit 1; \
	else \
		echo "══ CI: coverage (SKIPPED — SKIP_COV=1) ══"; \
	fi
	@echo ""
	@echo "══ CI: ALL STAGES PASSED ══"

audit:
	@tools/dep_audit.sh

check-restart-follow:
	./zcl-nodectl verify-follow --restart
