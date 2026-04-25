#!/bin/sh
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# End-to-end smoke test for the mcpbridge wrapper.
#
# Drives a full MCP handshake (initialize + notifications/initialized)
# followed by a tools/list through the built wrapper wrapping fake_mcp,
# and verifies the responses come back intact.
#
# Invoked from wrapper/ by `make test`; all paths are relative to
# that directory.

set -eu

BIN=./mcpbridge
CHILD=./tests/fake_mcp

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
CFG=$(mktemp -t mcpbridge-e2e.XXXXXX.json)
trap 'rm -f "$OUT" "$ERR" "$CFG"' EXIT

# Build a schema:2 stdio config pointing at fake_mcp. Brew/source is
# present for daemon validation but irrelevant here — no daemon runs.
CHILD_ABS=$(cd "$(dirname "$CHILD")" && pwd)/$(basename "$CHILD")
cat >"$CFG" <<EOF
{
  "schema": 2,
  "name": "fake-mcp",
  "command": "$CHILD_ABS",
  "source": {"type": "brew", "formula": "x/y/fake-mcp"}
}
EOF

INIT='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"e2e","version":"1"}}}'
INITED='{"jsonrpc":"2.0","method":"notifications/initialized"}'
LIST='{"jsonrpc":"2.0","id":2,"method":"tools/list"}'

# The sleep keeps the input pipe open briefly after the last write
# so mcpbridge sees the child's responses before observing stdin EOF.
# 1.0s is generous; the whole round-trip is typically sub-millisecond.
{
    printf '%s\n' "$INIT"
    printf '%s\n' "$INITED"
    printf '%s\n' "$LIST"
    sleep 1.0
} | "$BIN" connect "$CFG" >"$OUT" 2>"$ERR" || {
    echo "e2e: wrapper exited non-zero" >&2
    cat "$ERR" >&2
    exit 1
}

# Check that both responses came back. We do not pin exact JSON
# because cJSON_PrintUnformatted's field ordering is not a contract.
if ! grep -q '"id":1' "$OUT" || ! grep -q '"serverInfo"' "$OUT"; then
    echo "e2e: initialize response not seen" >&2
    cat "$OUT" >&2
    cat "$ERR" >&2
    exit 1
fi
if ! grep -q '"id":2' "$OUT" || ! grep -q '"tools"' "$OUT"; then
    echo "e2e: tools/list response not seen" >&2
    cat "$OUT" >&2
    cat "$ERR" >&2
    exit 1
fi

echo "e2e_wrapper_test: ok"
