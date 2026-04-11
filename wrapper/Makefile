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
    src/util.c

MAIN_SRCS  = src/main.c $(CORE_SRCS) $(WATCH_SRC)
CJSON_SRCS = vendor/cjson/cJSON.c

MAIN_OBJS  = $(MAIN_SRCS:.c=.o) $(CJSON_SRCS:.c=.o)

BIN        = mcpbridge
TEST_BINS  = tests/smoke_test

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
tests/smoke_test: tests/smoke_test.o $(CORE_SRCS:.c=.o)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/smoke_test.o $(CORE_SRCS:.c=.o)

test: $(TEST_BINS)
	@set -e; for t in $(TEST_BINS); do \
	    echo "==> $$t"; \
	    ./$$t; \
	done

clean:
	rm -f $(BIN) $(MAIN_OBJS) $(TEST_BINS) \
	      tests/*.o

install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)
