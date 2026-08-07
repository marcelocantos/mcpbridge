#!/bin/sh
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# End-to-end test for 🎯T21: a backend cycle must never strand the
# agent's session.
#
# e2e_reload_test.sh reloads while the session is idle. This one
# reloads while a request is genuinely IN FLIGHT — the case that bit
# us in the field: a slow tool call was outstanding when the daemon
# broadcast a reload, the child was replaced, and the response for
# that id never arrived. The agent waited forever, and because the
# wrapper stayed in DRAINING waiting for an in-flight count that
# could no longer reach zero, every later request queued behind it.
# The whole session was dead until the agent restarted.
#
# Two assertions, in order of importance:
#   1. The in-flight request gets an answer — result or JSON-RPC
#      error, we do not care which, but SOMETHING with that id.
#   2. The session survives: a request issued after the reload is
#      answered normally.
#
# Invoked from wrapper/ by `make test`.

set -eu

WRAPPER=./mcpbridge
DAEMON=../daemon/mcpbridge-daemon
CHILD=./tests/fake_mcp

for bin in "$WRAPPER" "$DAEMON" "$CHILD"; do
    if [ ! -x "$bin" ]; then
        echo "inflight e2e: $bin not built" >&2
        exit 1
    fi
done

SOCK="/tmp/mcpb-inflight-$$.sock"
WRAPPER_OUT=$(mktemp -t mcpbridge-inflight.XXXXXX)
WRAPPER_ERR=$(mktemp -t mcpbridge-inflight.XXXXXX)
DAEMON_ERR=$(mktemp -t mcpbridge-inflight.XXXXXX)
INPUT_FIFO=$(mktemp -u -t mcpbridge-inflight.XXXXXX)
CONFIG_FILE=$(mktemp -t mcpbridge-inflight.XXXXXX.json)
mkfifo "$INPUT_FIFO"

CHILD_ABS=$(cd "$(dirname "$CHILD")" && pwd)/$(basename "$CHILD")
cat >"$CONFIG_FILE" <<EOF
{
  "schema": 2,
  "name": "fake-mcp",
  "command": "$CHILD_ABS",
  "source": {"type": "brew", "formula": "x/y/fake-mcp"}
}
EOF

cleanup() {
    [ -n "${WRAPPER_PID:-}" ] && kill -TERM "$WRAPPER_PID" 2>/dev/null || true
    [ -n "${DAEMON_PID:-}" ]  && kill -TERM "$DAEMON_PID"  2>/dev/null || true
    wait 2>/dev/null || true
    if [ "${MCPBRIDGE_KEEP_E2E_LOGS:-}" = "1" ]; then
        echo "inflight e2e: logs preserved at $WRAPPER_OUT $WRAPPER_ERR $DAEMON_ERR" >&2
        return
    fi
    rm -f "$WRAPPER_OUT" "$WRAPPER_ERR" "$DAEMON_ERR" "$INPUT_FIFO" "$SOCK" "$CONFIG_FILE"
}
trap cleanup EXIT

fail() {
    echo "inflight e2e: $1" >&2
    echo "--- wrapper stdout ---" >&2
    cat "$WRAPPER_OUT" >&2
    echo "--- wrapper stderr ---" >&2
    cat "$WRAPPER_ERR" >&2
    echo "--- daemon stderr ---" >&2
    cat "$DAEMON_ERR" >&2
    exit 1
}

MCPBRIDGE_SOCKET="$SOCK" "$DAEMON" -v 2>"$DAEMON_ERR" &
DAEMON_PID=$!

i=0
while [ $i -lt 100 ]; do
    [ -S "$SOCK" ] && break
    i=$((i+1))
    sleep 0.05
done
[ -S "$SOCK" ] || fail "daemon did not create socket"

# The child sleeps 1.5s inside tools/call, so the reload below lands
# while the request is in flight.
FAKE_MCP_CALL_DELAY_MS=1500 MCPBRIDGE_SOCKET="$SOCK" \
    "$WRAPPER" -v connect "$CONFIG_FILE" \
    <"$INPUT_FIFO" >"$WRAPPER_OUT" 2>"$WRAPPER_ERR" &
WRAPPER_PID=$!

exec 3<>"$INPUT_FIFO"

INIT='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"inflight-e2e","version":"1"}}}'
INITED='{"jsonrpc":"2.0","method":"notifications/initialized"}'
SLOW='{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"echo","arguments":{}}}'
AFTER='{"jsonrpc":"2.0","id":3,"method":"tools/list"}'

printf '%s\n' "$INIT"   >&3
printf '%s\n' "$INITED" >&3
sleep 0.4

# Fire the slow call, then reload while it is still outstanding.
printf '%s\n' "$SLOW" >&3
sleep 0.3
kill -HUP "$DAEMON_PID"

# Generous: the whole point is that the agent is not left waiting.
# The slow call itself only takes 1.5s.
sleep 4

if ! grep -q '"id":2' "$WRAPPER_OUT"; then
    fail "no response for the in-flight request (id 2) — the agent would wait forever"
fi

# Now prove the session still works.
printf '%s\n' "$AFTER" >&3
sleep 2

if ! grep -q '"id":3' "$WRAPPER_OUT"; then
    fail "no response for the post-reload request (id 3) — session stranded"
fi

exec 3>&-
wait "$WRAPPER_PID" 2>/dev/null || true

echo "e2e_reload_inflight_test: ok"
