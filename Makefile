# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0

# Top-level Makefile — orchestrates the C wrapper under wrapper/ and
# the Go daemon under daemon/. Both subprojects build independently;
# this file just delegates. Deliberately boring.

VERSION ?= 0.0.0-dev
PREFIX  ?= /usr/local

.PHONY: all wrapper daemon test wrapper-test daemon-test clean install \
        wrapper-clean daemon-clean wrapper-install daemon-install

all: wrapper daemon

wrapper:
	$(MAKE) -C wrapper VERSION=$(VERSION)

daemon:
	cd daemon && go build -ldflags "-X main.Version=$(VERSION)" -o mcpbridge-daemon ./cmd/mcpbridge-daemon

test: wrapper-test daemon-test

# wrapper-test depends on the daemon binary: the integration test
# tests/daemon_client_test fork/execs the real mcpbridge-daemon.
wrapper-test: daemon
	$(MAKE) -C wrapper test VERSION=$(VERSION)

daemon-test:
	cd daemon && go test ./...

clean: wrapper-clean daemon-clean

wrapper-clean:
	$(MAKE) -C wrapper clean

daemon-clean:
	rm -f daemon/mcpbridge-daemon

install: wrapper-install daemon-install

wrapper-install: wrapper
	$(MAKE) -C wrapper install PREFIX=$(PREFIX)

daemon-install: daemon
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 daemon/mcpbridge-daemon $(DESTDIR)$(PREFIX)/bin/mcpbridge-daemon
