# Stability

mcpbridge is a pre-1.0 project. This document tracks the public
interaction surface and what still needs to settle before a 1.0
release.

Snapshot as of: **v0.2.0**.

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

Flags — each entry shows default if any and current stability:

| Flag | Arg | Default | Stability |
|---|---|---|---|
| `-- COMMAND [ARGS...]` | argv tail | one of `--` or `--url` is required | **stable** — tail-argv convention is non-negotiable for an MCP-client launched binary |
| `--url URL` | string | — | **stable** — plain `http://` loopback URLs only in v1; https + remote hosts are rejected. Mutually exclusive with `-- COMMAND`. |
| `--config NAME` | string | `basename(argv[1])` (stdio only; required for `--url`) | **needs review** — is tail-derived default good enough for `npx`/`uvx`/`python -m`? |
| `--socket PATH` | string | `resolve_daemon_socket_path()` | **stable** |
| `-v`, `--verbose` | — | off | **stable** |
| `--version` | — | — | **stable** |
| `--help`, `-h` | — | — | **stable** |
| `--help-agent` | — | — | **stable** |

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

### Config schema v1 (per-server JSON)

Authoritative reference: [`docs/config-schema.md`](docs/config-schema.md).

Envelope fields:

| Field | Type | Required | Default | Stability |
|---|---|---|---|---|
| `schema` | int | yes | — | **stable** (v1) |
| `name` | string | yes | — | **stable** |
| `source` | object | yes | — | **stable** (see per-type below) |
| `upgrade` | `off`\|`notify`\|`auto` | no | `notify` | **stable** |
| `check_interval` | Go duration | no | `1h` | **stable** |

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

2. **Wrapper config name inference for wrapped launchers.** When
   the wrapped server is launched via `npx @pkg/name`, `uvx pkg`,
   or `python -m pkg`, the `basename(argv[1])` default resolves
   to `npx` / `uvx` / `python` rather than the actual server
   name. Current workaround is to pass `--config NAME`
   explicitly. Before 1.0, decide whether this is documented-as-
   required or whether we grow smarter inference.

3. **API surface continuity across reloads.** When the new child
   after a reload exposes a different surface than the old one,
   the agent's worldview has to catch up.

   **What v0.2.0 ships:** after a successful replay, dispatch
   unconditionally emits `notifications/tools/list_changed`,
   `notifications/prompts/list_changed`, and
   `notifications/resources/list_changed` upstream. Agents react
   by refetching each list, so added / removed / renamed /
   reschemaed items on the new child become visible without any
   diffing state on the wrapper side. This is the common case —
   an MCP server upgrade that adds a tool or tightens a schema.

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
   wrapper supports both stdio and HTTP backends (🎯T3, landed
   post-v0.2.0). The HTTP path accepts only plain `http://` to
   loopback hosts (`localhost` / `127.0.0.1` / `::1`); `https://`
   and remote hosts are rejected at launch. There is no standing
   GET SSE stream — inbound notifications arrive via POST-response
   SSE streams. Widening any of these (TLS, remote hosts, standing
   GET SSE) is additive and can land post-1.0 without breaking
   existing users.

5. **No GitHub release publishing automation beyond what's
   shipped.** The Homebrew formula publishing is automated via
   homebrew-releaser, but version strings in source files remain
   injected from `VERSION=<tag>` at `make` time — there's no
   check that a commit hasn't accidentally hardcoded `0.0.0-dev`
   somewhere that bypasses the injection. A pre-tag lint would
   be nice.

6. **No mnemo-side integration.** The old Go library in this repo
   was deleted on the assumption mnemo would be reworked to target
   the new wrapper. That rework lives in mnemo's own repo and is
   not blocking this project's 1.0, but it's the canary that
   tells us whether the wire protocol survives real use.

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

Surface item count (rough): ~30 public items (CLI flags × 2
binaries + wire protocol message types + config schema fields +
environment variables + signals). That puts us in the 20–50
bracket → **minimum 2 months settling period** before 1.0
eligibility.

Clock starts at the last breaking change to the interaction
surface. As of v0.1.0, the clock starts **now**, so the earliest
1.0 release is at least 2 months out — assuming no breaking
changes land in the meantime.
