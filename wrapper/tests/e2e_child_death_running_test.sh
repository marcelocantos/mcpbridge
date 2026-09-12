#!/bin/sh
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# ENT-003: unexpected stdio child death while RUNNING ends the wrapper.
#
# Chosen remediation: delete the dead crash-recovery / backoff-retry
# path rather than wiring a timer. Session survival across
# upstream restarts is the daemon-driven reload / HTTP self-reload
# path (see e2e_child_death_inflight_test.sh). A child that just
# dies outside a drain is loud and recoverable: the agent sees the
# server go away.
#
# Invoked from wrapper/ by `make test`.

set -eu

BIN=./mcpbridge
CHILD=./tests/fake_mcp

if [ ! -x "$BIN" ]; then
    echo "child-death-running e2e: $BIN not built" >&2
    exit 1
fi
if [ ! -x "$CHILD" ]; then
    echo "child-death-running e2e: $CHILD not built" >&2
    exit 1
fi

OUT=$(mktemp -t mcpbridge-death-run.XXXXXX)
ERR=$(mktemp -t mcpbridge-death-run.XXXXXX)
CFG=$(mktemp -t mcpbridge-death-run.XXXXXX.json)
INPUT_FIFO=$(mktemp -u -t mcpbridge-death-run.XXXXXX)
mkfifo "$INPUT_FIFO"

CHILD_ABS=$(cd "$(dirname "$CHILD")" && pwd)/$(basename "$CHILD")
cat >"$CFG" <<EOF
{
  "schema": 2,
  "name": "fake-mcp",
  "command": "$CHILD_ABS"
}
EOF

cleanup() {
    [ -n "${WRAPPER_PID:-}" ] && kill -TERM "$WRAPPER_PID" 2>/dev/null || true
    wait 2>/dev/null || true
    rm -f "$OUT" "$ERR" "$CFG" "$INPUT_FIFO"
}
trap cleanup EXIT

fail() {
    echo "child-death-running e2e: $1" >&2
    echo "--- wrapper stdout ---" >&2
    cat "$OUT" >&2
    echo "--- wrapper stderr ---" >&2
    cat "$ERR" >&2
    exit 1
}

"$BIN" -v connect "$CFG" <"$INPUT_FIFO" >"$OUT" 2>"$ERR" &
WRAPPER_PID=$!

exec 3<>"$INPUT_FIFO"

INIT='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"death-run-e2e","version":"1"}}}'
INITED='{"jsonrpc":"2.0","method":"notifications/initialized"}'

printf '%s\n' "$INIT"   >&3
printf '%s\n' "$INITED" >&3

i=0
while [ $i -lt 40 ]; do
    if grep -q '"id":1' "$OUT" 2>/dev/null; then
        break
    fi
    i=$((i + 1))
    sleep 0.05
done
grep -q '"id":1' "$OUT" || fail "initialize never completed; FSM is not RUNNING"

CHILD_PID=$(pgrep -P "$WRAPPER_PID" || true)
[ -n "$CHILD_PID" ] || fail "could not find the wrapper's child process"

kill -KILL $CHILD_PID

# The wrapper must exit. A live process after this window would mean
# someone re-wired RESPAWN without updating this contract.
i=0
while [ $i -lt 40 ]; do
    if ! kill -0 "$WRAPPER_PID" 2>/dev/null; then
        WRAPPER_PID=
        echo "e2e_child_death_running_test: ok"
        exit 0
    fi
    i=$((i + 1))
    sleep 0.05
done

fail "wrapper still running after stdio child death while RUNNING — crash recovery is not a product path"
