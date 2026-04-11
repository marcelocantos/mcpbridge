# mcpbridge — agent guide

If you're a coding agent working on a project that uses MCP
servers, this guide covers how mcpbridge fits in and what you need
to know to install, configure, and troubleshoot it.

## What it is

mcpbridge is a **transparent wrapper around MCP servers** that
keeps an agent's MCP session alive across server upgrades. It
solves one specific problem: upgrading a wrapped MCP server
normally kills the agent's session, forcing a reconnect and
losing whatever ephemeral state the agent was building up (open
handles, cached initialize state, in-flight tool calls). With
mcpbridge in front, an upgrade becomes a seamless child-process
swap that the agent never observes.

It's split into two processes:

- **`mcpbridge`** (C) — a tiny per-server wrapper that the MCP
  client launches in place of the real server. It speaks MCP to
  the agent on stdio, spawns the real server as a child, bridges
  every message, and cycles the child when the daemon tells it
  to. Per session, lives and dies with the MCP client.
- **`mcpbridge-daemon`** (Go) — one long-lived process per user
  session, typically run under `brew services`. Reads per-server
  JSON config files, polls upgrade sources (Homebrew formulas,
  GitHub releases), performs upgrades, and tells connected
  wrappers when to cycle their children.

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

The daemon does nothing until it has configs. Create
`~/.config/mcpbridge/` and drop a JSON file there for each MCP
server you want to wrap. Example for mnemo (a brew-installed MCP
server):

```sh
mkdir -p ~/.config/mcpbridge
cat >~/.config/mcpbridge/mnemo.json <<'EOF'
{
  "schema": 1,
  "name": "mnemo",
  "source": {
    "type": "brew",
    "formula": "marcelocantos/tap/mnemo"
  },
  "upgrade": "auto",
  "check_interval": "30m"
}
EOF
```

Full schema reference: `docs/config-schema.md`. Supported source
types: `brew` (for Homebrew formulas) and `github` (for binaries
distributed as GitHub releases). Upgrade modes: `off` / `notify` /
`auto` (default: `notify`).

Restart the daemon so it picks up new or changed configs:

```sh
brew services restart mcpbridge
```

### 4. Point your MCP client at mcpbridge

Replace each MCP server entry in your client config with a call
to `mcpbridge` that wraps the original command. For Claude Code,
edit `~/.claude.json` or the project-local equivalent:

```json
{
  "mcpServers": {
    "mnemo": {
      "command": "/opt/homebrew/bin/mcpbridge",
      "args": ["--", "mnemo"]
    }
  }
}
```

The `--` separator marks where mcpbridge's own flags end and the
wrapped command begins. Anything after `--` is forwarded verbatim
as the child's argv:

```json
"args": ["--", "mcp-foo", "--config", "/etc/foo.conf"]
```

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
corresponding log lines (verbose mode), the config file's `name`
field doesn't match what the wrapper is registering as. Either
fix the config file to match or pass `--config NAME` to the
wrapper in the MCP client config.

## Quick start prompt

Copy-paste this into your agent if you just want it installed:

```
Install mcpbridge from https://github.com/marcelocantos/mcpbridge:

  1. `brew tap marcelocantos/tap && brew install mcpbridge`
  2. `brew services start mcpbridge`
  3. Drop `~/.config/mcpbridge/mnemo.json` (or whichever server)
     per the schema in docs/config-schema.md
  4. Edit the MCP client config so each wrapped server's command
     becomes `mcpbridge -- <original command>` — then restart the
     client.

  Installation is NOT complete until step 4 has run and the MCP
  client has been restarted. Verify by calling a tool on the
  wrapped server and checking the daemon log for a `registered`
  line. Do not use curl — the daemon socket is not HTTP.

  Follow agents-guide.md in the repo for the full walkthrough.
```

## CLI reference

### `mcpbridge` (the wrapper)

```
Usage: mcpbridge [OPTIONS] -- COMMAND [ARGS...]

Options:
  --config NAME    config name for daemon registration
                   (default: basename of COMMAND)
  --socket PATH    override daemon socket path
  -v, --verbose    extra logging to stderr
  --version        print version and exit
  --help           print this help and exit
  --help-agent     print agent-oriented help and exit
```

One `mcpbridge` process is launched per wrapped MCP server by the
MCP client. It stays alive for the duration of the agent session.
It is not a daemon and is not started manually.

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

Full reference: `docs/config-schema.md`. Short form:

```json
{
  "schema": 1,
  "name": "mnemo",
  "source": { "type": "brew", "formula": "marcelocantos/tap/mnemo" },
  "upgrade": "notify",
  "check_interval": "1h"
}
```

- `name` **must** match what the wrapper registers as, which is
  `basename(argv[1])` unless the wrapper was passed `--config NAME`.
- `upgrade` defaults to `notify` (detect + log, don't install).
  Use `auto` to let the daemon drive the install. `off` disables
  polling for that server.
- `source.type` is `brew` or `github`. See `docs/config-schema.md`
  for the type-specific fields.

## Troubleshooting

**The wrapper logs "daemon unreachable" at startup.** The daemon
isn't running. `brew services start mcpbridge`, or run
`mcpbridge-daemon -v` in the foreground to see startup errors.

**The wrapper registers but `reload` never arrives.** Check
`config_found` / `polling` in register_ok. If either is false,
the config name doesn't match, the config file failed to parse,
or the upgrade mode is `off`. Check
`$(brew --prefix)/var/log/mcpbridge-daemon.log` for parse errors.

**A wrapped server is named something the daemon can't infer.**
Pass `--config NAME` before the `--` separator in the MCP client
config:

```json
"args": ["--config", "mnemo", "--", "/path/to/different-binary"]
```

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
  your MCP client config without a `-- COMMAND` after it does
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
- **First-poll baselining**. On the very first scheduler tick for
  a GitHub-source config, no install happens even if the latest
  release is newer than what's on disk. The daemon can't tell
  which release you have installed locally without asking the
  binary (which many binaries can't tell you anyway), so it caches
  "whatever GitHub says is current right now" as the baseline and
  triggers only when that value changes. If you need an immediate
  upgrade, `SIGHUP` the daemon after the next real release, or
  run `brew upgrade` yourself and let fsnotify pick up the change.
- **Wire protocol v1 vs. schema v1 are independent**. The
  daemon↔wrapper UDS protocol has its own schema version
  (`v: 1` in envelopes). The per-server JSON config files have
  their own schema version (`"schema": 1`). They're bumped
  independently.
