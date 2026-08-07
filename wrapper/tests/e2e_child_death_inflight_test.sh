#!/bin/sh
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# 🎯T21: the backend dies while a request is in flight.
#
# This is the shape that actually bit us. e2e_reload_inflight_test.sh
# covers an orderly reload, where the drain waits and the child gets
# to answer. Here the child never answers — it is killed mid-call —
# so the FSM leaves DRAINING/RUNNING via CHILD_EXIT and the request
# is abandoned. Two things must still hold:
#
#   1. The agent gets a response for that id. Something must answer;
#      the process that owed the answer is gone.
#   2. The session survives. The in-flight bookkeeping must return to
#      zero, or every LATER reload parks the FSM in DRAINING forever
#      waiting for a count that can no longer reach zero, and every
#      subsequent request queues behind it — a permanently dead
#      session, which is the failure this wrapper exists to prevent.
#
# Assertion 2 is the one that regressed in the field: a single
# abandoned request poisoned the session for as long as the agent ran.
#
# Invoked from wrapper/ by `make test`.

set -eu

WRAPPER=./mcpbridge
DAEMON=../daemon/mcpbridge-daemon
CHILD=./tests/fake_mcp

for bin in "$WRAPPER" "$DAEMON" "$CHILD"; do
    if [ ! -x "$bin" ]; then
        echo "child-death e2e: $bin not built" >&2
        exit 1
    fi
done

SOCK="/tmp/mcpb-death-$$.sock"
WRAPPER_OUT=$(mktemp -t mcpbridge-death.XXXXXX)
WRAPPER_ERR=$(mktemp -t mcpbridge-death.XXXXXX)
DAEMON_ERR=$(mktemp -t mcpbridge-death.XXXXXX)
INPUT_FIFO=$(mktemp -u -t mcpbridge-death.XXXXXX)
CONFIG_FILE=$(mktemp -t mcpbridge-death.XXXXXX.json)
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
        echo "child-death e2e: logs preserved at $WRAPPER_OUT $WRAPPER_ERR $DAEMON_ERR" >&2
        return
    fi
    rm -f "$WRAPPER_OUT" "$WRAPPER_ERR" "$DAEMON_ERR" "$INPUT_FIFO" "$SOCK" "$CONFIG_FILE"
}
trap cleanup EXIT

fail() {
    echo "child-death e2e: $1" >&2
    echo "--- wrapper stdout ---" >&2
    cat "$WRAPPER_OUT" >&2
    echo "--- wrapper stderr ---" >&2
    cat "$WRAPPER_ERR" >&2
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

FAKE_MCP_CALL_DELAY_MS=3000 MCPBRIDGE_SOCKET="$SOCK" \
    "$WRAPPER" -v connect "$CONFIG_FILE" \
    <"$INPUT_FIFO" >"$WRAPPER_OUT" 2>"$WRAPPER_ERR" &
WRAPPER_PID=$!

exec 3<>"$INPUT_FIFO"

INIT='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"death-e2e","version":"1"}}}'
INITED='{"jsonrpc":"2.0","method":"notifications/initialized"}'
SLOW='{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"echo","arguments":{}}}'
AFTER='{"jsonrpc":"2.0","id":3,"method":"tools/list"}'
LATER='{"jsonrpc":"2.0","id":4,"method":"tools/list"}'

printf '%s\n' "$INIT"   >&3
printf '%s\n' "$INITED" >&3
sleep 0.5

# Slow call, then a reload so the FSM enters DRAINING waiting on it,
# then kill the child mid-drain. That is the field sequence: the
# drain ends via CHILD_EXIT instead of IN_FLIGHT_ZERO, so the wrapper
# swaps to a fresh backend while a request is still outstanding
# against the dead one. (An unexpected death outside a drain exits
# the wrapper by design — the agent sees the server go away, which
# is loud and recoverable. This path is the silent one.)
printf '%s\n' "$SLOW" >&3
sleep 0.4
kill -HUP "$DAEMON_PID"
sleep 0.4

CHILD_PID=$(pgrep -P "$WRAPPER_PID" || true)
[ -n "$CHILD_PID" ] || fail "could not find the wrapper's child process"
kill -KILL $CHILD_PID

# Respawn + replay initialize needs a moment.
sleep 3

if ! grep -q '"id":2' "$WRAPPER_OUT"; then
    fail "abandoned request (id 2) never got a response — the agent waits forever"
fi

# The session must still work.
printf '%s\n' "$AFTER" >&3
sleep 2
if ! grep -q '"id":3' "$WRAPPER_OUT"; then
    fail "no response for id 3 — session did not recover from the child's death"
fi

# And it must survive a reload afterwards: this is where a leaked
# in-flight count shows up, by parking the FSM in DRAINING forever.
kill -HUP "$DAEMON_PID"
sleep 2
printf '%s\n' "$LATER" >&3
sleep 2
if ! grep -q '"id":4' "$WRAPPER_OUT"; then
    fail "no response for id 4 — a later reload wedged the session, so the in-flight count never returned to zero"
fi

exec 3>&-
wait "$WRAPPER_PID" 2>/dev/null || true

echo "e2e_child_death_inflight_test: ok"
