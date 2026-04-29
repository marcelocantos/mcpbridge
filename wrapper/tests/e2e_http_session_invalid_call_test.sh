#!/bin/sh
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# Companion to e2e_http_session_invalid_test.sh — verifies the
# replay-safety contract for HTTP-backend session-loss recovery
# (🎯T9). When the upstream rejects an in-flight side-effecting
# request (here: tools/call) with a session-invalid 4xx, mcpbridge
# MUST surface a structured JSON-RPC error to the agent rather than
# silently re-issue the call. Idempotent reads (covered by the
# sibling test) are still replayed transparently.
#
# Sequence:
#   1. initialize + initialized + tools/list  (succeeds)
#   2. SIGUSR1 to fake_http_mcp → rotates the session id
#   3. tools/call (id=3) — POSTs with the now-invalid session id;
#      fake_http_mcp returns 400; mcpbridge detects ESTALE, but
#      because tools/call is NOT in the replay-safe whitelist, it
#      synthesises a -32002 error response to the agent and still
#      kicks off the cycle so the next call lands cleanly
#   4. tools/list (id=4) — POSTs with the new session id, succeeds
#
# Verifications:
#   - Response for id=2 is a normal tools/list result
#   - Response for id=3 is an error with code -32002 (no silent retry)
#   - Response for id=4 is a normal tools/list result
#   - Wrapper logs `upstream: cycling — session stale` and the
#     `not safe to replay` rationale, plus the `complete` marker

set -eu

WRAPPER=./mcpbridge
HTTP_SERVER=./tests/fake_http_mcp

for bin in "$WRAPPER" "$HTTP_SERVER"; do
    if [ ! -x "$bin" ]; then
        echo "http session invalid call e2e: $bin not built" >&2
        exit 1
    fi
done

WRAPPER_OUT=$(mktemp -t mcpbridge-http-stale-call.XXXXXX)
WRAPPER_ERR=$(mktemp -t mcpbridge-http-stale-call.XXXXXX)
SERVER_OUT=$(mktemp -t mcpbridge-http-stale-call.XXXXXX)
SERVER_ERR=$(mktemp -t mcpbridge-http-stale-call.XXXXXX)
INPUT_FIFO=$(mktemp -u -t mcpbridge-http-stale-call.XXXXXX)
CONFIG_FILE=$(mktemp -t mcpbridge-http-stale-call.XXXXXX.json)
SOCK="/tmp/mcpb-http-stale-call-$$.sock"
mkfifo "$INPUT_FIFO"

cleanup() {
    [ -n "${WRAPPER_PID:-}" ] && kill -TERM "$WRAPPER_PID" 2>/dev/null || true
    [ -n "${SERVER_PID:-}" ]  && kill -TERM "$SERVER_PID"  2>/dev/null || true
    wait 2>/dev/null || true
    if [ "${MCPBRIDGE_KEEP_E2E_LOGS:-}" = "1" ]; then
        echo "http session invalid call e2e: logs preserved at $WRAPPER_OUT $WRAPPER_ERR $SERVER_OUT $SERVER_ERR $CONFIG_FILE" >&2
        return
    fi
    rm -f "$WRAPPER_OUT" "$WRAPPER_ERR" "$SERVER_OUT" "$SERVER_ERR" "$INPUT_FIFO" "$CONFIG_FILE" "$SOCK"
}
trap cleanup EXIT

"$HTTP_SERVER" --port 0 -v >"$SERVER_OUT" 2>"$SERVER_ERR" &
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
    echo "http session invalid call e2e: fake server did not report a port" >&2
    cat "$SERVER_ERR" >&2
    exit 1
fi
URL="http://127.0.0.1:$PORT/mcp"
echo "http session invalid call e2e: fake server on $URL" >&2

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

INIT='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"http-stale-call-e2e","version":"1"}}}'
INITED='{"jsonrpc":"2.0","method":"notifications/initialized"}'
LIST1='{"jsonrpc":"2.0","id":2,"method":"tools/list"}'
CALL='{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"launch_app","arguments":{"bundleId":"com.example.app"}}}'
LIST2='{"jsonrpc":"2.0","id":4,"method":"tools/list"}'

printf '%s\n' "$INIT"   >&3
printf '%s\n' "$INITED" >&3
printf '%s\n' "$LIST1"  >&3

sleep 0.5

kill -USR1 "$SERVER_PID"
sleep 0.1

printf '%s\n' "$CALL" >&3

# Allow recovery to complete before the next read.
sleep 1.0

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
    echo "http session invalid call e2e: response for id=2 not observed" >&2
    dump_and_exit
fi

# id=3 must come back as a structured error with -32002 (mcpbridge
# session-reset error code), NOT as a tools/call result. The agent
# is responsible for retrying the side-effecting call if it wants.
if ! grep -q '"id":3' "$WRAPPER_OUT"; then
    echo "http session invalid call e2e: response for id=3 not observed" >&2
    dump_and_exit
fi
if ! grep '"id":3' "$WRAPPER_OUT" | grep -q '"error"'; then
    echo "http session invalid call e2e: id=3 should be a structured error" >&2
    dump_and_exit
fi
if ! grep '"id":3' "$WRAPPER_OUT" | grep -q '"code":-32002'; then
    echo "http session invalid call e2e: id=3 missing the -32002 code" >&2
    dump_and_exit
fi

# id=4 must succeed under the new session.
if ! grep -q '"id":4' "$WRAPPER_OUT"; then
    echo "http session invalid call e2e: response for id=4 not observed" >&2
    dump_and_exit
fi

# Cycle log markers — both bookends must appear.
if ! grep -q 'upstream: cycling.*session stale' "$WRAPPER_ERR"; then
    echo "http session invalid call e2e: missing cycle-start log" >&2
    dump_and_exit
fi
if ! grep -q 'not safe to replay' "$WRAPPER_ERR"; then
    echo "http session invalid call e2e: missing replay-skip rationale" >&2
    dump_and_exit
fi
if ! grep -q 'upstream: cycling.*complete' "$WRAPPER_ERR"; then
    echo "http session invalid call e2e: missing cycle-complete log" >&2
    dump_and_exit
fi

echo "e2e_http_session_invalid_call_test: ok"
