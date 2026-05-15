# mcpbridge — agent guide

If you're a coding agent working on a project that uses MCP
servers, this guide covers how mcpbridge fits in and what you need
to know to install, configure, and troubleshoot it.

## What it is

mcpbridge is a **transparent bridge for MCP servers** that keeps
an agent's MCP session alive across server upgrades. It solves
one specific problem: upgrading an MCP server normally kills the
agent's session, forcing a reconnect and losing whatever ephemeral
state the agent was building up (open handles, cached initialize
state, in-flight tool calls). With mcpbridge in front, an upgrade
becomes a seamless backend cycle that the agent never observes.

The agent always launches mcpbridge the same way:

    mcpbridge connect <path-to-config-file>

The config file describes how to reach the wrapped server —
either:

- **stdio**: a `command` + optional `args` that mcpbridge
  fork/execs as a child.
- **http**: a localhost MCP Streamable HTTP `url` that mcpbridge
  POSTs to on behalf of the agent and streams responses back from.

The agent always talks to mcpbridge over stdio regardless of
which backend the wrapped server uses. Backend-transport changes
(e.g., a server moving from stdio to HTTP at upgrade) are absorbed
in the config file — the agent's launch command never changes.
The wrapped server requires no modification and cannot tell it is
being proxied.

It's split into two processes:

- **`mcpbridge`** (C) — a tiny per-server wrapper that the MCP
  client launches in place of the real server. It speaks MCP to
  the agent on stdio, talks to the backend on either stdio or
  http, bridges every message, and cycles the backend when the
  daemon tells it to. Per session, lives and dies with the MCP
  client.
- **`mcpbridge-daemon`** (Go) — one long-lived process per user
  session, typically run under `brew services`. Reads the same
  per-server config files, watches each registered wrapper's child
  binary via fsnotify, and tells connected wrappers to cycle their
  backends when the binary changes on disk.

They talk over a user-local Unix domain socket. When the daemon
isn't running, the wrapper still works — it just runs as a pure
bridge with no upgrade handling.

## Installation (multi-step — not complete until every step succeeds)

Installing mcpbridge is **not one command**. All four steps below
must succeed before the system is doing its job. Stopping after
`brew install` leaves you with a binary that isn't running and
isn't wrapping anything.

### 1. Install both binaries

```sh
brew tap marcelocantos/tap
brew install mcpbridge
```

This installs `mcpbridge` and `mcpbridge-daemon` into
`$(brew --prefix)/bin`.

### 2. Start the daemon as a service

```sh
brew services start mcpbridge
```

This launches `mcpbridge-daemon` under launchd (macOS) or systemd
(Linux) with keep-alive. Logs go to
`$(brew --prefix)/var/log/mcpbridge-daemon.log`.

**Verification**: before going further, confirm the daemon is
running and the socket exists:

```sh
# Is the daemon process alive?
pgrep -fl mcpbridge-daemon

# Has it created its socket?
ls -la ~/Library/Caches/mcpbridge/daemon.sock   # macOS
ls -la $XDG_RUNTIME_DIR/mcpbridge/daemon.sock   # Linux
```

**Do NOT `curl` or telnet the socket to test it.** It's a Unix
domain socket speaking newline-delimited JSON envelopes — not
HTTP. A plain connection attempt returns nothing useful and will
mislead you into thinking the daemon is broken. Just confirm the
socket file exists and is mode 0600.

### 3. Drop at least one per-server config

The config file is the single source of truth for both the wrapper
(connection details) and the daemon (name-based reload routing).
Create
`~/.config/mcpbridge/` and drop one schema-v2 JSON file per MCP
server.

Stdio example (mnemo as a child process via brew):

```sh
mkdir -p ~/.config/mcpbridge
cat >~/.config/mcpbridge/mnemo.json <<'EOF'
{
  "schema": 2,
  "name": "mnemo",
  "command": "/opt/homebrew/bin/mnemo"
}
EOF
```

