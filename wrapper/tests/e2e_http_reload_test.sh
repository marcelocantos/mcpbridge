#!/bin/sh
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# End-to-end test of the HTTP backend + reload cycle:
#
#   agent --(stdio)--> mcpbridge --(http/POST+SSE)--> fake_http_mcp
#                       |
#                       +--(uds)--> mcpbridge-daemon
#
# The test drives:
#   1. A normal initialize + initialized + tools/list over HTTP
#   2. SIGHUP to the daemon, which broadcasts a reload notification
#      that the wrapper handles by stopping the HTTP transport
#      (closing the self-pipe + dropping the session id), starting
#      a fresh one, replaying the cached initialize + initialized
#      against the (still-running) fake_http_mcp, and sending
#      reload_ack to the daemon.
#   3. A second tools/list that must succeed under the new session id
#
# fake_http_mcp runs unchanged across the reload — the reload cycle
# is a wrapper-side operation (drop session_id, re-dial). This proves
# the FSM + dispatch path works identically against HTTP.
#
# Invoked from wrapper/ by `make test`. Both wrapper and daemon
# binaries plus fake_http_mcp must already be built.

set -eu

WRAPPER=./mcpbridge
DAEMON=../daemon/mcpbridge-daemon
HTTP_SERVER=./tests/fake_http_mcp

for bin in "$WRAPPER" "$DAEMON" "$HTTP_SERVER"; do
    if [ ! -x "$bin" ]; then
        echo "http reload e2e: $bin not built" >&2
        exit 1
    fi
done

SOCK="/tmp/mcpb-http-reload-$$.sock"
WRAPPER_OUT=$(mktemp -t mcpbridge-http-reload.XXXXXX)
WRAPPER_ERR=$(mktemp -t mcpbridge-http-reload.XXXXXX)
DAEMON_ERR=$(mktemp -t mcpbridge-http-reload.XXXXXX)
SERVER_OUT=$(mktemp -t mcpbridge-http-reload.XXXXXX)
SERVER_ERR=$(mktemp -t mcpbridge-http-reload.XXXXXX)
INPUT_FIFO=$(mktemp -u -t mcpbridge-http-reload.XXXXXX)
CONFIG_FILE=$(mktemp -t mcpbridge-http-reload.XXXXXX.json)
mkfifo "$INPUT_FIFO"

cleanup() {
    [ -n "${WRAPPER_PID:-}" ] && kill -TERM "$WRAPPER_PID" 2>/dev/null || true
    [ -n "${DAEMON_PID:-}" ]  && kill -TERM "$DAEMON_PID"  2>/dev/null || true
    [ -n "${SERVER_PID:-}" ]  && kill -TERM "$SERVER_PID"  2>/dev/null || true
    wait 2>/dev/null || true
    if [ "${MCPBRIDGE_KEEP_E2E_LOGS:-}" = "1" ]; then
        echo "http reload e2e: logs preserved at $WRAPPER_OUT $WRAPPER_ERR $DAEMON_ERR $SERVER_OUT $SERVER_ERR $CONFIG_FILE" >&2
        return
    fi
    rm -f "$WRAPPER_OUT" "$WRAPPER_ERR" "$DAEMON_ERR" "$SERVER_OUT" "$SERVER_ERR" "$INPUT_FIFO" "$SOCK" "$CONFIG_FILE"
}
trap cleanup EXIT

# Start the fake HTTP MCP server on an ephemeral port. It prints the
# bound port to stdout as its first line. Redirect stdout to a file
# we can read back.
"$HTTP_SERVER" --port 0 -v >"$SERVER_OUT" 2>"$SERVER_ERR" &
SERVER_PID=$!

# Wait up to 2s for the port line to appear.
PORT=""
i=0
while [ $i -lt 40 ]; do
    if [ -s "$SERVER_OUT" ]; then
        PORT=$(head -n 1 "$SERVER_OUT")
        [ -n "$PORT" ] && break
    fi
    i=$((i+1))
    sleep 0.05
done
if [ -z "$PORT" ]; then
    echo "http reload e2e: fake server did not report a port" >&2
    cat "$SERVER_ERR" >&2
    exit 1
fi
URL="http://127.0.0.1:$PORT/mcp"
echo "http reload e2e: fake server on $URL" >&2

