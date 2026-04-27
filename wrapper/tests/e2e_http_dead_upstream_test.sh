#!/bin/sh
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# End-to-end tests for 🎯T7.1: mcpbridge survives outage windows
# without exiting the agent's stdio session.
#
# Three scenarios:
#
#   Idle-survives — the wrapper is initialised but no tool call is
#   in flight. Kill the upstream, wait several seconds, restart it,
#   issue a tool call. The call must succeed (T7's reinit kicks in
#   on the post-restart session-id mismatch). The wrapper must NOT
#   have made any connection attempts during the idle window.
#
#   In-flight-recovers — kill the upstream, issue a tool call, then
#   restart the upstream within the timeout window. The wrapper's
#   tcp_connect-with-backoff retries through the gap, the call lands
#   under a fresh session, and the agent sees a normal response.
#
#   In-flight-timeout — kill the upstream, issue a tool call with a
#   short configured timeout, never restart. The wrapper synthesises
#   a structured JSON-RPC error to the agent, the stdio session
#   stays alive, and a subsequent tool call gets its own structured
#   error (proving the wrapper didn't exit).

set -eu

WRAPPER=./mcpbridge
HTTP_SERVER=./tests/fake_http_mcp

for bin in "$WRAPPER" "$HTTP_SERVER"; do
    if [ ! -x "$bin" ]; then
        echo "http dead upstream e2e: $bin not built" >&2
        exit 1
    fi
done

# Each subtest uses fresh tempfiles. Track them in arrays so cleanup
# can wipe everything regardless of which subtest tripped.
TMPFILES=""
PIDS=""

cleanup() {
    for pid in $PIDS; do
        kill -TERM "$pid" 2>/dev/null || true
    done
    wait 2>/dev/null || true
    if [ "${MCPBRIDGE_KEEP_E2E_LOGS:-}" = "1" ]; then
        echo "http dead upstream e2e: logs preserved at $TMPFILES" >&2
        return
    fi
    for f in $TMPFILES; do
        rm -f "$f"
    done
}
trap cleanup EXIT

mktmp() {
    f=$(mktemp -t "mcpbridge-dead-upstream.XXXXXX$1")
    TMPFILES="$TMPFILES $f"
    echo "$f"
}

mkfifo_tmp() {
    f=$(mktemp -u -t "mcpbridge-dead-upstream.XXXXXX")
    mkfifo "$f"
    TMPFILES="$TMPFILES $f"
    echo "$f"
}

# start_server: spawn the fake_http_mcp on the given port (or 0 for
# ephemeral). Sets SERVER_PORT and SERVER_PID in the caller's scope
# (no subshell — POSIX functions share scope, but $() does not).
# Restart-on-same-port works because fake_http_mcp sets SO_REUSEADDR.
#
# We explicitly close fds 4/5/6 in the child. Each subtest opens
# the test's input FIFO on one of these fds via `exec N<>FIFO` so it
# can write requests to the wrapper's stdin. If start_server were
# called WHILE that fd was open in the parent shell (it is for the
# server-restart cases in subtests A and B), the spawned fake_http
# would inherit the fd as a FIFO writer, keeping the wrapper's stdin
# alive past the test's `exec N>&-`. The wrapper would never see
# EOF and `wait WRAPPER_PID` would hang forever. Closing 4/5/6
# defensively here works for every subtest regardless of which fd
# it picked.
start_server() {
    OUT=$1; ERR=$2; REQ_PORT=${3:-0}
    : >"$OUT"  # truncate so head reads the freshly-bound port line
    "$HTTP_SERVER" --port "$REQ_PORT" -v >"$OUT" 2>"$ERR" \
        4<&- 4>&- 5<&- 5>&- 6<&- 6>&- &
    SERVER_PID=$!
    PIDS="$PIDS $SERVER_PID"
    SERVER_PORT=""
    i=0
    while [ $i -lt 100 ]; do
        if [ -s "$OUT" ]; then
            SERVER_PORT=$(head -n 1 "$OUT")
            [ -n "$SERVER_PORT" ] && break
        fi
        i=$((i+1))
        sleep 0.05
    done
    if [ -z "$SERVER_PORT" ]; then
        echo "http dead upstream e2e: fake server did not report a port" >&2
        cat "$ERR" >&2
        exit 1
    fi
}

