# mcpbridge wire protocol

Version: **1**

This document specifies the protocol spoken between the C wrapper
(`mcpbridge`) and the Go daemon (`mcpbridge-daemon`) over a Unix domain
socket. Both sides must agree on this spec; changes require bumping
the schema version and handling both forms during a transition window.

## Transport

Unix domain socket, `SOCK_STREAM`. Line-oriented: one JSON object per
line, terminated by a single `\n` (0x0A). Lines must be complete
UTF-8. Maximum line length is **256 KiB**; longer lines are protocol
errors and the peer MUST close the connection.

The protocol is intentionally **not** JSON-RPC 2.0. The full
request/id/result machinery is overkill for the four-ish message types
exchanged here, and the simpler envelope is easier to hand-parse in C.

## Socket path resolution

The daemon creates the socket at startup, and the wrapper dials it.
Both sides resolve the path identically using this priority:

1. `--socket PATH` (both binaries) — explicit override for testing.
2. `$MCPBRIDGE_SOCKET` environment variable, if set and non-empty.
3. Platform default:
   - **macOS**: `$HOME/Library/Caches/mcpbridge/daemon.sock`
   - **Linux**: `$XDG_RUNTIME_DIR/mcpbridge/daemon.sock` if
     `$XDG_RUNTIME_DIR` is set and writable.
4. Fallback (both platforms): `$TMPDIR/mcpbridge-$UID/daemon.sock`
   (or `/tmp/mcpbridge-$UID/daemon.sock` if `$TMPDIR` is unset).

The daemon `mkdir -p`s the parent directory with mode `0700` and
creates the socket with mode `0600`. Both are owned by the user
running the daemon; the wrapper verifies ownership and mode on connect
and refuses to use a socket it does not exclusively own.

## Connection lifecycle

```
wrapper                                      daemon
   │                                            │
   │────── connect() ─────────────────────────►│
   │                                            │
   │────── hello {...} ────────────────────────►│
   │                                            │
   │◄───── hello_ok {...} ──────────────────────│
   │                                            │
   │────── register {...} ─────────────────────►│
   │                                            │
   │◄───── register_ok {...} (or error) ────────│
   │                                            │
   │  (operational — daemon may push reload)    │
   │                                            │
   │◄───── reload {...} ────────────────────────│
   │                                            │
   │────── reload_ack {...} ───────────────────►│
   │                                            │
   │  ... eventually ...                        │
   │                                            │
   │────── deregister {...} ───────────────────►│
   │                                            │
   │────── close() ────────────────────────────►│
```

The wrapper drives the first two messages (hello, register). The
daemon drives reload. Either side may send shutdown before closing.

## Message envelope

Every message is a JSON object with at least these fields:

```json
{
  "t": "<type>",          // message type, see below
  "v": 1,                 // protocol schema version
  "seq": 42               // monotonic sequence number per connection
}
```

Additional fields depend on `t`. Unknown fields MUST be ignored by
the receiver (forward compatibility). Unknown values for `t` MUST be
logged and the message dropped; the connection stays open.

`seq` is assigned independently by each side starting at 1. It is used
to correlate requests with their responses (e.g. `register` with
`register_ok`) and for logging.

## Message types

### `hello` (wrapper → daemon)

First message after connect. Identifies the wrapper's protocol
capabilities. The daemon replies with `hello_ok` or closes the
connection if the version is unsupported.

```json
{
  "t": "hello",
  "v": 1,
  "seq": 1,
  "wrapper_version": "0.1.0",
  "pid": 12345
}
```

### `hello_ok` (daemon → wrapper)

Acknowledges the hello and advertises daemon capabilities.

```json
{
  "t": "hello_ok",
  "v": 1,
  "seq": 1,
  "daemon_version": "0.1.0",
  "ack_seq": 1
}
```

If the daemon does not support the wrapper's `v`, it MAY instead reply
with an `error` message and then close.

### `register` (wrapper → daemon)

The wrapper announces which MCP server it is wrapping. The daemon uses
this to watch the child binary for changes (fsnotify) and to track
which wrappers should receive `reload` notifications when the binary
changes on disk.

```json
{
  "t": "register",
  "v": 1,
  "seq": 2,
  "name": "mnemo",
  "child_pid": 12346,
  "child_binary": "/opt/homebrew/bin/mnemo"
}
```

