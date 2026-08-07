# Stability

mcpbridge is a pre-1.0 project. This document tracks the public
interaction surface and what still needs to settle before a 1.0
release.

Snapshot as of: **v0.9.0**.

## Stability commitment

**Once mcpbridge ships 1.0, the public interaction surface below
becomes a backwards-compatibility contract.** After 1.0, breaking
changes to any of it — CLI flags, config file schema, wire
protocol, socket path rules, file-system layout — require a major
version bump, and the project's stance on major bumps is that they
fork the product rather than upgrade it in place.

Pre-1.0 exists so we can get this surface right before the
contract takes effect. Annotations below mark which items are
settled vs. still under review vs. actively fluid.

## Interaction surface

### `mcpbridge` CLI (C wrapper)

Invocation: `mcpbridge [OPTIONS] connect <path>`.

| Item | Arg | Default | Stability |
|---|---|---|---|
| `connect <path>` | path to schema-v2 config file | required | **stable** — the single agent-facing form. `<path>` accepts `~`/`~user` expansion and absolute or cwd-relative forms. |
| `--socket PATH` | string | `resolve_daemon_socket_path()` | **stable** |
| `-v`, `--verbose` | — | off | **stable** |
| `--version` | — | — | **stable** |
| `--help`, `-h` | — | — | **stable** |
| `--help-agent` | — | — | **stable** |

Removed in v0.4.0 (vs v0.3.0): `-- COMMAND [ARGS...]`, `--url URL`,
`--config NAME`. Each is rejected at startup with a one-line
migration message pointing at the `connect <path>` form. Pre-1.0
breakage; permitted.

Exit codes: 0 on clean shutdown, 1 on fatal error, 2 on usage
error. **stable**.

Signals handled: SIGINT, SIGTERM (clean shutdown), SIGPIPE
(ignored). **stable**.

Environment:
- `MCPBRIDGE_SOCKET` — overrides the daemon socket path. **stable**.

### `mcpbridge-daemon` CLI (Go daemon)

| Flag | Arg | Default | Stability |
|---|---|---|---|
| `--socket PATH` | string | platform default | **stable** |
| `-v`, `--verbose` | — | off | **stable** |
| `--version` | — | — | **stable** |
| `--help`, `-h` | — | — | **stable** |
| `--help-agent` | — | — | **stable** |

Signals handled: SIGHUP (broadcast manual reload), SIGINT,
SIGTERM (clean shutdown). **stable**.

Environment:
- `MCPBRIDGE_SOCKET` — overrides the UDS path. **stable**.
- `MCPBRIDGE_CONFIG_DIR` — overrides the config search path
  (primarily used by tests). **stable**.
- `MCPBRIDGE_BREW_PATH` — explicit path to the `brew` executable
  for the brew source backend. If unset, the daemon resolves via
  `$PATH`, then falls back to known install locations
  (`/opt/homebrew/bin/brew`, `/usr/local/bin/brew`,
  `/home/linuxbrew/.linuxbrew/bin/brew`). Added in v0.3.0.
  **stable**.

### Socket path resolution

Both binaries resolve the daemon socket path in the same order:

1. `$MCPBRIDGE_SOCKET` if set and non-empty
2. macOS: `$HOME/Library/Caches/mcpbridge/daemon.sock`
3. Linux: `$XDG_RUNTIME_DIR/mcpbridge/daemon.sock` if set and writable
4. Fallback: `$TMPDIR/mcpbridge-$UID/daemon.sock`
   (or `/tmp/mcpbridge-$UID/daemon.sock` if `$TMPDIR` unset)

The daemon `mkdir -p`s the parent directory with mode 0700 and
creates the socket with mode 0600. **stable**.

### Wire protocol v1 (daemon ↔ wrapper over UDS)

Authoritative reference: [`docs/wire-protocol.md`](docs/wire-protocol.md).

- **Framing**: newline-delimited JSON. One complete object per
  line terminated by `\n`. Max line length **256 KiB**; exceeding
  is a protocol error and the peer closes the connection.
- **Envelope**: every message includes `{t, v, seq}` plus type-
  specific fields. Unknown fields are ignored on receive.
- **Schema version**: `v: 1`. Both sides refuse to talk to an
  unknown `v`.

Message types (all **stable** for v1):

