# Packaging and install

This document walks from a fresh machine to a working mcpbridge
install with the daemon running under `brew services` and one
wrapped MCP server configured.

The daemon does not auto-upgrade. Upgrades happen via the user's
normal install path (`brew upgrade`, manual `gh release download`,
etc.); the daemon's fsnotify watcher detects the binary change and
fires a targeted reload so the wrapper cycles its child seamlessly.

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
- `mcpbridge-daemon` — the Go daemon that watches for binary changes
  and notifies wrappers to reload

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
INFO listening path=~/Library/Caches/mcpbridge/daemon.sock version=0.4.0
```

(Socket path is macOS — Linux uses `$XDG_RUNTIME_DIR/mcpbridge/daemon.sock`.)

## 3. Drop a config

The same JSON file describes both how to reach the wrapped MCP
server (for the wrapper) and the server's name (for the daemon to
route reloads). Schema is documented in [config-schema.md](./config-schema.md).

Example — mnemo as a stdio child, installed via Homebrew:

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

For an HTTP MCP server (one that runs as a localhost daemon
itself), use `url` instead of `command`+`args`:

```json
{
  "schema": 2,
  "name": "mnemo",
  "url": "http://localhost:19419/mcp"
}
```

Bounce the daemon so it picks up the new config:

```sh
brew services restart mcpbridge
```

## 4. Point your MCP client at mcpbridge

In your MCP client config (Claude Code: `~/.claude.json` or the
project-level equivalent), replace each MCP server entry with a
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

The same launch command works regardless of whether the wrapped
server is stdio or HTTP — the config file's `command`/`url` field
selects the backend transparently. If the wrapped server later
moves from stdio to HTTP (or vice versa), only the config file
changes; the agent's MCP client config does not.

`<path>` may begin with `~` (home dir) or `~user`, or be an
absolute or cwd-relative path.

Restart your MCP client. The wrapped server requires no
modification and cannot tell it is being proxied. From then on,
whenever you upgrade a wrapped server (`brew upgrade mnemo`,
`gh release download`, or any other install method), the daemon's
fsnotify watcher detects the binary change, pushes a targeted
reload notification, mcpbridge cycles the backend transparently,
and your agent's MCP session continues without reconnecting.

## Troubleshooting

**The daemon won't start.** Look at
`$HOMEBREW_PREFIX/var/log/mcpbridge-daemon.log`. Most errors are
config-related (unknown schema version, bad JSON). Fix the
offending file and `brew services restart mcpbridge`.

**The wrapper says "daemon unreachable".** The daemon isn't
running. Start it with `brew services start mcpbridge` or run it
in the foreground to diagnose: `mcpbridge-daemon -v`.

**The wrapper exits immediately with "schema v1 is not supported".**
You have an old v0.3.0 config file. Edit it: set `"schema": 2` and
add a `"command": "..."` (stdio) or `"url": "..."` (HTTP) field
describing the connection. See `docs/config-schema.md`.

**Reloads never happen.** Check that `config_found=true` on
register:

```sh
MCPBRIDGE_SOCKET=... mcpbridge -v connect /path/to/config.json 2>&1 | grep registered
```

If `config_found=false`, the config name doesn't match what the
wrapper is registering as. The daemon watches the `child_binary`
path the wrapper reports at register time — confirm that path
points at the real binary on disk.

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
