# mcpbridge

Keep your MCP server sessions alive across upgrades.

mcpbridge is split into two processes:

- **`mcpbridge`** (C) — a tiny wrapper that your MCP client launches in
  place of the real MCP server. It speaks MCP transparently to the
  upstream agent on stdio, and reaches the wrapped server through
  whichever backend it was told to use — either spawning a child
  process (stdio backend, via `-- COMMAND [ARGS...]`) or connecting
  to a localhost MCP Streamable HTTP endpoint (HTTP backend, via
  `--url http://localhost:PORT/path`). It cycles the backend when
  told to, without interrupting the agent's session.
- **`mcpbridge-daemon`** (Go) — one long-lived process per user session,
  typically run under `brew services`. It reads per-server config files,
  polls upgrade sources (Homebrew formulas, GitHub releases), performs
  the upgrades, and tells connected wrappers when it's time to reload.

The two processes talk over a Unix domain socket. When the daemon isn't
running, the wrapper still works — it just runs as a pure bridge with
no upgrade handling. The wrapped MCP server, regardless of backend,
requires no modification and cannot tell it is being proxied.

## Why two languages?

The wrapper is C for stability and a small runtime surface. It runs
per MCP server, so keeping it small and dependency-light matters.
The C code evolves as MCP and its transports evolve, but at a rate
bounded by those upstream specs — not by our own internal churn.

The daemon runs exactly once per user session, so its resource
footprint is irrelevant. Go gets us HTTPS, JSON, fsnotify, goroutines,
and a mature standard library essentially for free — and the code that
talks to `api.github.com` and Homebrew benefits from all of that.

## History

Earlier commits of this repo contained a Go library for building
daemon+proxy MCP servers. That library had a single consumer and served
a different problem (daemonising MCP servers so expensive state would
survive proxy restarts). It has been removed; the consumer will be
reworked separately to target the new binary pair.

## Quick start

```sh
brew tap marcelocantos/tap
brew install mcpbridge
brew services start mcpbridge
```

Drop a per-server config in `~/.config/mcpbridge/`
(see [docs/config-schema.md](docs/config-schema.md)) and point
your MCP client at `mcpbridge` in place of the real server.
The full walkthrough is in
[docs/packaging.md](docs/packaging.md).

Prefer to have your agent do it? Give it this prompt:

```
Install mcpbridge from https://github.com/marcelocantos/mcpbridge:
brew install, start the service, drop a config in
~/.config/mcpbridge/, update the MCP client config to wrap each
server — either `mcpbridge -- <original command>` for a stdio
server or `mcpbridge --url http://localhost:PORT/path --config
NAME` for an HTTP MCP daemon — and restart the session. Follow
agents-guide.md in the repo — installation is a four-step process
and is not complete until the client has been restarted.
```

If you use an agentic coding tool, include
[agents-guide.md](agents-guide.md) in your project context.

## Status

Work in progress. See `bullseye.yaml` for the roadmap.

## Build from source

```sh
make            # builds both wrapper and daemon
make test       # runs both test suites
./wrapper/mcpbridge --version
./daemon/mcpbridge-daemon --version
```

Requires a C11 compiler, POSIX.1-2008, and Go 1.24 or later. Supported
platforms: macOS arm64 and Linux x86_64 / arm64.

## License

Apache 2.0. See `LICENSE`. Vendored third-party code retains its own
license — see `wrapper/vendor/cjson/LICENSE` for cJSON.