write_config() {
    F=$1; URL=$2; TIMEOUT=$3
    cat >"$F" <<EOF
{
  "schema": 2,
  "name": "fake-http-mcp",
  "url": "$URL",
  "tool_call_timeout_ms": $TIMEOUT,
  "source": {"type": "brew", "formula": "x/y/fake-http-mcp"}
}
EOF
}

# ========== Subtest A: idle-survives ==========
{
echo "--- subtest A: idle survives a multi-second outage ---" >&2
SERVER_OUT_A=$(mktmp .a-srv-out)
SERVER_ERR_A=$(mktmp .a-srv-err)
WRAPPER_OUT_A=$(mktmp .a-wr-out)
WRAPPER_ERR_A=$(mktmp .a-wr-err)
CONFIG_A=$(mktmp .a-cfg.json)
FIFO_A=$(mkfifo_tmp)
SOCK_A="/tmp/mcpb-dead-A-$$.sock"
TMPFILES="$TMPFILES $SOCK_A"

start_server "$SERVER_OUT_A" "$SERVER_ERR_A"
PORT_A=$SERVER_PORT
SERVER_PID_A=$SERVER_PID
URL_A="http://127.0.0.1:$PORT_A/mcp"
write_config "$CONFIG_A" "$URL_A" 30000

MCPBRIDGE_SOCKET="$SOCK_A" "$WRAPPER" -v connect "$CONFIG_A" \
    <"$FIFO_A" >"$WRAPPER_OUT_A" 2>"$WRAPPER_ERR_A" &
WRAPPER_PID_A=$!
PIDS="$PIDS $WRAPPER_PID_A"
exec 4<>"$FIFO_A"

INIT='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"dead-upstream-e2e","version":"1"}}}'
INITED='{"jsonrpc":"2.0","method":"notifications/initialized"}'
LIST_PRE='{"jsonrpc":"2.0","id":2,"method":"tools/list"}'
LIST_POST='{"jsonrpc":"2.0","id":3,"method":"tools/list"}'

printf '%s\n' "$INIT"     >&4
printf '%s\n' "$INITED"   >&4
printf '%s\n' "$LIST_PRE" >&4

sleep 0.5

# Snapshot the wrapper's stderr so we can check that nothing happened
# during the idle window.
LINES_BEFORE_IDLE=$(wc -l <"$WRAPPER_ERR_A" | tr -d ' ')

# Kill the upstream, idle for several seconds.
kill -TERM "$SERVER_PID_A"
wait "$SERVER_PID_A" 2>/dev/null || true

sleep 3

# During the idle window, no upstream I/O should have happened. The
# wrapper is allowed to log its periodic poll-loop housekeeping but
# must NOT log connection attempts to the dead server.
if grep -q 'http: connect 127.0.0.1' "$WRAPPER_ERR_A"; then
    SINCE=$(tail -n +"$((LINES_BEFORE_IDLE+1))" "$WRAPPER_ERR_A")
    if echo "$SINCE" | grep -q 'http: connect 127.0.0.1'; then
        echo "http dead upstream e2e (A): wrapper polled the dead upstream while idle" >&2
        echo "--- wrapper stderr (since idle) ---" >&2
        echo "$SINCE" >&2
        exit 1
    fi
fi

# Restart the upstream on the SAME port — fake_http_mcp uses
# SO_REUSEADDR so this works even if the previous instance is still
# in TIME_WAIT. If the rebind fails, fall back to skipping the
# post-recovery call assertion (idle-survives is already proven
# above by the absence of polling).
start_server "$SERVER_OUT_A" "$SERVER_ERR_A" "$PORT_A"
PORT_A2=$SERVER_PORT
SERVER_PID_A=$SERVER_PID
if [ "$PORT_A2" != "$PORT_A" ]; then
    # The kernel may hand us a different ephemeral port. That's fine
    # for T7.1's idle-survives semantics — the wrapper isn't configured
    # against a dynamic port, it's configured against the same URL.
    # Skip this subtest's call assertion if we can't bind back to the
    # original port: the test still proved the wrapper survived idle.
    echo "http dead upstream e2e (A): server restarted on different port ($PORT_A2 vs $PORT_A); skipping post-recovery assertion" >&2
    exec 4>&-
    kill -TERM "$WRAPPER_PID_A" 2>/dev/null || true
    wait "$WRAPPER_PID_A" 2>/dev/null || true
    kill -TERM "$SERVER_PID_A" 2>/dev/null || true
else
    sleep 0.3
    printf '%s\n' "$LIST_POST" >&4
    sleep 1.5
    exec 4>&-
    wait "$WRAPPER_PID_A" 2>/dev/null || true

    if ! grep -q '"id":3' "$WRAPPER_OUT_A"; then
        echo "http dead upstream e2e (A): post-recovery tools/list response missing" >&2
        echo "--- wrapper stdout ---" >&2; cat "$WRAPPER_OUT_A" >&2
        echo "--- wrapper stderr ---" >&2; cat "$WRAPPER_ERR_A" >&2
        exit 1
    fi
fi

echo "subtest A: ok" >&2
}