| Type | Direction | Purpose |
|---|---|---|
| `hello` | wrapper → daemon | first message, wrapper version + pid |
| `hello_ok` | daemon → wrapper | ack + daemon version |
| `register` | wrapper → daemon | name + child_pid + child_binary |
| `register_ok` | daemon → wrapper | config_found + polling |
| `deregister` | wrapper → daemon | clean disconnect |
| `reload` | daemon → wrapper | name + old_version + new_version + reason |
| `reload_ack` | wrapper → daemon | ack_seq + status (ok/drain_timeout/spawn_failed/init_failed) + detail |
| `shutdown` | daemon → wrapper | advisory "daemon going down" |
| `error` | either | code + detail, closes the connection |

Known `reason` strings on `reload`:
- `manual` (SIGHUP)
- `brew_upgrade`
- `github_release`
- `binary_changed` (fsnotify)

Known `status` values on `reload_ack`: `ok`, `drain_timeout`,
`spawn_failed`, `init_failed`.

**stable** — this is what 1.0 will commit to if shipped today.

### Config schema v2 (per-server JSON)

Authoritative reference: [`docs/config-schema.md`](docs/config-schema.md).

Envelope fields:

| Field | Type | Required | Default | Stability |
|---|---|---|---|---|
| `schema` | int | yes | — | **stable** (v2 — v1 is rejected with a migration message) |
| `name` | string | yes | — | **stable** — identifier the wrapper sends to the daemon on registration; the file path is no longer constrained to match. |
| `command` | string | one of `command`/`url` | — | **stable** — stdio backend; tilde-expanded at parse time. |
| `args` | array of string | no | `[]` | **stable** — passed verbatim to `execvp`; no shell expansion. |
| `url` | string | one of `command`/`url` | — | **stable** — plain `http://` loopback URLs only in v1 of the HTTP backend (`localhost` / `127.0.0.1` / `::1`); `https://` and remote hosts rejected. |
| `source` | object | yes | — | **stable** (see per-type below) |
| `upgrade` | `off`\|`notify`\|`auto` | no | `notify` | **stable** |
| `check_interval` | Go duration | no | `1h` | **stable** |
| `tool_call_timeout_ms` | int (≥0) | no | `300000` | **stable** — added in v0.6.0. Bounds how long any single tool call may wait for the upstream when the HTTP backend is in retry. `0` disables the bound (retry until the agent or some outer timeout intervenes). Idle wrapper does no upstream I/O regardless of value. |

Source `brew`:
- `type: "brew"`, `formula: "<string>"` — **stable**

Source `github`:
- `type: "github"`, `repo: "owner/name"` — **stable**
- `asset`, `binary_in_archive`, `checksum_asset` — **stable**
- Template vars in `asset`: `{version}`, `{tag}`, `{os}`, `{arch}` — **stable**

Config discovery paths:
1. `$MCPBRIDGE_CONFIG_DIR` (if set; overrides everything)
2. `$HOME/.config/mcpbridge/*.json`
3. `/opt/homebrew/share/mcpbridge/*.json`
4. `/usr/local/share/mcpbridge/*.json`

Name collisions resolve earlier-dir-wins. **stable**.

### File-system contract

- Config files: `~/.config/mcpbridge/<name>.json` (user) and
  `$prefix/share/mcpbridge/<name>.json` (system). **stable**.
- Socket: as documented above.
- Daemon logs: `$HOMEBREW_PREFIX/var/log/mcpbridge-daemon.log`
  (when run under `brew services`). **stable**.

### Resilience guarantees (HTTP backend)

What the agent observes across upstream restarts and outages.
Stable contract from v0.6.0 onward; additive changes only.