HTTP example (mnemo as a localhost daemon):

```sh
cat >~/.config/mcpbridge/mnemo.json <<'EOF'
{
  "schema": 2,
  "name": "mnemo",
  "url": "http://localhost:19419/mcp"
}
EOF
```

Full schema reference: `docs/config-schema.md`. `command` is
tilde-expanded; `args` are passed verbatim to `execvp`.

Restart the daemon so it picks up new or changed configs:

```sh
brew services restart mcpbridge
```

### 4. Point your MCP client at mcpbridge

In your MCP client config, replace each MCP server entry with a
call to `mcpbridge connect <path>`:

```json
{
  "mcpServers": {
    "mnemo": {
      "command": "/opt/homebrew/bin/mcpbridge",
      "args": ["connect", "~/.config/mcpbridge/mnemo.json"]
    }
  }
}
```

The config file's `command`/`url` field selects the backend
transparently. Switching mnemo from a stdio backend to an HTTP
backend (or vice versa) means editing the config file — the MCP
client config does not change.

`<path>` may begin with `~` (home dir) or `~user` (named user's
home dir), or be an absolute or cwd-relative path.

**Restart the MCP client** so it picks up the new launch command.
Until you do this, the MCP client is still spawning the old direct
command and nothing is wrapped. This is the step people forget.

### Installation is only complete after all four steps

If you stopped at step 2, the daemon is running but wraps nothing.
If you stopped at step 3, the daemon knows what to poll but no
wrapper is registered. If you stopped at step 4 without restarting
the client, the new config isn't live. **Do not report success
until the client has been restarted and the wrapped server is
responding through mcpbridge.**

### Post-restart verification

After the MCP client restart, the cleanest way to confirm
everything is wired up is to ask the wrapped MCP server for
something it can answer. For mnemo that's any stats or memory
query; for any MCP server, `tools/list` works.

Then check the daemon log for the registration:

```sh
tail -n 20 $(brew --prefix)/var/log/mcpbridge-daemon.log
```

Look for lines like:

```
INFO conn: hello
INFO conn: registered name=mnemo child_pid=... child_binary=...
```

If `name=` shows up but `config_found` is `false` in the
corresponding log lines (verbose mode), the registered name does
not match any loaded config — check that the file's `name` field
matches what the wrapper registers (which is the config's `name`,
not the file path).

## Quick start prompt

Copy-paste this into your agent if you just want it installed:

```
Install mcpbridge from https://github.com/marcelocantos/mcpbridge:

  1. `brew tap marcelocantos/tap && brew install mcpbridge`
  2. `brew services start mcpbridge`
  3. Drop `~/.config/mcpbridge/<name>.json` per the schema in
     docs/config-schema.md, for each MCP server you want to wrap.
     The file describes the connection (command/args or url).
  4. Edit the MCP client config so each wrapped server's command
     becomes `mcpbridge connect ~/.config/mcpbridge/<name>.json`.
     Then restart the client.

  Installation is NOT complete until step 4 has run and the MCP
  client has been restarted. Verify by calling a tool on the
  wrapped server and checking the daemon log for a `registered`
  line. Do not use curl — the daemon socket is not HTTP.

  Follow agents-guide.md in the repo for the full walkthrough.
```

## CLI reference

### `mcpbridge` (the wrapper)

```
Usage: mcpbridge [OPTIONS] connect <path>

Options:
  --socket PATH    override daemon socket path
  -v, --verbose    extra logging to stderr
  --version        print version and exit
  --help           print this help and exit
  --help-agent     print agent-oriented help and exit
```

One `mcpbridge` process is launched per wrapped MCP server by the
MCP client. It stays alive for the duration of the agent session.
It is not a daemon and is not started manually.

`<path>` may begin with `~` (home dir) or `~user`, or be an
absolute or cwd-relative path. The file at `<path>` must be a
schema-v2 config with exactly one of `command` (stdio) or `url`
(http) set.

