#!/bin/sh
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# End-to-end test of mcpbridge's autonomous recovery from upstream
# session loss — the canonical 🎯T7 scenario:
#
#   agent --(stdio)--> mcpbridge --(http/POST+SSE)--> fake_http_mcp
#
# Unlike e2e_http_reload_test.sh, this test does NOT involve the
# daemon. The daemon-broadcast reload path is already covered there.
# Here we simulate what happens when the upstream MCP server
# autonomously restarts (or otherwise loses our session id) and the
# next POST comes back with `400 Bad Request: Invalid session ID` —
# the wrapper has to recover transparently without the daemon ever
# having said anything about a reload.
#
# Sequence:
#   1. initialize + initialized + tools/list (succeeds; session id captured)
#   2. SIGUSR1 to fake_http_mcp → it rotates its current session id
#   3. tools/list (id=3) — POSTs with the now-invalid session id;
#      fake_http_mcp returns 400; mcpbridge detects ESTALE, undoes
#      the in-flight increment, queues the request, and emits
#      RELOAD_REQUESTED
#   4. The wrapper transitions DRAINING -> SWAPPING -> STARTING,
#      replays initialize against the upstream (capturing the new
#      session id), and on RUNNING drains the queue — id=3 reaches
#      the server under the new session and a response flows back
#   5. tools/list (id=4) — POSTs with the new session id, succeeds
#
# The agent must see exactly one initialize response (the original;
# the replayed one is consumed by dispatch) and responses for ids
# 2, 3, AND 4. Any test failure indicates the wrapper is not actually
# transparent across upstream session loss.

set -eu

WRAPPER=./mcpbridge
HTTP_SERVER=./tests/fake_http_mcp

for bin in "$WRAPPER" "$HTTP_SERVER"; do
    if [ ! -x "$bin" ]; then
        echo "http session invalid e2e: $bin not built" >&2
        exit 1
    fi
done

WRAPPER_OUT=$(mktemp -t mcpbridge-http-stale.XXXXXX)
WRAPPER_ERR=$(mktemp -t mcpbridge-http-stale.XXXXXX)
SERVER_OUT=$(mktemp -t mcpbridge-http-stale.XXXXXX)
SERVER_ERR=$(mktemp -t mcpbridge-http-stale.XXXXXX)
INPUT_FIFO=$(mktemp -u -t mcpbridge-http-stale.XXXXXX)
CONFIG_FILE=$(mktemp -t mcpbridge-http-stale.XXXXXX.json)
SOCK="/tmp/mcpb-http-stale-$$.sock"
mkfifo "$INPUT_FIFO"

cleanup() {
    [ -n "${WRAPPER_PID:-}" ] && kill -TERM "$WRAPPER_PID" 2>/dev/null || true
    [ -n "${SERVER_PID:-}" ]  && kill -TERM "$SERVER_PID"  2>/dev/null || true
    wait 2>/dev/null || true
    if [ "${MCPBRIDGE_KEEP_E2E_LOGS:-}" = "1" ]; then
        echo "http session invalid e2e: logs preserved at $WRAPPER_OUT $WRAPPER_ERR $SERVER_OUT $SERVER_ERR $CONFIG_FILE" >&2
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
    echo "http session invalid e2e: fake server did not report a port" >&2
    cat "$SERVER_ERR" >&2
    exit 1
fi
URL="http://127.0.0.1:$PORT/mcp"
echo "http session invalid e2e: fake server on $URL" >&2

cat >"$CONFIG_FILE" <<EOF
{
  "schema": 2,
  "name": "fake-http-mcp",
  "url": "$URL",
  "source": {"type": "brew", "formula": "x/y/fake-http-mcp"}
}
EOF

# Run the wrapper with the daemon socket pointed at a non-existent
# path so it operates in standalone mode — this test deliberately
# exercises the no-daemon recovery path.
MCPBRIDGE_SOCKET="$SOCK" "$WRAPPER" -v connect "$CONFIG_FILE" \
    <"$INPUT_FIFO" >"$WRAPPER_OUT" 2>"$WRAPPER_ERR" &
WRAPPER_PID=$!

exec 3<>"$INPUT_FIFO"

INIT='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"http-stale-e2e","version":"1"}}}'
INITED='{"jsonrpc":"2.0","method":"notifications/initialized"}'
LIST1='{"jsonrpc":"2.0","id":2,"method":"tools/list"}'
LIST2='{"jsonrpc":"2.0","id":3,"method":"tools/list"}'
LIST3='{"jsonrpc":"2.0","id":4,"method":"tools/list"}'

printf '%s\n' "$INIT"   >&3
printf '%s\n' "$INITED" >&3
printf '%s\n' "$LIST1"  >&3

sleep 0.5

# Rotate the upstream's session id — anything wearing the old one is
# now rejected with 400. This is the exact failure mode 🎯T7 targets.
kill -USR1 "$SERVER_PID"

# Give the signal time to land before the next request races it.
sleep 0.1

printf '%s\n' "$LIST2" >&3

# Allow the recovery cycle to complete (DRAINING -> SWAPPING ->
# STARTING -> RUNNING + queue drain + response).
sleep 1.0

printf '%s\n' "$LIST3" >&3

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

# All three tools/list responses must come back to the agent.
for id in 2 3 4; do
    if ! grep -q "\"id\":${id}" "$WRAPPER_OUT"; then
        echo "http session invalid e2e: response for id=${id} not observed" >&2
        dump_and_exit
    fi
done

# The original initialize response is forwarded once; the replayed
# initialize response is consumed by dispatch. So `serverInfo` must
# appear exactly once in the wrapper's stdout.
init_count=$(grep -c '"serverInfo"' "$WRAPPER_OUT" || true)
if [ "$init_count" != "1" ]; then
    echo "http session invalid e2e: expected 1 initialize response, saw $init_count" >&2
    dump_and_exit
fi

# list_changed notifications signal the agent to refetch tools /
# prompts / resources after the silent re-handshake. All three must
# fire — same contract as the daemon-driven reload.
for kind in tools prompts resources; do
    if ! grep -q "\"notifications/${kind}/list_changed\"" "$WRAPPER_OUT"; then
        echo "http session invalid e2e: ${kind}/list_changed notification missing" >&2
        dump_and_exit
    fi
done

# Wrapper logs must mention the recovery — the operator-visible
# evidence that the abstraction held under upstream session loss.
if ! grep -q 'session is stale' "$WRAPPER_ERR" \
&& ! grep -q 'session stale' "$WRAPPER_ERR"; then
    echo "http session invalid e2e: wrapper did not log a session-stale event" >&2
    dump_and_exit
fi

echo "e2e_http_session_invalid_test: ok"