# Start the daemon.
MCPBRIDGE_SOCKET="$SOCK" "$DAEMON" -v 2>"$DAEMON_ERR" &
DAEMON_PID=$!

# Wait up to 2s for the socket to appear.
i=0
while [ $i -lt 40 ]; do
    [ -S "$SOCK" ] && break
    i=$((i+1))
    sleep 0.05
done
if [ ! -S "$SOCK" ]; then
    echo "http reload e2e: daemon did not create socket" >&2
    cat "$DAEMON_ERR" >&2
    exit 1
fi

# Build a schema:2 config pointing at the fake HTTP server.
cat >"$CONFIG_FILE" <<EOF
{
  "schema": 2,
  "name": "fake-http-mcp",
  "url": "$URL",
  "source": {"type": "brew", "formula": "x/y/fake-http-mcp"}
}
EOF

# Start the wrapper in HTTP mode (backend selected by the config file).
MCPBRIDGE_SOCKET="$SOCK" "$WRAPPER" -v connect "$CONFIG_FILE" \
    <"$INPUT_FIFO" >"$WRAPPER_OUT" 2>"$WRAPPER_ERR" &
WRAPPER_PID=$!

# Keep a writer on the FIFO so the wrapper's stdin doesn't see EOF
# prematurely. See the stdio reload e2e for the detailed rationale.
exec 3<>"$INPUT_FIFO"

INIT='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"http-reload-e2e","version":"1"}}}'
INITED='{"jsonrpc":"2.0","method":"notifications/initialized"}'
LIST1='{"jsonrpc":"2.0","id":2,"method":"tools/list"}'
LIST2='{"jsonrpc":"2.0","id":3,"method":"tools/list"}'

printf '%s\n' "$INIT"   >&3
printf '%s\n' "$INITED" >&3
printf '%s\n' "$LIST1"  >&3

sleep 0.5

# Trigger the reload broadcast.
kill -HUP "$DAEMON_PID"

sleep 0.8

# Second tools/list should hit the re-established HTTP session.
printf '%s\n' "$LIST2" >&3

sleep 0.5

# Close the FIFO so the wrapper sees EOF and exits cleanly.
exec 3>&-

wait "$WRAPPER_PID" 2>/dev/null || true

# Diagnostic block: on any failure below, dump everything.
dump_and_exit() {
    echo "--- wrapper stdout ---" >&2
    cat "$WRAPPER_OUT" >&2
    echo "--- wrapper stderr ---" >&2
    cat "$WRAPPER_ERR" >&2
    echo "--- daemon stderr ---" >&2
    cat "$DAEMON_ERR" >&2
    echo "--- server stderr ---" >&2
    cat "$SERVER_ERR" >&2
    exit 1
}

# Both tools/list responses must be present on the wrapper's stdout.
if ! grep -q '"id":2' "$WRAPPER_OUT"; then
    echo "http reload e2e: first tools/list response not observed" >&2
    dump_and_exit
fi
if ! grep -q '"id":3' "$WRAPPER_OUT"; then
    echo "http reload e2e: second tools/list response not observed" >&2
    dump_and_exit
fi

# Exactly one initialize response: the replayed initialize must be
# consumed by dispatch, not forwarded upstream.
init_count=$(grep -c '"serverInfo"' "$WRAPPER_OUT" || true)
if [ "$init_count" != "1" ]; then
    echo "http reload e2e: expected 1 initialize response, saw $init_count" >&2
    dump_and_exit
fi

# list_changed notifications for all three namespaces must be emitted
# after the reload. This is the same contract as the stdio reload.
for kind in tools prompts resources; do
    if ! grep -q "\"notifications/${kind}/list_changed\"" "$WRAPPER_OUT"; then
        echo "http reload e2e: ${kind}/list_changed notification missing" >&2
        dump_and_exit
    fi
done

# Daemon log: SIGHUP broadcast + reload_ack.
if ! grep -q 'SIGHUP' "$DAEMON_ERR"; then
    echo "http reload e2e: daemon didn't log SIGHUP broadcast" >&2
    dump_and_exit
fi
if ! grep -q 'reload_ack' "$DAEMON_ERR"; then
    echo "http reload e2e: daemon didn't log reload_ack" >&2
    dump_and_exit
fi

echo "e2e_http_reload_test: ok"