# ========== Subtest B: in-flight recovers within timeout ==========
{
echo "--- subtest B: in-flight call survives outage within timeout ---" >&2
SERVER_OUT_B=$(mktmp .b-srv-out)
SERVER_ERR_B=$(mktmp .b-srv-err)
WRAPPER_OUT_B=$(mktmp .b-wr-out)
WRAPPER_ERR_B=$(mktmp .b-wr-err)
CONFIG_B=$(mktmp .b-cfg.json)
FIFO_B=$(mkfifo_tmp)
SOCK_B="/tmp/mcpb-dead-B-$$.sock"
TMPFILES="$TMPFILES $SOCK_B"

start_server "$SERVER_OUT_B" "$SERVER_ERR_B"
PORT_B=$SERVER_PORT
SERVER_PID_B=$SERVER_PID
URL_B="http://127.0.0.1:$PORT_B/mcp"
# Generous timeout so the upstream has time to come back during the
# in-flight retry window.
write_config "$CONFIG_B" "$URL_B" 10000

MCPBRIDGE_SOCKET="$SOCK_B" "$WRAPPER" -v connect "$CONFIG_B" \
    <"$FIFO_B" >"$WRAPPER_OUT_B" 2>"$WRAPPER_ERR_B" &
WRAPPER_PID_B=$!
PIDS="$PIDS $WRAPPER_PID_B"
exec 5<>"$FIFO_B"

printf '%s\n' "$INIT"   >&5
printf '%s\n' "$INITED" >&5
printf '%s\n' "$LIST_PRE" >&5
sleep 0.5

# Kill the upstream and IMMEDIATELY issue a call. The wrapper detects
# 400 (or connect-refused once the socket dies), enters retry, and
# the call hangs in flight while the upstream is missing.
kill -TERM "$SERVER_PID_B"
wait "$SERVER_PID_B" 2>/dev/null || true

printf '%s\n' "$LIST_POST" >&5

# Restart the upstream after a short outage — well within the
# 10-second timeout. The wrapper's retry should reach it; T7's reinit
# captures the new session id; the call lands.
sleep 1
start_server "$SERVER_OUT_B" "$SERVER_ERR_B" "$PORT_B"
PORT_B2=$SERVER_PORT
SERVER_PID_B=$SERVER_PID

# If the kernel handed us a different port, this subtest can't
# proceed — bail with a soft skip rather than a hard fail.
if [ "$PORT_B2" != "$PORT_B" ]; then
    echo "http dead upstream e2e (B): server restarted on different port; skipping" >&2
    exec 5>&-
    kill -TERM "$WRAPPER_PID_B" 2>/dev/null || true
    wait "$WRAPPER_PID_B" 2>/dev/null || true
    kill -TERM "$SERVER_PID_B" 2>/dev/null || true
else
    sleep 2
    exec 5>&-
    wait "$WRAPPER_PID_B" 2>/dev/null || true

    if ! grep -q '"id":3' "$WRAPPER_OUT_B"; then
        echo "http dead upstream e2e (B): in-flight tools/list did not recover" >&2
        echo "--- wrapper stdout ---" >&2; cat "$WRAPPER_OUT_B" >&2
        echo "--- wrapper stderr ---" >&2; cat "$WRAPPER_ERR_B" >&2
        exit 1
    fi
    if ! grep -q 'retrying in' "$WRAPPER_ERR_B"; then
        echo "http dead upstream e2e (B): no connect retry log line observed" >&2
        echo "--- wrapper stderr ---" >&2; cat "$WRAPPER_ERR_B" >&2
        exit 1
    fi
fi

echo "subtest B: ok" >&2
}

