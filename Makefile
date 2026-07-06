# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0

# Top-level Makefile — orchestrates the C wrapper under wrapper/ and
# the Go daemon under daemon/. Both subprojects build independently;
# this file just delegates. Deliberately boring.

VERSION ?= 0.0.0-dev
PREFIX  ?= /usr/local

.PHONY: all wrapper daemon test wrapper-test daemon-test clean install \
        wrapper-clean daemon-clean wrapper-install daemon-install bullseye

all: wrapper daemon

wrapper:
	$(MAKE) -C wrapper VERSION=$(VERSION)

daemon: daemon/cmd/mcpbridge-daemon/help_agent.md
	cd daemon && go build -ldflags "-X main.Version=$(VERSION)" -o mcpbridge-daemon ./cmd/mcpbridge-daemon

# daemon/cmd/mcpbridge-daemon/main.go embeds agents-guide.md via
# //go:embed help_agent.md. The embed directive can only see files
# in the same package dir, so we stage a copy there before building.
# The file is gitignored — the copy is a build artifact, not source.
daemon/cmd/mcpbridge-daemon/help_agent.md: agents-guide.md
	cp agents-guide.md $@

test: wrapper-test daemon-test

# wrapper-test depends on the daemon binary: the integration test
# tests/daemon_client_test fork/execs the real mcpbridge-daemon.
wrapper-test: daemon
	$(MAKE) -C wrapper test VERSION=$(VERSION)

# -race is load-bearing here: the daemon's concurrency contract (push
# methods vs the per-connection serve goroutine) is only enforced by
# the race detector. Without it, data races on shared *conn state ship
# silently (see docs/audit/fable-2026-07.md F3). Both CI runners
# (ubuntu, macos) support -race.
daemon-test:
	cd daemon && go test -race ./...

clean: wrapper-clean daemon-clean

wrapper-clean:
	$(MAKE) -C wrapper clean

daemon-clean:
	rm -f daemon/mcpbridge-daemon daemon/cmd/mcpbridge-daemon/help_agent.md

install: wrapper-install daemon-install

wrapper-install: wrapper
	$(MAKE) -C wrapper install PREFIX=$(PREFIX)

daemon-install: daemon
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 daemon/mcpbridge-daemon $(DESTDIR)$(PREFIX)/bin/mcpbridge-daemon

# Standing-invariants hook for `bullseye_convergence` / /cv.
# Exit 0 = all invariants green; non-zero = at least one violation.
# Test output is suppressed on success and shown on failure so the
# bullseye summary stays readable.
bullseye:
	@cd daemon && go vet ./... && echo "✓ go vet"
	@cd daemon && gofmt -l . | (! grep .) && echo "✓ gofmt"
	@out=$$(mktemp); \
	 if $(MAKE) -s test >$$out 2>&1; then \
	   echo "✓ tests"; rm -f $$out; \
	 else \
	   echo "✗ tests"; cat $$out; rm -f $$out; exit 1; \
	 fi
	@test -z "$$(git status --porcelain)" && echo "✓ clean tree" || \
	 (echo "✗ dirty tree"; git status --short; exit 1)
