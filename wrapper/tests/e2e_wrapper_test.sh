#!/bin/sh
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# End-to-end smoke test for the mcpbridge wrapper.
#
# Spawns the built wrapper with fake_echo as the wrapped child, pipes
# a JSON-RPC line in, and verifies the identical bytes come back out.
# Deliberately simple: no JSON parsing, no fancy harness, no timeouts
# beyond a short sleep to keep the input pipe open while the child
# echoes.
#
# Invoked from wrapper/ by `make test`; all paths are relative to
# that directory.

set -eu

BIN=./mcpbridge
CHILD=./tests/fake_echo

if [ ! -x "$BIN" ]; then
    echo "e2e: $BIN not built" >&2
    exit 1
fi
if [ ! -x "$CHILD" ]; then
    echo "e2e: $CHILD not built" >&2
    exit 1
fi

OUT=$(mktemp -t mcpbridge-e2e.XXXXXX)
ERR=$(mktemp -t mcpbridge-e2e.XXXXXX)
trap 'rm -f "$OUT" "$ERR"' EXIT

PAYLOAD='{"jsonrpc":"2.0","id":1,"method":"tools/list"}'

# The sleep keeps the input pipe open briefly so mcpbridge sees the
# child's echo before observing stdin EOF.
{ printf '%s\n' "$PAYLOAD"; sleep 0.5; } | "$BIN" -- "$CHILD" >"$OUT" 2>"$ERR" || {
    echo "e2e: wrapper exited non-zero" >&2
    cat "$ERR" >&2
    exit 1
}

if ! grep -qxF "$PAYLOAD" "$OUT"; then
    echo "e2e: payload not round-tripped" >&2
    echo "--- expected ---" >&2
    echo "$PAYLOAD" >&2
    echo "--- got ---" >&2
    cat "$OUT" >&2
    echo "--- stderr ---" >&2
    cat "$ERR" >&2
    exit 1
fi

echo "e2e_wrapper_test: ok"