# ========== Subtest C: timeout returns structured error, wrapper survives ==========
{
echo "--- subtest C: in-flight timeout produces JSON-RPC error, wrapper alive ---" >&2
SERVER_OUT_C=$(mktmp .c-srv-out)
SERVER_ERR_C=$(mktmp .c-srv-err)
WRAPPER_OUT_C=$(mktmp .c-wr-out)
WRAPPER_ERR_C=$(mktmp .c-wr-err)
CONFIG_C=$(mktmp .c-cfg.json)
FIFO_C=$(mkfifo_tmp)
SOCK_C="/tmp/mcpb-dead-C-$$.sock"
TMPFILES="$TMPFILES $SOCK_C"

start_server "$SERVER_OUT_C" "$SERVER_ERR_C"
PORT_C=$SERVER_PORT
SERVER_PID_C=$SERVER_PID
URL_C="http://127.0.0.1:$PORT_C/mcp"
# Short timeout — the test wants the timeout to fire, not a hang.
write_config "$CONFIG_C" "$URL_C" 1500

MCPBRIDGE_SOCKET="$SOCK_C" "$WRAPPER" -v connect "$CONFIG_C" \
    <"$FIFO_C" >"$WRAPPER_OUT_C" 2>"$WRAPPER_ERR_C" &
WRAPPER_PID_C=$!
PIDS="$PIDS $WRAPPER_PID_C"
exec 6<>"$FIFO_C"

printf '%s\n' "$INIT"   >&6
printf '%s\n' "$INITED" >&6
printf '%s\n' "$LIST_PRE" >&6
sleep 0.5

# Kill the upstream for good — never restart.
kill -TERM "$SERVER_PID_C"
wait "$SERVER_PID_C" 2>/dev/null || true

# First post-outage call should time out and produce a JSON-RPC error.
printf '%s\n' "$LIST_POST" >&6
sleep 3

# Second post-outage call should ALSO produce a JSON-RPC error (proves
# the wrapper didn't exit after the first timeout).
LIST_POST_2='{"jsonrpc":"2.0","id":4,"method":"tools/list"}'
printf '%s\n' "$LIST_POST_2" >&6
sleep 3

exec 6>&-
wait "$WRAPPER_PID_C" 2>/dev/null || true

# Both ids must come back as errors.
for id in 3 4; do
    line=$(grep "\"id\":${id}" "$WRAPPER_OUT_C" || true)
    if [ -z "$line" ]; then
        echo "http dead upstream e2e (C): no response for id=${id}" >&2
        echo "--- wrapper stdout ---" >&2; cat "$WRAPPER_OUT_C" >&2
        echo "--- wrapper stderr ---" >&2; cat "$WRAPPER_ERR_C" >&2
        exit 1
    fi
    case "$line" in
        *'"error"'*)
            ;;
        *)
            echo "http dead upstream e2e (C): id=${id} response not an error envelope: $line" >&2
            exit 1
            ;;
    esac
    case "$line" in
        *'unreachable'*)
            ;;
        *)
            echo "http dead upstream e2e (C): id=${id} error message missing 'unreachable': $line" >&2
            exit 1
            ;;
    esac
done

# The wrapper must have logged the timeout signal explicitly.
if ! grep -q 'timeout' "$WRAPPER_ERR_C" \
&& ! grep -q 'unreachable past' "$WRAPPER_ERR_C"; then
    echo "http dead upstream e2e (C): wrapper did not log the timeout" >&2
    echo "--- wrapper stderr ---" >&2; cat "$WRAPPER_ERR_C" >&2
    exit 1
fi

echo "subtest C: ok" >&2
}

echo "e2e_http_dead_upstream_test: ok"