| Guarantee | Trigger | Behaviour |
|---|---|---|
| Daemon-driven reload | `mcpbridge-daemon` broadcasts `reload` (e.g. brew upgrade detected) | wrapper drains in-flight, re-handshakes upstream, replays cached `initialize` + `notifications/initialized`, emits `tools/list_changed` + `prompts/list_changed` + `resources/list_changed` upstream, sends `reload_ack`. Agent sees zero disconnect. **stable**. |
| Autonomous self-reload | Upstream returns 4xx (typically `400 Bad Request: Invalid session ID`) for a request bearing the cached MCP-Session-Id | wrapper detects the stale session, runs the daemon-less equivalent of the reload pathway (re-handshake + replay + list_changed broadcast). Idempotent reads on the replay-safe whitelist (`tools/list`, `prompts/list`, `prompts/get`, `resources/list`, `resources/read`, `resources/templates/list`, `resources/subscribe`, `resources/unsubscribe`, `roots/list`, `ping`, `logging/setLevel`, `completion/complete`, plus `initialize`/`notifications/initialized`) are queued and drained under the new session — agent sees zero error. Side-effecting requests (notably `tools/call` and `sampling/createMessage`) get a structured error per the next row rather than a silent retry. Added in v0.6.0; replay-safety split refined in v0.7.0. **stable**. |
| Side-effecting call across stale session | Upstream returns 4xx for an in-flight `tools/call` (or any non-whitelisted method) | wrapper synthesises a JSON-RPC error response to the agent (preserving the original `id`) carrying code **`-32002`** with message `"mcpbridge: upstream session reset during call; please retry"`. The reload cycle still runs so subsequent calls land cleanly under the new session. Agents that receive `-32002` may safely retry the call. Added in v0.7.0. **stable**. |
| Idle outage tolerance | Upstream becomes unreachable while no tool call is in flight | wrapper does no upstream I/O — there is no background pinger and no idle timeout. Outages of arbitrary length pass unobserved. The next tool call after the upstream returns succeeds via autonomous self-reload. Added in v0.6.0. **stable**. |
| In-flight outage tolerance | Upstream becomes unreachable *while* a tool call is waiting | wrapper retries connect with bounded backoff (100ms → 500ms → 1s → 2s → 5s, capped) up to `tool_call_timeout_ms` (default 5 min). On success: T7's reinit fires and the call lands under the new session. On deadline expiry: see "Tool-call timeout error". Added in v0.6.0. **stable**. |
| Tool-call timeout error | `tool_call_timeout_ms` elapses with the upstream still unreachable | wrapper synthesises a JSON-RPC error response to the agent (preserving the original `id`) and keeps the stdio session alive. Subsequent tool calls trigger fresh retries. Added in v0.6.0. **stable**. |

JSON-RPC error codes emitted by the wrapper:
- **`-32001`** — upstream unreachable past `tool_call_timeout_ms`,
  with message `"mcpbridge: upstream unreachable past timeout"` (or
  `"… during reinit"` if the timeout fired during recovery's
  replay). Added in v0.6.0. **stable**.
- **`-32002`** — upstream session reset during a side-effecting
  call, with message `"mcpbridge: upstream session reset during
  call; please retry"`. Added in v0.7.0. **stable**.
- **`-32003`** — the backend was cycled while the request was still
  outstanding against it, with message `"mcpbridge: backend cycled
  during call; please retry"`. Emitted for every request the old
  backend still owed an answer for at the moment it went away, each
  addressed to its own `id`. Added in v0.9.0. **stable**.

Operator-visible cycle-window log markers (paired, autonomous
cycles only):
- `upstream: cycling — session stale; …` at the start of every
  autonomous cycle.
- `upstream: cycling — complete; agent session resumed` at the
  return to RUNNING. Added in v0.7.0. **stable**.

### Resilience guarantees (daemon side)

What the agent observes across `mcpbridge-daemon` restarts and
absences. The behaviour itself has been operational since the
daemon-loop work landed (v0.1.0 era); it is promoted to the public
contract here so consumers can rely on it without reading the source.

| Guarantee | Trigger | Behaviour |
|---|---|---|
| Standalone-mode startup | Daemon socket absent at wrapper startup | wrapper logs `daemon unreachable at <sock>; continuing without it`, brings up the agent's stdio session, and forwards messages normally. Reconnect timer arms. **stable**. |
| Daemon disconnect tolerance | Daemon socket closes mid-session (clean shutdown, crash, SIGTERM, brew restart) | wrapper logs `daemon socket closed`, frees the daemon client, arms the reconnect timer. The agent's stdio session is unaffected — message forwarding continues. The wrapper does **not** exit. **stable**. |
| Daemon reconnect with backoff | Daemon socket is unavailable | wrapper retries the connect with capped exponential backoff: 1s → 2s → 4s → 5s (`BACKOFF_INITIAL_MS=1000`, `BACKOFF_MAX_MS=5000`). Each successful reconnect re-runs the full `hello` + `register` handshake, so the daemon learns about the wrapper's child-pid and config-name from scratch. **stable**. |
| Backend cycled with a request still outstanding | The drain ends via child exit rather than in-flight-zero — the backend died or was replaced while it still owed an answer | wrapper answers every outstanding request with a **`-32003`** JSON-RPC error carrying that request's own `id`, and resets its in-flight bookkeeping. Without the reset, the count never returns to zero and every *later* reload parks the wrapper in DRAINING permanently, queueing all subsequent requests — a session that is dead until the agent restarts. Added in v0.9.0. Verified by `tests/e2e_child_death_inflight_test.sh`. **stable**. |
| In-progress reload across daemon restart | Daemon dies after broadcasting `reload` but before the wrapper sends `reload_ack` | wrapper completes the reload cycle locally (DRAINING → SWAPPING → STARTING → RUNNING). The `reload_ack` send fails and is logged as a warn; the agent's session has already migrated to the new child. The next reconnect re-registers the wrapper from scratch. **stable**. |

