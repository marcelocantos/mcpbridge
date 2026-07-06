#!/bin/sh
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# Fable-5 F5 / T17: a per-request HTTP upstream failure (here: a 503
# on a single tools/call from a transiently-unhealthy backend) must be
# surfaced to the agent as a JSON-RPC error for THAT request and must
# NOT kill the whole wrapper / tear down the agent's MCP session.
#
# Before the fix, transport_http maps the 503 to errno=EPROTO,
# sink_send_child treats any non-ESTALE/non-ETIMEDOUT errno as FATAL
# and sets exit_requested, and the wrapper exits with no response for
# the call — dropping the agent's request and the whole session.
#
# Sequence:
#   1. initialize + initialized + tools/list (id=2)   → succeeds
#   2. tools/call (id=3)  → backend answers HTTP 503
#   3. tools/list (id=4)  → succeeds (proves the wrapper survived)
#
# Verifications:
#   - id=2 → normal result
#   - id=3 → a JSON-RPC error (not a silent drop)
#   - id=4 → normal result (the wrapper is still alive and serving)

set -eu

WRAPPER=./mcpbridge
HTTP_SERVER=./tests/fake_http_mcp

for bin in "$WRAPPER" "$HTTP_SERVER"; do
    if [ ! -x "$bin" ]; then
        echo "http upstream error e2e: $bin not built" >&2
        exit 1
    fi
done

WRAPPER_OUT=$(mktemp -t mcpbridge-http-uerr.XXXXXX)
WRAPPER_ERR=$(mktemp -t mcpbridge-http-uerr.XXXXXX)
SERVER_OUT=$(mktemp -t mcpbridge-http-uerr.XXXXXX)
SERVER_ERR=$(mktemp -t mcpbridge-http-uerr.XXXXXX)
INPUT_FIFO=$(mktemp -u -t mcpbridge-http-uerr.XXXXXX)
CONFIG_FILE=$(mktemp -t mcpbridge-http-uerr.XXXXXX.json)
SOCK="/tmp/mcpb-http-uerr-$$.sock"
mkfifo "$INPUT_FIFO"

cleanup() {
    [ -n "${WRAPPER_PID:-}" ] && kill -TERM "$WRAPPER_PID" 2>/dev/null || true
    [ -n "${SERVER_PID:-}" ]  && kill -TERM "$SERVER_PID"  2>/dev/null || true
    wait 2>/dev/null || true
    if [ "${MCPBRIDGE_KEEP_E2E_LOGS:-}" = "1" ]; then
        echo "http upstream error e2e: logs preserved at $WRAPPER_OUT $WRAPPER_ERR $SERVER_OUT $SERVER_ERR $CONFIG_FILE" >&2
        return
    fi
    rm -f "$WRAPPER_OUT" "$WRAPPER_ERR" "$SERVER_OUT" "$SERVER_ERR" "$INPUT_FIFO" "$CONFIG_FILE" "$SOCK"
}
trap cleanup EXIT

# Backend answers 503 to any tools/call; everything else is normal.
"$HTTP_SERVER" --port 0 --fail-5xx-method tools/call -v \
    >"$SERVER_OUT" 2>"$SERVER_ERR" &
SERVER_PID=$!

PORT=""
i=0
while [ $i -lt 100 ]; do
    if [ -s "$SERVER_OUT" ]; then
        PORT=$(head -n 1 "$SERVER_OUT")
        [ -n "$PORT" ] && break
    fi
    i=$((i+1))
    sleep 0.05
done
if [ -z "$PORT" ]; then
    echo "http upstream error e2e: fake server did not report a port" >&2
    cat "$SERVER_ERR" >&2
    exit 1
fi
URL="http://127.0.0.1:$PORT/mcp"
echo "http upstream error e2e: fake server on $URL" >&2

cat >"$CONFIG_FILE" <<EOF
{
  "schema": 2,
  "name": "fake-http-mcp",
  "url": "$URL",
  "source": {"type": "brew", "formula": "x/y/fake-http-mcp"}
}
EOF

MCPBRIDGE_SOCKET="$SOCK" "$WRAPPER" -v connect "$CONFIG_FILE" \
    <"$INPUT_FIFO" >"$WRAPPER_OUT" 2>"$WRAPPER_ERR" &
WRAPPER_PID=$!

exec 3<>"$INPUT_FIFO"

INIT='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"http-uerr-e2e","version":"1"}}}'
INITED='{"jsonrpc":"2.0","method":"notifications/initialized"}'
LIST1='{"jsonrpc":"2.0","id":2,"method":"tools/list"}'
CALL='{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"do_thing","arguments":{}}}'
LIST2='{"jsonrpc":"2.0","id":4,"method":"tools/list"}'

printf '%s\n' "$INIT"   >&3
printf '%s\n' "$INITED" >&3
printf '%s\n' "$LIST1"  >&3

sleep 0.5

printf '%s\n' "$CALL" >&3

sleep 0.5

printf '%s\n' "$LIST2" >&3

sleep 0.5

exec 3>&-

wait "$WRAPPER_PID" 2>/dev/null || true

dump_and_exit() {
    echo "--- wrapper stdout ---" >&2
    cat "$WRAPPER_OUT" >&2
    echo "--- wrapper stderr ---" >&2
    cat "$WRAPPER_ERR" >&2
    echo "--- server stderr ---" >&2
    cat "$SERVER_ERR" >&2
    exit 1
}

# id=2 must succeed.
if ! grep -q '"id":2' "$WRAPPER_OUT"; then
    echo "http upstream error e2e: response for id=2 not observed" >&2
    dump_and_exit
fi

# id=3 must come back as a structured error, NOT be silently dropped.
if ! grep -q '"id":3' "$WRAPPER_OUT"; then
    echo "http upstream error e2e: response for id=3 not observed (request silently dropped / wrapper died)" >&2
    dump_and_exit
fi
if ! grep '"id":3' "$WRAPPER_OUT" | grep -q '"error"'; then
    echo "http upstream error e2e: id=3 should be a structured JSON-RPC error" >&2
    dump_and_exit
fi

# id=4 must succeed — proof the wrapper survived the 503 and is still
# serving the same MCP session.
if ! grep -q '"id":4' "$WRAPPER_OUT"; then
    echo "http upstream error e2e: response for id=4 not observed (wrapper did not survive the per-request failure)" >&2
    dump_and_exit
fi

echo "e2e_http_upstream_error_test: ok"
