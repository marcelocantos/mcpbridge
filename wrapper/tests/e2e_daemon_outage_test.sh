#!/bin/sh
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# End-to-end test of the wrapper's tolerance of daemon outages.
# Exercises the contract documented in STABILITY.md's "Resilience
# guarantees" table for the daemon side: the wrapper survives the
# daemon disappearing and reappearing without dropping the agent's
# stdio session.
#
# Sequence:
#   1. Start daemon + wrapper, complete initialize + tools/list over
#      HTTP. Wrapper has registered with the daemon.
#   2. SIGTERM the daemon. Wrapper observes the socket close and
#      logs a daemon disconnect. The agent's stdio session must
#      stay alive.
#   3. Send another tools/list while the daemon is gone. It must
#      succeed — the daemon connection is non-essential to forwarding.
#   4. Restart the daemon on the same socket path. The wrapper's
#      reconnect-with-backoff must dial back in and re-handshake
#      (logged as "daemon connected").
#   5. Send a final tools/list to confirm the wrapper is still healthy
#      after the daemon round-trip.
#
# The wrapper must never exit during any of this. Any wrapper PID
# disappearance before the cleanup trap is a hard failure.

set -eu

WRAPPER=./mcpbridge
DAEMON=../daemon/mcpbridge-daemon
HTTP_SERVER=./tests/fake_http_mcp

for bin in "$WRAPPER" "$DAEMON" "$HTTP_SERVER"; do
    if [ ! -x "$bin" ]; then
        echo "daemon outage e2e: $bin not built" >&2
        exit 1
    fi
done

SOCK="/tmp/mcpb-daemon-outage-$$.sock"
WRAPPER_OUT=$(mktemp -t mcpbridge-daemon-outage.XXXXXX)
WRAPPER_ERR=$(mktemp -t mcpbridge-daemon-outage.XXXXXX)
DAEMON_ERR=$(mktemp -t mcpbridge-daemon-outage.XXXXXX)
SERVER_OUT=$(mktemp -t mcpbridge-daemon-outage.XXXXXX)
SERVER_ERR=$(mktemp -t mcpbridge-daemon-outage.XXXXXX)
INPUT_FIFO=$(mktemp -u -t mcpbridge-daemon-outage.XXXXXX)
CONFIG_FILE=$(mktemp -t mcpbridge-daemon-outage.XXXXXX.json)
mkfifo "$INPUT_FIFO"

cleanup() {
    [ -n "${WRAPPER_PID:-}" ] && kill -TERM "$WRAPPER_PID" 2>/dev/null || true
    [ -n "${DAEMON_PID:-}" ]  && kill -TERM "$DAEMON_PID"  2>/dev/null || true
    [ -n "${SERVER_PID:-}" ]  && kill -TERM "$SERVER_PID"  2>/dev/null || true
    wait 2>/dev/null || true
    if [ "${MCPBRIDGE_KEEP_E2E_LOGS:-}" = "1" ]; then
        echo "daemon outage e2e: logs preserved at $WRAPPER_OUT $WRAPPER_ERR $DAEMON_ERR $SERVER_OUT $SERVER_ERR $CONFIG_FILE" >&2
        return
    fi
    rm -f "$WRAPPER_OUT" "$WRAPPER_ERR" "$DAEMON_ERR" "$SERVER_OUT" "$SERVER_ERR" "$INPUT_FIFO" "$SOCK" "$CONFIG_FILE"
}
trap cleanup EXIT

# Start the fake upstream HTTP MCP server on an ephemeral port.
# Closing fds 3+ in the child so it doesn't inherit the input FIFO
# writer (see e2e_http_dead_upstream_test.sh for the same trap).
"$HTTP_SERVER" --port 0 -v >"$SERVER_OUT" 2>"$SERVER_ERR" 3<&- 3>&- &
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
    echo "daemon outage e2e: fake server did not report a port" >&2
    cat "$SERVER_ERR" >&2
    exit 1
fi
URL="http://127.0.0.1:$PORT/mcp"

# Start daemon (round 1).
MCPBRIDGE_SOCKET="$SOCK" "$DAEMON" -v 2>"$DAEMON_ERR" 3<&- 3>&- &
DAEMON_PID=$!
i=0
while [ $i -lt 100 ]; do
    [ -S "$SOCK" ] && break
    i=$((i+1))
    sleep 0.05
done
if [ ! -S "$SOCK" ]; then
    echo "daemon outage e2e: daemon did not create socket" >&2
    cat "$DAEMON_ERR" >&2
    exit 1
fi