The wrapper exits only on: stdin EOF, SIGINT, SIGTERM, or an FSM
transition to `FAILED`. Daemon socket loss never triggers any of
these. Verified by `tests/e2e_daemon_outage_test.sh`.

### Internal Go API

`daemon/internal/*` — **not public**. Go's `internal` mechanism
forbids import from outside `daemon/`. No external stability
commitment; refactor at will. This is by design — the daemon
exposes itself through the wire protocol and config schema, not
through library imports.

### Internal C API

`wrapper/src/*.h` — **not public**. No external consumers. No
stability commitment.

## Gaps and prerequisites for 1.0

These must be addressed before the v1.0.0 release can ship.

1. **Version-derivation for github source baselining.** The
   scheduler currently caches "whatever GitHub says is latest on
   the first poll" as the baseline, rather than asking the wrapped
   binary what version it is. This is safe but means the first
   poll per config is always a no-op. Before 1.0, pick one of:
   (a) accept this as intended behaviour and document it more
   prominently, or (b) add an optional `version_cmd` field to the
   config so the scheduler can learn the installed version by
   running a user-supplied command (e.g., `mnemo --version`).

2. **Resolved as of v0.4.0** — config-driven `name`. The wrapper no
   longer infers a name from the launch command; the config file's
   `name` field is authoritative for registration. Wrapped launchers
   like `npx @pkg/name` / `uvx pkg` / `python -m pkg` work without
   special handling because the user names the config explicitly.

3. **API surface continuity across reloads.** When the new child
   after a reload exposes a different surface than the old one,
   the agent's worldview has to catch up.

   **What v0.2.0 shipped:** after a successful replay, dispatch
   unconditionally emits `notifications/tools/list_changed`,
   `notifications/prompts/list_changed`, and
   `notifications/resources/list_changed` upstream. Agents react
   by refetching each list, so added / removed / renamed /
   reschemaed items on the new child become visible without any
   diffing state on the wrapper side. This is the common case —
   an MCP server upgrade that adds a tool or tightens a schema.

   **What v0.6.0 added:** the same broadcast now also fires on
   autonomous self-reload (4xx-stale-session detection in the
   HTTP backend), not just daemon-driven reload. Practically
   that means a `brew services restart <upstream>` outside the
   daemon's view — or any other autonomous upstream restart —
   no longer leaves the agent with a stale tool registry.

   **What's still deferred to 1.0:**

   - **capabilities diff.** MCP's `initialize` handshake
     negotiates `capabilities` once per session and has no
     mid-session renegotiation mechanism. If the new child
     advertises different capabilities, the wrapper today
     silently replaces the cached init response and the agent
     is left holding a stale view. The 1.0 fix is to parse the
     new init response, diff `capabilities` and
     `protocolVersion` against the cached one, log a structured
     warning when they differ, and surface the divergence via
     `reload_ack`'s `detail` field so the daemon can log a
     forensic record. That's not a fix (there's nothing we can
     do for the agent short of forcing a reconnect), but it
     turns a silent correctness bug into a visible operational
     one.

   - **reload_ack failure statuses acted on.** The `reload_ack`
     envelope already defines `drain_timeout`, `spawn_failed`,
     and `init_failed` values alongside `ok`, but nothing in
     the scheduler currently reacts to them — the daemon logs
     and continues, and the next poll triggers another upgrade
     attempt regardless. Before 1.0, the scheduler should
     treat non-`ok` ack statuses as signal to back off the
     source for that config (configurable cooldown) and surface
     a health indicator via a future `status` RPC.

   - **Eager tools/list diff instead of unconditional emission.**
     v0.2.0's "just emit three notifications" approach is
     correct but wasteful when the new child's surface is
     identical to the old one. A diff-on-the-wrapper-side
     implementation would suppress the notifications when the
     cached and new `tools/list` match byte-for-byte. Not
     urgent; the waste is a single refetch per reload.

