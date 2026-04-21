# Packaging and install

This document walks from a fresh machine to a working mcpbridge
install with the daemon running under `brew services` and one
wrapped MCP server configured for auto-upgrade.

## Prerequisites

- macOS or Linux (Windows isn't supported)
- Homebrew installed (macOS: [brew.sh](https://brew.sh); Linux: Linuxbrew)
- An MCP client (e.g. Claude Code) whose config you can edit

## 1. Install

```sh
brew tap marcelocantos/tap
brew install mcpbridge
```

This installs two binaries:

- `mcpbridge` — the C wrapper the MCP client launches per server
- `mcpbridge-daemon` — the Go daemon that polls for upgrades

Plus two empty directories for config files:

- `$HOMEBREW_PREFIX/etc/mcpbridge/` (reserved)
- `$HOMEBREW_PREFIX/share/mcpbridge/` (system-wide config files)

## 2. Start the daemon

```sh
brew services start mcpbridge
```

`brew services` runs `mcpbridge-daemon` under launchd (macOS) or
systemd (Linux), restarting it if it dies. Logs go to
`$HOMEBREW_PREFIX/var/log/mcpbridge-daemon.log`.

Verify:

```sh
tail -f $(brew --prefix)/var/log/mcpbridge-daemon.log
```

You should see lines like:

```
INFO config: loaded count=0 dirs="[~/.config/mcpbridge ...]"
INFO listening path=~/Library/Caches/mcpbridge/daemon.sock version=0.1.0
```

(Socket path is macOS — Linux uses `$XDG_RUNTIME_DIR/mcpbridge/daemon.sock`.)

## 3. Drop a config

Tell the daemon how to check for upgrades for each MCP server you
want to wrap. Config schema is documented in
[config-schema.md](./config-schema.md).

Example — mnemo installed via Homebrew:

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

Bounce the daemon so it picks up the new config:

```sh
brew services restart mcpbridge
```

## 4. Point your MCP client at mcpbridge

In your MCP client config (Claude Code: `~/.claude.json` or the
project-level equivalent), replace each MCP server command with a
call to `mcpbridge`. Two backend forms are supported; pick the
one that matches how the wrapped server runs.

**Stdio backend (spawn a child — the common case):**

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

The `--` separator tells mcpbridge where its own flags end and the
wrapped command begins. Any flags the real MCP server expects go
after the command, e.g.:

```json
"args": ["--", "mcp-foo", "--some-flag", "value"]
```

**HTTP backend (connect to a localhost MCP Streamable HTTP
endpoint, for servers that run as standalone daemons — mnemo as
of v0.20.0 is one):**

```json
{
  "mcpServers": {
    "mnemo": {
      "command": "/opt/homebrew/bin/mcpbridge",
      "args": [
        "--url", "http://localhost:19419/mcp",
        "--config", "mnemo"
      ]
    }
  }
}
```

v1 restrictions on `--url`:
- Plain `http://` only; `https://` is rejected.
- Host must be `localhost`, `127.0.0.1`, or `::1`.
- `--config NAME` is required.
- `--url` and `-- COMMAND` are mutually exclusive.

Restart your MCP client. For stdio, mcpbridge fork/execs the child
and bridges stdio; for HTTP, it POSTs to the URL and streams
responses back. Either way, the wrapped server requires no
modification and cannot tell it is being proxied. From then on,
whenever the daemon notices a new version (either via its poll or
because you ran `brew upgrade` yourself), it pushes a reload
notification, mcpbridge cycles the backend transparently, and
your agent's MCP session continues without reconnecting.

## Troubleshooting

**The daemon won't start.** Look at
`$HOMEBREW_PREFIX/var/log/mcpbridge-daemon.log`. Most errors are
config-related (unknown schema version, bad JSON, unknown source
type). Fix the offending file and `brew services restart mcpbridge`.

**The wrapper says "daemon unreachable".** The daemon isn't
running. Start it with `brew services start mcpbridge` or run it
in the foreground to diagnose: `mcpbridge-daemon -v`.

**Reloads never happen.** Check that `config_found=true, polling=true`
on register:

```sh
MCPBRIDGE_SOCKET=... mcpbridge -v -- your-server 2>&1 | grep registered
```

If `polling=false`, the config's `upgrade` mode is `off`, or the
config name doesn't match what the wrapper is registering as.

**A wrapped server is named something the daemon can't infer.**
Pass `--config NAME` before the `--` separator in the client config:

```json
"args": ["--config", "mnemo", "--", "/path/to/different-binary"]
```

## Release workflow (for maintainers)

See `packaging/homebrew/mcpbridge.rb`. On each release:

1. Cut a tag: `git tag v0.1.0 && git push --tags`
2. GitHub creates a source tarball at
   `https://github.com/marcelocantos/mcpbridge/archive/refs/tags/v0.1.0.tar.gz`
3. `shasum -a 256` the tarball
4. Update `url` and `sha256` in `packaging/homebrew/mcpbridge.rb`
5. Copy the updated formula into `marcelocantos/homebrew-tap/Formula/`
6. Push the tap. Users `brew update && brew upgrade mcpbridge`.

Automating this with a GitHub Actions release workflow is a
follow-up task.