### `mcpbridge-daemon`

```
Usage: mcpbridge-daemon [OPTIONS]

Options:
  --socket PATH    override the UDS path (default: platform-specific)
  -v, --verbose    extra logging
  --version        print version and exit
  --help           print this help and exit

Signals:
  SIGHUP           broadcast a manual reload to all connected wrappers
  SIGINT, SIGTERM  clean shutdown
```

Runs under `brew services` in production. The `SIGHUP` knob is
useful in development for triggering a reload cycle end-to-end
without waiting for a real upgrade.

## Config schema at a glance

Full reference: `docs/config-schema.md`. Short form (stdio):

```json
{
  "schema": 2,
  "name": "mnemo",
  "command": "/opt/homebrew/bin/mnemo"
}
```

Or HTTP:

```json
{
  "schema": 2,
  "name": "mnemo",
  "url": "http://localhost:19419/mcp"
}
```

- Exactly one of `command` (with optional `args`) or `url` is
  required. The wrapper picks the backend based on which is set.
- `name` is the identifier the wrapper sends to the daemon on
  registration; it must be unique across all loaded configs.

## Troubleshooting

**The wrapper logs "daemon unreachable" at startup.** The daemon
isn't running. `brew services start mcpbridge`, or run
`mcpbridge-daemon -v` in the foreground to see startup errors.

**The wrapper exits immediately with "schema v1 is not supported".**
You have an old v0.3.0 config file. Edit it: set `"schema": 2` and
add a `"command": "..."` (or `"url": "..."`) field describing the
connection. See `docs/config-schema.md` for the migration recipe.

**The wrapper registers but `reload` never arrives.** Check
`config_found` in register_ok. If it is false, the config name
doesn't match or the config file failed to parse. Check
`$(brew --prefix)/var/log/mcpbridge-daemon.log` for parse errors.
If `config_found=true` but reloads still don't arrive, confirm
that `child_binary` on register points at the real binary that
gets replaced on upgrade — the daemon watches that exact path.

**The agent sees an initialize failure after a reload.** The
wrapped server is broken in a way that the daemon-driven upgrade
exposed — it isn't mcpbridge's fault. Check the wrapped server's
own logs; restart the agent manually; investigate the upstream
problem.

## What mcpbridge does NOT do

- **It does not expose tools.** It's a transport-level transparent
  proxy. The wrapped MCP server is what provides tools; mcpbridge
  just keeps the connection to it alive across upgrades.
- **It does not launch the daemon on demand.** Start the daemon
  via `brew services`. A wrapper that can't reach the daemon runs
  as a plain bridge with no upgrade handling.
- **It is not itself an MCP server.** Registering `mcpbridge` in
  your MCP client config without a `connect <path>` after it does
  nothing useful.
- **It does not speak HTTP on its socket.** The daemon's UDS uses
  newline-delimited JSON envelopes. Agents that reflexively try
  `curl http://localhost:...` or similar against it will find
  nothing.

## Gotchas

- **On-disk `MCPBRIDGE_VERSION` default is `0.0.0-dev`**. Release
  builds override this via `-DMCPBRIDGE_VERSION=<tag>` in the
  Makefile / `-ldflags` for the daemon. If you see `0.0.0-dev`
  you're running an unreleased local build.
- **macOS `poll()` doesn't reliably surface `POLLHUP` on a FIFO
  whose writers have all closed.** The wrapper works around this
  with a bounded poll timeout and an unconditional non-blocking
  `read()` probe on stdin every iteration. If you're debugging a
  hung wrapper, this is why the loop keeps ticking even when
  nothing is happening — it's intentional.
- **Wire protocol v1 vs. config schema v2 are independent**. The
  daemon↔wrapper UDS protocol has its own schema version
  (`v: 1` in envelopes). The per-server JSON config files have
  their own schema version (`"schema": 2`). They're bumped
  independently.
