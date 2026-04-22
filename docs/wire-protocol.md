# mcpbridge wire protocol

Versions: **1** (shipped in 0.1.x–0.3.x), **2** (🎯T4, v1.0).

This document specifies the protocol spoken between the C wrapper
(`mcpbridge`) and the Go daemon (`mcpbridge-daemon`) over a Unix domain
socket. Two versions coexist:

- **Version 1** is the MCP-aware wrapper / thin-daemon protocol used
  through 0.3.x. JSON envelopes, newline-framed. The wrapper understands
  MCP, HTTP, SSE; the daemon only orchestrates reloads. **Frozen** — no
  new v1 messages will be added. Daemons continue to speak v1 to any
  still-running 0.x wrappers they encounter.
- **Version 2** is the dumb-T-piece / protocol-aware-daemon architecture
  from 🎯T4. Length-prefixed binary envelopes. The wrapper is a
  minimal OS-primitive shell; the daemon drives it. This is the
  protocol we expect to run on long-term.

Each connection chooses its version in the opening `hello` (see
[Version negotiation](#version-negotiation)). The daemon implements
both; the wrapper implements only the version it was compiled
against. A v1 wrapper talks v1; a v2 wrapper talks v2.

## Socket path resolution (shared by v1 and v2)

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

## Version negotiation

Both protocol versions share the same socket and the same initial
handshake shape: the wrapper's first bytes on a fresh connection are
a `hello`. The daemon chooses a version based on what it sees.

- **v1 hello** is a newline-terminated JSON object whose first byte
  is `{` (0x7B).
- **v2 hello** is a length-prefixed binary envelope whose first four
  bytes are the payload length in big-endian, followed by the v2
  opcode byte `0x01` (HELLO).

The daemon peeks the first byte: `{` → v1 handler, anything else →
v2 handler. The handler then reads the rest of the hello in its
version's framing.

This lets the daemon ship both codecs simultaneously through a
multi-release transition: 0.x wrappers continue to work against 1.0+
daemons, and 1.0+ wrappers get the new protocol on day one. **Both
versions MUST be supported by the daemon indefinitely**; the v1
codec is frozen but never removed.

The wrapper is simpler: a given wrapper binary implements exactly
one version. Which version is a compile-time property of the
wrapper — there is no runtime fallback from v2 to v1.

---

# Protocol version 1 (legacy)

## v1 Transport

Unix domain socket, `SOCK_STREAM`. Line-oriented: one JSON object per
line, terminated by a single `\n` (0x0A). Lines must be complete
UTF-8. Maximum line length is **256 KiB**; longer lines are protocol
errors and the peer MUST close the connection.

The protocol is intentionally **not** JSON-RPC 2.0. The full
request/id/result machinery is overkill for the four-ish message types
exchanged here, and the simpler envelope is easier to hand-parse in C.

## v1 Connection lifecycle

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

## v1 Message envelope

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

## v1 Message types

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
this to enable upgrade polling for that server (if a config exists)
and to track which wrappers should receive `reload` notifications.

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
wrapper remains registered (so the daemon knows it exists) but
`polling` will be `false` and no `reload` will ever arrive. The
wrapper logs a one-line warning.

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
  "reason": "brew_upgrade"
}
```

`reason` is advisory; the wrapper logs it but does not branch on it.
Known values: `brew_upgrade`, `github_release`, `binary_changed`
(fsnotify-detected), `manual` (SIGHUP to daemon).

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

## v1 Reconnect and backoff

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
lost — the next poll cycle on the daemon side will re-detect the
condition and send a new `reload` after reconnect.

## v1 Versioning

The `v` field is the protocol schema version. Bump when:

- adding a required field to an existing message type
- removing a field
- changing semantics of an existing field
- adding a new message type that the opposite side MUST understand

Do **not** bump for:

- adding an optional field that older parsers safely ignore
- adding a new `reason` / `code` string value

v1 is **frozen**: no further changes. New functionality lands in v2.
Both sides MUST refuse to talk to a peer with an unknown v1 `v`
value — older v1 clients encountering v1=2 (hypothetical) close
immediately. In practice this never happens because v2 uses a
different envelope format entirely.

---

# Protocol version 2

## v2 Rationale

v1 had the wrapper speaking MCP, HTTP, SSE, and JSON — protocols
that evolve at their own pace. That contradicted the C wrapper's
founding charter ("compile unchanged for decades"). v2 flips the
split: the wrapper becomes a minimal OS-primitive T-piece, and the
daemon absorbs all protocol interpretation.

The wrapper's job under v2 is:

- Execute primitives requested by the daemon (`run`, `connect`,
  `read`, `write`, `stream`, `close`).
- Hold per-handle state (pids, cmds, tags) and a daemon-supplied
  opaque state blob across daemon restart.
- Emit asynchronous events (child exit, stream end, EOF).
- Fall back to a minimal stdio-child bridge when the daemon is
  unreachable and a `-- COMMAND` argv was supplied.

The wrapper does NOT parse MCP, HTTP, SSE, or JSON. The v2 envelope
is a tiny binary format that a C implementation can fully parse in
under 100 lines.

## v2 Envelope format

Every message is a length-prefixed binary frame:

```
+---------+---------+-----------------+
| len u32 | opcode  | payload (len-1) |
| (BE)    | u8      | bytes           |
+---------+---------+-----------------+
```

- `len` — big-endian uint32, the total payload length in bytes
  **including** the opcode byte. Minimum 1 (an opcode with no
  fields), maximum **1 MiB** (hard limit; anything longer is a
  protocol error and the peer MUST close the connection).
- `opcode` — u8 identifying the message type (see tables below).
- `payload` — opcode-specific fields, laid out in a fixed order.
  `len - 1` bytes.

### Primitive types used in payloads

| Type | Wire form |
|---|---|
| `u8` | 1 byte |
| `u16` | 2 bytes, big-endian |
| `u32` | 4 bytes, big-endian |
| `u64` | 8 bytes, big-endian |
| `i32` | 4 bytes, big-endian two's complement |
| `string` | `u32 length` + `length` bytes UTF-8 (no NUL terminator) |
| `bytes` | `u32 length` + `length` bytes, no encoding assumed |
| `array<T>` | `u32 count` + `count` items of type T |

Strings are length-prefixed rather than NUL-terminated so they can
carry arbitrary bytes safely (including actual NULs, if the daemon
wants), and so the C side never needs to scan for a terminator.

### Correlation IDs

Every daemon → wrapper message that expects a reply carries a `u64
correlation_id` as its **first** payload field (after the opcode).
The wrapper echoes it in the matching reply. Wrapper → daemon
events (unsolicited notifications: child exit, stream end, EOF)
carry `correlation_id = 0`, which is reserved and never allocated
by the daemon.

Correlation IDs are assigned by the daemon and MAY be sparse.
Wrappers MUST treat them as opaque tokens.

## v2 Connection lifecycle

```
wrapper                                        daemon
   │                                              │
   │──── connect() ──────────────────────────────►│
   │                                              │
   │──── HELLO { wire_version: 2, argv[], pid } ─►│
   │                                              │
   │◄─── HELLO_OK { daemon_version, session: u64 }│
   │                                              │
   │──── SNAPSHOT { handles[], streams[], blob } ►│
   │         (may be empty on first connect)      │
   │                                              │
   │◄─── RUN / CONNECT / STREAM / SET_STATE / ... │
   │      (operational — daemon drives)           │
   │                                              │
   │───► CHILD_EXITED / STREAM_ENDED / HANDLE_EOF │
   │      (async events whenever they occur)      │
   │                                              │
   │   ... eventually ...                         │
   │                                              │
   │◄─── EXIT { code: u8 }  OR  socket closure    │
```

The wrapper's opening move is always HELLO + SNAPSHOT, in that
order. SNAPSHOT is mandatory even on the very first connection
(when all lists are empty and the blob is absent); this keeps the
reconnect and first-connect code paths identical.

After SNAPSHOT the daemon is in charge. It issues instructions and
the wrapper executes them, interleaved with asynchronous events the
wrapper pushes whenever underlying fds change state.

## v2 Handle model

A **handle** is an opaque `u64` allocated by the wrapper to identify
a child process, a TCP connection, or an active stream. Handles are
stable across a single wrapper lifetime and are re-announced on
reconnect via SNAPSHOT. Handles are NOT stable across wrapper
restarts (a new wrapper process allocates from zero).

The wrapper reserves two handles with fixed IDs for its own stdio:

- `AGENT_IN = 1` — read side, bytes from the agent's stdin.
- `AGENT_OUT = 2` — write side, bytes to the agent's stdout.

These handles always exist; they cannot be `close`d by the daemon.
All other handles use IDs `>= 3`, allocated sequentially by the
wrapper.

Every handle has an associated **tag** — an opaque `string` (up to
256 bytes) that the daemon supplied when creating the handle. The
wrapper stores tags verbatim and returns them in SNAPSHOT. Tags are
how the daemon identifies what a handle is for after its own
restart; the wrapper never interprets them.

## v2 State blob

The daemon holds per-wrapper state (cached MCP initialize, in-flight
JSON-RPC ids, captured session IDs for HTTP backends, etc.) and
pushes snapshots of it to the wrapper whenever it changes. The
wrapper stores the blob atomically, overwriting any prior value,
and returns it unchanged in its next SNAPSHOT.

- Blob size is bounded: **64 KiB** maximum. A daemon that needs to
  push more than 64 KiB is asked to reconsider; the wrapper rejects
  oversized blobs with an ERROR event.
- The blob is accompanied by a `schema_version: u32` that the
  wrapper also stores verbatim. Schema migration (from older
  schemas to current) is entirely the daemon's concern on read.
- There is exactly ONE blob per wrapper. SET_STATE fully replaces
  the previous blob.

The invariant: **the daemon is stateless between restarts. Every
restart of the daemon is a rediscovery, not a recovery** — the new
daemon reassembles its model from the SNAPSHOTs its reconnecting
wrappers send, using the blobs as its primary memory.

## v2 Opcode reference — daemon → wrapper

All daemon-originated opcodes have `correlation_id: u64` as their
first payload field. The wrapper's reply (success or failure)
echoes that correlation_id.

| Opcode | Name | Payload (after correlation_id) |
|---:|---|---|
| 0x01 | HELLO_OK | `string daemon_version`, `u64 session_id` |
| 0x10 | RUN | `string cmd`, `array<string> argv`, `string tag` |
| 0x11 | CONNECT | `string host`, `u16 port`, `string tag` |
| 0x12 | CLOSE | `u64 handle` |
| 0x13 | WAIT | `u64 handle` |
| 0x14 | WRITE | `u64 handle`, `bytes data` |
| 0x15 | READ | `u64 handle`, `u32 max`, `u32 timeout_ms` |
| 0x16 | STREAM | `u64 src`, `u64 dst`, `string tag` |
| 0x17 | STOP_STREAM | `u64 stream_handle` |
| 0x18 | SET_STATE | `bytes blob`, `u32 schema_version` |
| 0x19 | SNAPSHOT_REQUEST | (no further fields) |
| 0x1A | EXIT | `u8 code` |

Notes:

- **RUN** fork/execs `cmd` with `argv` (argv[0] is the program
  name, per POSIX convention). `tag` is stored with the child
  handle. The child inherits the wrapper's env and cwd. Reply is
  `HANDLE_CREATED` on success with the new `u64 handle` and the
  child's `u32 pid`, or `ERROR` on fork/exec failure.
- **CONNECT** opens a blocking TCP socket to `host:port`. Host must
  resolve to a loopback address in v2.0 (wrapped by the daemon's
  URL validator; the wrapper does not re-check). Reply is
  `HANDLE_CREATED` on success.
- **CLOSE** tears down a handle. For child handles this closes the
  stdin/stdout pipes (triggering the child's own exit) but does
  NOT forcibly signal the child; WAIT is used to reap after.
- **WAIT** registers interest in a child's exit. Reply is deferred
  until the child exits; at that point a `CHILD_EXITED` event is
  sent with the correlation_id echoed, plus the exit status.
- **WRITE** writes `data` to the handle synchronously. Reply is
  `WRITE_OK` with the bytes written (always equal to `len(data)`
  on success; short writes are retried internally) or `ERROR`.
- **READ** reads up to `max` bytes from the handle with an optional
  timeout. Reply is `READ_RESULT` with `bytes data` and an `eof`
  flag. Zero bytes + eof=true means the peer closed. Zero bytes
  + eof=false means timeout.
- **STREAM** sets up continuous forwarding from `src` to `dst`.
  Returns `HANDLE_CREATED` with the stream's own handle. Steady-
  state bytes flow inside the wrapper with zero daemon involvement
  until STOP_STREAM or src EOF.
- **STOP_STREAM** cancels an active stream. Any bytes already
  read from src but not yet written to dst are drained before
  teardown. Reply is `OK`.
- **SET_STATE** replaces the wrapper's stored state blob. Reply is
  `OK` or `ERROR` (e.g. on size limit).
- **SNAPSHOT_REQUEST** asks the wrapper to re-emit a `SNAPSHOT`
  event now. Rarely used (the wrapper sends SNAPSHOT unprompted
  after every hello); a debug aid for tooling.
- **EXIT** tells the wrapper to shut down cleanly. All streams
  stop, all children receive SIGTERM (and SIGKILL on grace
  timeout), all connections close, and the wrapper process exits
  with `code`.

## v2 Opcode reference — wrapper → daemon

Replies carry the correlation_id echoed from the originating
daemon message. Events carry `correlation_id = 0`.

| Opcode | Name | Payload (after correlation_id) |
|---:|---|---|
| 0x81 | HELLO | `u8 wire_version` (=2), `string wrapper_version`, `u32 pid`, `array<string> argv` |
| 0x82 | SNAPSHOT | `array<HandleEntry> handles`, `array<StreamEntry> streams`, `bytes blob`, `u32 schema_version` |
| 0x83 | HANDLE_CREATED | `u64 handle`, opcode-specific extras (see below) |
| 0x84 | WRITE_OK | `u32 bytes_written` |
| 0x85 | READ_RESULT | `bytes data`, `u8 flags` (bit 0 = eof) |
| 0x86 | OK | (no further fields) |
| 0x87 | ERROR | `u8 code`, `string detail` |
| 0x88 | CHILD_EXITED | `u64 handle`, `i32 exit_status` |
| 0x89 | STREAM_ENDED | `u64 stream_handle`, `u8 reason`, `u64 bytes_copied` |
| 0x8A | HANDLE_EOF | `u64 handle` |

### HandleEntry (inside SNAPSHOT)

```
u64    handle
u8     kind            // 0 = child, 1 = connection
string tag
// kind-specific tail:
//   child:      string cmd, array<string> argv, u32 pid
//   connection: string host, u16 port
```

### StreamEntry (inside SNAPSHOT)

```
u64 stream_handle
u64 src_handle
u64 dst_handle
u64 bytes_copied
```

### HANDLE_CREATED extras

- From RUN: `u32 pid`.
- From CONNECT: (no extras — just the handle).
- From STREAM: (no extras — just the stream's own handle).

### STREAM_ENDED reasons

| Value | Meaning |
|---:|---|
| 0 | `src_eof` — source handle closed by peer |
| 1 | `stopped` — daemon issued STOP_STREAM |
| 2 | `write_error` — destination write failed |
| 3 | `read_error` — source read failed with a non-EOF error |

### ERROR codes

| Value | Name | Meaning |
|---:|---|---|
| 0x01 | BAD_OPCODE | wrapper received an opcode it doesn't know |
| 0x02 | BAD_PAYLOAD | payload failed to decode |
| 0x03 | BAD_HANDLE | handle does not exist |
| 0x04 | HANDLE_WRONG_KIND | op is not supported on this handle kind |
| 0x05 | RUN_FAILED | fork/exec failed |
| 0x06 | CONNECT_FAILED | TCP connect failed |
| 0x07 | IO_ERROR | read/write to underlying fd failed |
| 0x08 | OVERSIZE | payload exceeds the 1 MiB frame limit or 64 KiB blob |
| 0x09 | SHUTTING_DOWN | wrapper refuses new work during its shutdown |

The `detail` string is a human-readable explanation suitable for
logs. Daemons MUST NOT branch on its exact contents.

## v2 Daemon-less fallback

If the wrapper cannot dial the daemon at startup (socket missing,
connection refused, permission denied), its behaviour is:

- **With `-- COMMAND ARGS`:** the wrapper falls back to a minimal
  stdio bridge implemented entirely in C. It fork/execs the
  command, sets up `stream(AGENT_IN → child_stdin)` and
  `stream(child_stdout → AGENT_OUT)` internally, and exits on
  either EOF. No upgrade handling. No state preservation. Matches
  the pre-daemon UX of a bare stdio proxy.
- **With `--url URL`:** fatal error. HTTP support requires the
  daemon in the data path; there is no standalone HTTP mode. The
  wrapper prints a diagnostic and exits with code 1.

If the daemon is up at startup but later disappears mid-session,
the wrapper's behaviour depends on whether any streams are active:

- **Active `stream`s continue forwarding bytes.** The daemon is not
  in the data path for streams; its absence is invisible to
  steady-state traffic.
- **In-flight `read` / `write` operations targeting a daemon reply
  fail.** The wrapper cannot complete a request whose answer was
  meant to go to the daemon.
- **HTTP wraps break.** Since HTTP requires the daemon to parse
  responses, in-flight HTTP tool calls time out agent-side. This
  is a documented cost of the architecture and will not be fixed
  in v2.0.

The wrapper reconnects with 1s → 2s → 4s → 5s backoff (same as v1)
and re-issues HELLO + SNAPSHOT on success. The new daemon sees the
existing state and reconciles.

## v2 Reconnect and state reconciliation

On every successful dial (first connect or reconnect), the wrapper:

1. Sends HELLO with its wrapper version, wire_version=2, pid, and
   the argv it was launched with.
2. Immediately sends SNAPSHOT listing every handle, every active
   stream, and the current state blob.
3. Awaits HELLO_OK. Only after HELLO_OK does it accept further
   daemon instructions.

The daemon processes SNAPSHOT by:

1. Reading the blob (possibly migrating its schema forward if
   `schema_version < current`).
2. For each handle, looking at the tag. If the tag matches a
   config the daemon knows about, the handle is adopted into the
   daemon's model. If the tag is unknown, the daemon logs and
   sends CLOSE for that handle.
3. For each stream, the daemon records it. Streams are generally
   safe to adopt — they're just byte forwarders.
4. For each config the daemon expected but didn't see a matching
   handle for, it initiates a fresh session (new RUN or CONNECT).

Forward compatibility: SNAPSHOT may grow additional entry kinds in
future wire versions. Daemons MUST ignore HandleEntry kinds they
don't recognise rather than erroring; the wrapper's kind byte is
the authority. Similarly, the wrapper MUST ignore opcodes with a
high bit reserved for future events.

## v2 Size and resource limits

- Maximum frame size: **1 MiB**. Peers MUST close on violation.
- Maximum state blob size: **64 KiB**.
- Maximum tag length: **256 bytes**.
- Maximum string length: frame-size-bounded (there's no separate
  per-string cap; strings that fit in the frame are accepted).
- Maximum array count: frame-size-bounded similarly.
- Maximum concurrent handles per wrapper: implementation-defined,
  but 64 is a comfortable target. The wrapper MAY ERROR on
  further RUN/CONNECT past its internal limit.

## v2 Versioning and evolution

The `wire_version` byte in HELLO is **frozen at 2** for this
wrapper. Future wrappers can ship a `wire_version = 3` and bring
new opcodes, new fields in existing opcodes, or a different frame
layout entirely. Daemons MUST keep the v2 codec forever so that
any deployed v2 wrapper keeps working after a daemon upgrade.

Within v2, the following evolutions are forward-compatible:

- **New opcodes** may be added. The wrapper MUST reply with
  ERROR / BAD_OPCODE to any it doesn't know. The daemon MAY probe
  for support via SNAPSHOT_REQUEST or similar benign opcodes
  before relying on new ones.
- **New HandleEntry kinds** may be added. Daemons MUST ignore
  unknown kinds in SNAPSHOT.
- **New STREAM_ENDED reason values** may be added. Daemons MUST
  treat unknown reasons as "stream ended unexpectedly."

Evolutions that REQUIRE a wire version bump:

- Changing an existing opcode's payload layout.
- Changing an existing field's semantics.
- Adding a required field to an existing message.

In practice: the T4 charter says this wrapper should not need to
change after 1.0. If we ever find ourselves wanting to bump to
`wire_version = 3`, it means the T-piece abstraction failed and
we should reconsider the design rather than iterate on the spec.