# Schema-v2 config pointing at the fake HTTP upstream.
cat >"$CONFIG_FILE" <<EOF
{
  "schema": 2,
  "name": "fake-http-mcp",
  "url": "$URL",
  "source": {"type": "brew", "formula": "x/y/fake-http-mcp"}
}
EOF

# Start the wrapper.
MCPBRIDGE_SOCKET="$SOCK" "$WRAPPER" -v connect "$CONFIG_FILE" \
    <"$INPUT_FIFO" >"$WRAPPER_OUT" 2>"$WRAPPER_ERR" &
WRAPPER_PID=$!

exec 3<>"$INPUT_FIFO"

INIT='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"daemon-outage-e2e","version":"1"}}}'
INITED='{"jsonrpc":"2.0","method":"notifications/initialized"}'
LIST1='{"jsonrpc":"2.0","id":2,"method":"tools/list"}'
LIST2='{"jsonrpc":"2.0","id":3,"method":"tools/list"}'
LIST3='{"jsonrpc":"2.0","id":4,"method":"tools/list"}'

printf '%s\n' "$INIT"   >&3
printf '%s\n' "$INITED" >&3
printf '%s\n' "$LIST1"  >&3

# Allow handshake + first tools/list to complete.
sleep 0.6

# Confirm the wrapper logged a successful daemon registration so we
# know the connection was real before we sever it.
if ! grep -q 'daemon connected' "$WRAPPER_ERR"; then
    echo "daemon outage e2e: wrapper never logged daemon registration" >&2
    echo "--- wrapper stderr ---" >&2; cat "$WRAPPER_ERR" >&2
    exit 1
fi

# Kill the daemon mid-session. The wrapper must observe the socket
# close, log a disconnect, and stay alive.
kill -TERM "$DAEMON_PID"
wait "$DAEMON_PID" 2>/dev/null || true

# Sanity: wrapper still running.
if ! kill -0 "$WRAPPER_PID" 2>/dev/null; then
    echo "daemon outage e2e: wrapper exited when daemon died" >&2
    echo "--- wrapper stderr ---" >&2; cat "$WRAPPER_ERR" >&2
    exit 1
fi

# Send a tools/list while the daemon is gone. Must succeed — the
# daemon connection is non-essential.
printf '%s\n' "$LIST2" >&3
sleep 0.6

if ! grep -q '"id":3' "$WRAPPER_OUT"; then
    echo "daemon outage e2e: tools/list during daemon outage did not get a response" >&2
    echo "--- wrapper stdout ---" >&2; cat "$WRAPPER_OUT" >&2
    echo "--- wrapper stderr ---" >&2; cat "$WRAPPER_ERR" >&2
    exit 1
fi

# Restart the daemon on the same socket. The wrapper's
# reconnect-with-backoff (1s -> 5s) should dial back in within a
# few seconds.
MCPBRIDGE_SOCKET="$SOCK" "$DAEMON" -v 2>>"$DAEMON_ERR" 3<&- 3>&- &
DAEMON_PID=$!
i=0
while [ $i -lt 100 ]; do
    [ -S "$SOCK" ] && break
    i=$((i+1))
    sleep 0.05
done

# Wait long enough for the wrapper's backoff timer to elapse and the
# re-handshake to complete. Backoff cap is 5s, plus we want a margin.
sleep 6

# The wrapper's stderr must show two distinct "daemon connected"
# events (initial + reconnect) and at least one disconnect/socket
# close in between.
connect_count=$(grep -c 'daemon connected' "$WRAPPER_ERR" || true)
if [ "$connect_count" -lt 2 ]; then
    echo "daemon outage e2e: wrapper did not reconnect (saw $connect_count 'daemon connected' lines, want 2)" >&2
    echo "--- wrapper stderr ---" >&2; cat "$WRAPPER_ERR" >&2
    exit 1
fi
if ! grep -qE 'daemon socket closed|daemon disconnected' "$WRAPPER_ERR"; then
    echo "daemon outage e2e: wrapper did not log the daemon disconnect" >&2
    echo "--- wrapper stderr ---" >&2; cat "$WRAPPER_ERR" >&2
    exit 1
fi

# Final tools/list to confirm normal operation after the round-trip.
printf '%s\n' "$LIST3" >&3
sleep 0.6

if ! grep -q '"id":4' "$WRAPPER_OUT"; then
    echo "daemon outage e2e: tools/list after daemon reconnect did not get a response" >&2
    echo "--- wrapper stdout ---" >&2; cat "$WRAPPER_OUT" >&2
    echo "--- wrapper stderr ---" >&2; cat "$WRAPPER_ERR" >&2
    exit 1
fi

# Clean shutdown.
exec 3>&-
wait "$WRAPPER_PID" 2>/dev/null || true

echo "e2e_daemon_outage_test: ok"