4. **HTTP backend scope is loopback + plain HTTP only.** The
   wrapper supports both stdio and HTTP backends (selected per
   config file under v0.4.0's unified `connect <path>` form). The
   HTTP path accepts only plain `http://` to loopback hosts
   (`localhost` / `127.0.0.1` / `::1`); `https://` and remote hosts
   are rejected at config-load time. There is no standing GET SSE
   stream — inbound notifications arrive via POST-response SSE
   streams. Widening any of these (TLS, remote hosts, standing GET
   SSE) is additive and can land post-1.0 without breaking
   existing users.

5. **No GitHub release publishing automation beyond what's
   shipped.** The Homebrew formula publishing is automated via
   homebrew-releaser, but version strings in source files remain
   injected from `VERSION=<tag>` at `make` time — there's no
   check that a commit hasn't accidentally hardcoded `0.0.0-dev`
   somewhere that bypasses the injection. A pre-tag lint would
   be nice.

6. **mnemo integration path is via the HTTP backend.** The old
   Go library in this repo was deleted on the assumption mnemo
   would be reworked to target the new wrapper. mnemo 0.20.0
   (2026-04-18) took a different path: it collapsed to a single
   HTTP MCP daemon. Integration happens via mcpbridge's HTTP
   backend rather than a dedicated wrapper-aware library on
   mnemo's side. Smoke-verified during v0.3.0 prep against a live
   mnemo 0.21.0; under v0.4.0 the same setup works via a config
   file with `"url": "http://localhost:19419/mcp"`. Not a 1.0
   blocker; HTTP-backed flows still need more real-world use
   before the scope can be declared settled.

## Out of scope for 1.0

- **HTTPS for github source.** The daemon uses Go's `net/http`
  which supports HTTPS natively; no TLS work is needed in the
  current code. Flagging here only to note that we explicitly
  do not intend to stand up our own TLS stack.
- **Remote HTTP MCP servers over TLS from the wrapper.** The
  HTTP backend is plain-HTTP loopback only. TLS and remote hosts
  are out of scope for 1.0; wrapping a remote MCP server over
  HTTPS is a different product.
- **Windows.** macOS and Linux only.
- **`npm` / `pypi` / `go install` source backends.** Deferred
  indefinitely. The `brew` + `github` backends cover everything
  we actually ship.
- **Cross-user daemons.** One daemon per user session. No
  system-wide install.
- **Re-using the old mcpbridge-as-library shape.** The Go library
  that preceded this project is gone for good. 1.0 does not
  revive it.

## Settling clock

Surface item count (rough): ~40 public items (CLI flags × 2
binaries + wire protocol message types + config schema fields +
environment variables + signals + resilience guarantees). That
keeps us in the 20–50 bracket → **minimum 2 months settling
period** before 1.0 eligibility.

Clock starts at the last breaking change to the interaction
surface. v0.4.0 restructured the wrapper's CLI into the unified
`connect <path>` form, removed the v0.3.0 argv flags (`--`,
`--url`, `--config`), and bumped the config schema to v2 with new
`command`/`args`/`url` fields — a meaningful breaking change.
Settling clock restarts at **2026-04-25**. v0.5.0 was internal
hardening with no surface change. v0.6.0 adds
`tool_call_timeout_ms` (new optional field, default preserves
prior behaviour for any practical workload) and a Resilience
guarantees section documenting behaviour that pre-v0.6 left
implicit — both additive, both extending the contract rather than
narrowing it, neither resets the clock. v0.7.0 adds the
replay-safety split for autonomous self-reload (idempotent reads
replay; side-effecting calls surface `-32002` rather than silent
retry) plus paired cycle-window log markers — also additive, also
not a reset. Earliest 1.0 eligibility is therefore at least 2
months out from 2026-04-25, assuming no further breaking changes.