`name` is the config name — derived by the wrapper from
`basename(argv[1])` (stdio mode) or from `--config NAME`. The daemon
looks up `~/.config/mcpbridge/<name>.json` (and the share fallback) to
find the upgrade metadata.

### `register_ok` (daemon → wrapper)

```json
{
  "t": "register_ok",
  "v": 1,
  "seq": 2,
  "ack_seq": 2,
  "config_found": true,
  "polling": true
}
```

`config_found` is `false` if no config file matched `name`. The
wrapper remains registered (so the daemon knows it exists) and the
wrapper logs a one-line warning.

`polling` is a vestigial field kept in the envelope for backwards
compatibility; it is always `false`. Reloads still arrive when the
watcher detects a binary change on disk, regardless of this field.

### `deregister` (wrapper → daemon)

Sent by the wrapper at clean shutdown. The daemon removes the wrapper
from its active set and MAY stop polling the corresponding server if
no other wrappers are registered for the same `name`.

```json
{
  "t": "deregister",
  "v": 1,
  "seq": 99,
  "reason": "upstream_closed"
}
```

An ungraceful wrapper exit (crash, kill -9) is detected by the daemon
via socket closure. `deregister` is best-effort cleanup, not required
for correctness.

### `reload` (daemon → wrapper)

The daemon has installed a new version of the wrapped server. The
wrapper should drain in-flight requests, stop the child, start a new
child, replay `initialize`, diff the tool list, and resume.

```json
{
  "t": "reload",
  "v": 1,
  "seq": 7,
  "name": "mnemo",
  "old_version": "0.4.2",
  "new_version": "0.5.0",
  "reason": "binary_changed"
}
```

`reason` is advisory; the wrapper logs it but does not branch on it.
Known values: `binary_changed` (fsnotify-detected) and `manual`
(SIGHUP to daemon). Older daemons may emit `brew_upgrade` or
`github_release`; wrappers ignore the value.

### `reload_ack` (wrapper → daemon)

Sent by the wrapper when the reload is complete (new child is running
and has answered `initialize`) or has failed.

```json
{
  "t": "reload_ack",
  "v": 1,
  "seq": 3,
  "ack_seq": 7,
  "status": "ok"
}
```

`status` is one of `ok`, `drain_timeout`, `spawn_failed`,
`init_failed`. On anything other than `ok`, the wrapper includes a
`detail` string.

### `shutdown` (daemon → wrapper)

The daemon is going down cleanly. The wrapper MAY stay up and
continue bridging; it just will not receive further `reload`
notifications until the daemon comes back and the wrapper reconnects.

```json
{
  "t": "shutdown",
  "v": 1,
  "seq": 10,
  "reason": "service_stop"
}
```

### `error` (either direction)

Used for fatal protocol errors. The sender MUST close the connection
after sending.

```json
{
  "t": "error",
  "v": 1,
  "seq": 5,
  "code": "unsupported_version",
  "detail": "wrapper v=2, daemon supports v=1"
}
```

Known codes: `unsupported_version`, `bad_envelope`, `line_too_long`,
`unknown_name`, `internal`.

## Reconnect and backoff

If the wrapper cannot connect at startup (socket missing, connection
refused, permission denied), it logs **one** warning line and enters a
retry loop:

- First retry at `+1s`
- Subsequent retries double (2s, 4s) and are capped at **5s**
- No retry ceiling — the wrapper retries for its entire lifetime

Successful connection resets the backoff to 1s. The wrapper
re-sends `hello` + `register` on each successful dial; the daemon
treats a new connection from a known `name` as a fresh registration
(previous entry is replaced).

If the daemon dies mid-session, the wrapper's socket `read` returns
EOF. The wrapper logs, closes its end, and re-enters the retry loop.
Any `reload` notification that was in flight and not yet acked is
lost — the next fsnotify event on the daemon side will re-detect
the binary change and send a new `reload` after reconnect.

## Versioning

The `v` field is the protocol schema version. Bump when:

- adding a required field to an existing message type
- removing a field
- changing semantics of an existing field
- adding a new message type that the opposite side MUST understand

Do **not** bump for:

- adding an optional field that older parsers safely ignore
- adding a new `reason` / `code` string value

Both sides MUST refuse to talk to a peer with an unknown `v`. Forward
compatibility is not promised; upgrades are coordinated between
wrapper and daemon and are expected to happen together (both are
installed from the same release).
