# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0

# Hand-written POSIX Makefile. Deliberately boring so it keeps working
# forever. No autotools, CMake, meson, or ninja. No -j: never pass -j to
# make per the project conventions in CLAUDE.md.

PREFIX     ?= /usr/local
VERSION    ?= 0.0.0-dev

CC         ?= cc
CSTD       ?= -std=c11
WARN       ?= -Wall -Wextra -Wpedantic -Werror -Wshadow -Wstrict-prototypes
OPT        ?= -O2
DEBUG      ?= -g
CFLAGS     ?= $(CSTD) $(WARN) $(OPT) $(DEBUG)
CFLAGS     += -D_POSIX_C_SOURCE=200809L
CFLAGS     += -DMCPBRIDGE_VERSION=\"$(VERSION)\"
CFLAGS     += -Isrc -Ivendor/cjson

LDFLAGS    ?=

# Platform detection. The watcher backend is picked at compile time
# based on the host OS. Darwin -> kqueue, Linux -> inotify. Other
# platforms are not supported.
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    WATCH_SRC =
else ifeq ($(UNAME_S),Linux)
    WATCH_SRC =
else
    $(error unsupported platform: $(UNAME_S))
endif

# Shared sources (modules landing in later sub-targets get added here).
CORE_SRCS  = \
    src/log.c \
    src/util.c \
    src/mcp.c \
    src/fsm.c \
    src/transport_stdio.c \
    src/transport_http.c \
    src/dispatch.c \
    src/daemon_client.c \
    src/config.c

MAIN_SRCS  = src/main.c $(CORE_SRCS) $(WATCH_SRC)
CJSON_SRCS = vendor/cjson/cJSON.c

MAIN_OBJS  = $(MAIN_SRCS:.c=.o) $(CJSON_SRCS:.c=.o)

# Test-only helpers built as standalone binaries used by integration
# tests. These are NOT part of TEST_BINS — they are prerequisites of
# the actual test binaries that invoke them.
TEST_HELPERS = tests/fake_echo tests/fake_mcp tests/fake_http_mcp

BIN        = mcpbridge
TEST_BINS  = tests/smoke_test tests/mcp_test tests/fsm_test \
             tests/transport_stdio_test tests/transport_http_test \
             tests/dispatch_test tests/daemon_client_test

.PHONY: all test clean install

all: $(BIN)

$(BIN): $(MAIN_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(MAIN_OBJS)

# Generic .c -> .o rule, scoped to our source dirs.
src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

vendor/cjson/%.o: vendor/cjson/%.c
	$(CC) $(CFLAGS) -Wno-error -c -o $@ $<

tests/%.o: tests/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Test binaries link against the core sources directly so each test
# module exercises real code with no mocking layer.
tests/smoke_test: tests/smoke_test.o $(CORE_SRCS:.c=.o) $(CJSON_SRCS:.c=.o)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/smoke_test.o $(CORE_SRCS:.c=.o) $(CJSON_SRCS:.c=.o)

tests/mcp_test: tests/mcp_test.o $(CORE_SRCS:.c=.o) $(CJSON_SRCS:.c=.o)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/mcp_test.o $(CORE_SRCS:.c=.o) $(CJSON_SRCS:.c=.o)

tests/fsm_test: tests/fsm_test.o $(CORE_SRCS:.c=.o) $(CJSON_SRCS:.c=.o)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/fsm_test.o $(CORE_SRCS:.c=.o) $(CJSON_SRCS:.c=.o)

# Standalone test helper: linked by itself, no core deps. Built as
# a prerequisite of transport_stdio_test because the test invokes it.
tests/fake_echo: tests/fake_echo.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/fake_echo.o

# fake_mcp needs cJSON (it parses and emits JSON-RPC).
tests/fake_mcp: tests/fake_mcp.o $(CJSON_SRCS:.c=.o)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/fake_mcp.o $(CJSON_SRCS:.c=.o)

# fake_http_mcp: standalone HTTP MCP server used by the HTTP reload
# e2e. Needs cJSON for JSON parsing.
tests/fake_http_mcp: tests/fake_http_mcp.o $(CJSON_SRCS:.c=.o)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/fake_http_mcp.o $(CJSON_SRCS:.c=.o)

tests/transport_stdio_test: tests/transport_stdio_test.o \
                            $(CORE_SRCS:.c=.o) $(CJSON_SRCS:.c=.o) \
                            tests/fake_echo
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/transport_stdio_test.o \
	    $(CORE_SRCS:.c=.o) $(CJSON_SRCS:.c=.o)

# transport_http_test forks a tiny in-test HTTP MCP server from the
# same binary (no helper process), so it links only against the core
# library.
tests/transport_http_test: tests/transport_http_test.o \
                           $(CORE_SRCS:.c=.o) $(CJSON_SRCS:.c=.o)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/transport_http_test.o \
	    $(CORE_SRCS:.c=.o) $(CJSON_SRCS:.c=.o)

tests/dispatch_test: tests/dispatch_test.o $(CORE_SRCS:.c=.o) $(CJSON_SRCS:.c=.o)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/dispatch_test.o \
	    $(CORE_SRCS:.c=.o) $(CJSON_SRCS:.c=.o)

# daemon_client_test forks the real daemon binary, so it depends on
# the daemon having been built at the top level. The Makefile here
# has no way to build the Go daemon itself; the top-level Makefile's
# `test` target satisfies this by building `daemon` before running
# the wrapper tests.
tests/daemon_client_test: tests/daemon_client_test.o $(CORE_SRCS:.c=.o) $(CJSON_SRCS:.c=.o)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/daemon_client_test.o \
	    $(CORE_SRCS:.c=.o) $(CJSON_SRCS:.c=.o)

test: $(TEST_BINS) $(BIN) $(TEST_HELPERS)
	@set -e; for t in $(TEST_BINS); do \
	    echo "==> $$t"; \
	    ./$$t; \
	done
	@echo "==> tests/e2e_wrapper_test.sh"
	@./tests/e2e_wrapper_test.sh
	@echo "==> tests/e2e_reload_test.sh"
	@./tests/e2e_reload_test.sh
	@echo "==> tests/e2e_http_reload_test.sh"
	@./tests/e2e_http_reload_test.sh
	@echo "==> tests/e2e_http_session_invalid_test.sh"
	@./tests/e2e_http_session_invalid_test.sh
	@echo "==> tests/e2e_http_dead_upstream_test.sh"
	@./tests/e2e_http_dead_upstream_test.sh

clean:
	rm -f $(BIN) $(MAIN_OBJS) $(TEST_BINS) $(TEST_HELPERS) \
	      tests/*.o

install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)
