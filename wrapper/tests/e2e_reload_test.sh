#!/bin/sh
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# End-to-end test of the full reload cycle:
#
#   agent --(stdio)--> mcpbridge --(stdio)--> fake_mcp
#                       |
#                       +--(uds)--> mcpbridge-daemon
#
# The test drives:
#   1. A normal initialize + initialized + tools/list
#   2. SIGHUP to the daemon, which broadcasts a reload notification
#      that the wrapper handles by stopping fake_mcp, spawning a new
#      fake_mcp, replaying the cached initialize + initialized, and
#      sending reload_ack to the daemon
#   3. A second tools/list that must succeed on the NEW child
#
# Invoked from wrapper/ by `make test`. Both wrapper and daemon
# binaries must already be built — the top-level Makefile ensures
# this via the wrapper-test -> daemon dependency.

set -eu

WRAPPER=./mcpbridge
DAEMON=../daemon/mcpbridge-daemon
CHILD=./tests/fake_mcp

if [ ! -x "$WRAPPER" ]; then
    echo "reload e2e: $WRAPPER not built" >&2
    exit 1
fi
if [ ! -x "$DAEMON" ]; then
    echo "reload e2e: $DAEMON not built" >&2
    exit 1
fi
if [ ! -x "$CHILD" ]; then
    echo "reload e2e: $CHILD not built" >&2
    exit 1
fi

SOCK="/tmp/mcpb-reload-$$.sock"
WRAPPER_OUT=$(mktemp -t mcpbridge-reload.XXXXXX)
WRAPPER_ERR=$(mktemp -t mcpbridge-reload.XXXXXX)
DAEMON_ERR=$(mktemp -t mcpbridge-reload.XXXXXX)
INPUT_FIFO=$(mktemp -u -t mcpbridge-reload.XXXXXX)
CONFIG_FILE=$(mktemp -t mcpbridge-reload.XXXXXX.json)
mkfifo "$INPUT_FIFO"

# Build a schema:2 stdio config pointing at fake_mcp.
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
        echo "reload e2e: logs preserved at $WRAPPER_OUT $WRAPPER_ERR $DAEMON_ERR" >&2
        return
    fi
    rm -f "$WRAPPER_OUT" "$WRAPPER_ERR" "$DAEMON_ERR" "$INPUT_FIFO" "$SOCK" "$CONFIG_FILE"
}
trap cleanup EXIT

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
    echo "reload e2e: daemon did not create socket" >&2
    cat "$DAEMON_ERR" >&2
    exit 1
fi

# Start the wrapper reading from the FIFO and writing to WRAPPER_OUT.
# The FIFO lets us hold stdin open while driving the session from
# several shell commands with a sleep between them.
MCPBRIDGE_SOCKET="$SOCK" "$WRAPPER" -v connect "$CONFIG_FILE" \
    <"$INPUT_FIFO" >"$WRAPPER_OUT" 2>"$WRAPPER_ERR" &
WRAPPER_PID=$!

# Open the FIFO for both reading and writing on fd 3 so there is
# always at least one writer (us) holding the FIFO open, even
# transiently while the wrapper child is still exec'ing. Without
# this, the wrapper's blocking open(RDONLY) can race with the
# shell's exec 3>FIFO, causing the first printf to see EPIPE when
# the wrapper briefly has no open read-fd yet.
exec 3<>"$INPUT_FIFO"

INIT='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"reload-e2e","version":"1"}}}'
INITED='{"jsonrpc":"2.0","method":"notifications/initialized"}'
LIST1='{"jsonrpc":"2.0","id":2,"method":"tools/list"}'
LIST2='{"jsonrpc":"2.0","id":3,"method":"tools/list"}'

printf '%s\n' "$INIT"   >&3
printf '%s\n' "$INITED" >&3
printf '%s\n' "$LIST1"  >&3

# Give the wrapper time to register with the daemon and forward the
# first three messages.
sleep 0.5

# Trigger the reload broadcast.
kill -HUP "$DAEMON_PID"

# Give the reload cycle time to run: drain -> stop child -> start new
# child -> replay initialize -> INITIALIZE_OK -> reload_ack.
sleep 0.8

# Second tools/list should hit the new child.
printf '%s\n' "$LIST2" >&3

sleep 0.5

# Close the FIFO so the wrapper sees EOF and exits cleanly.
exec 3>&-

# Wait for wrapper to finish.
wait "$WRAPPER_PID" 2>/dev/null || true

# Check the wrapper's stdout for both tools/list responses.
if ! grep -q '"id":2' "$WRAPPER_OUT"; then
    echo "reload e2e: first tools/list response not observed" >&2
    echo "--- wrapper stdout ---" >&2
    cat "$WRAPPER_OUT" >&2
    echo "--- wrapper stderr ---" >&2
    cat "$WRAPPER_ERR" >&2
    echo "--- daemon stderr ---" >&2
    cat "$DAEMON_ERR" >&2
    exit 1
fi
if ! grep -q '"id":3' "$WRAPPER_OUT"; then
    echo "reload e2e: second tools/list response not observed" >&2
    echo "--- wrapper stdout ---" >&2
    cat "$WRAPPER_OUT" >&2
    echo "--- wrapper stderr ---" >&2
    cat "$WRAPPER_ERR" >&2
    echo "--- daemon stderr ---" >&2
    cat "$DAEMON_ERR" >&2
    exit 1
fi

# There must be exactly ONE initialize response — the wrapper must
# consume the replayed initialize's response rather than forwarding
# it upstream.
init_count=$(grep -c '"serverInfo"' "$WRAPPER_OUT" || true)
if [ "$init_count" != "1" ]; then
    echo "reload e2e: expected 1 initialize response, saw $init_count" >&2
    cat "$WRAPPER_OUT" >&2
    exit 1
fi

# The wrapper must emit all three list_changed notifications after
# the reload so the agent refetches tools / prompts / resources
# from the (potentially changed) new child. See STABILITY.md
# "API surface continuity across reloads".
for kind in tools prompts resources; do
    if ! grep -q "\"notifications/${kind}/list_changed\"" "$WRAPPER_OUT"; then
        echo "reload e2e: ${kind}/list_changed notification missing" >&2
        cat "$WRAPPER_OUT" >&2
        exit 1
    fi
done

# The daemon log should show the reload broadcast and the reload_ack.
if ! grep -q 'SIGHUP' "$DAEMON_ERR"; then
    echo "reload e2e: daemon didn't log SIGHUP broadcast" >&2
    cat "$DAEMON_ERR" >&2
    exit 1
fi
if ! grep -q 'reload_ack' "$DAEMON_ERR"; then
    echo "reload e2e: daemon didn't log reload_ack" >&2
    cat "$DAEMON_ERR" >&2
    exit 1
fi

echo "e2e_reload_test: ok"
