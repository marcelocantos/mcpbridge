# mcpbridge

Keep your MCP server sessions alive across upgrades.

mcpbridge is split into two processes:

- **`mcpbridge`** (C) — a tiny per-server wrapper that your MCP
  client launches in place of the real MCP server. The MCP client
  invokes it as

      mcpbridge connect <path-to-config-file>

  The config file describes how to reach the wrapped server —
  either as a stdio child process or as a localhost MCP Streamable
  HTTP endpoint. The wrapper reads the file, brings up the backend,
  and bridges every message. Backend-transport changes (e.g., a
  server moving from stdio to HTTP at upgrade) are absorbed in the
  config file — the agent's launch command never changes.
- **`mcpbridge-daemon`** (Go) — one long-lived process per user
  session, typically run under `brew services`. Reads the same
  config files, polls upgrade sources (Homebrew formulas, GitHub
  releases), performs the upgrades, and tells connected wrappers
  when it's time to reload.

The two processes talk over a Unix domain socket. When the daemon
isn't running, the wrapper still works — it just runs as a pure
bridge with no upgrade handling. The wrapped MCP server, regardless
of backend, requires no modification and cannot tell it is being
proxied.

## Why two languages?

The wrapper is C for stability and a small runtime surface. It runs
per MCP server, so keeping it small and dependency-light matters.
The C code evolves as MCP and its transports evolve, but at a rate
bounded by those upstream specs — not by our own internal churn.

The daemon runs exactly once per user session, so its resource
footprint is irrelevant. Go gets us HTTPS, JSON, fsnotify,
goroutines, and a mature standard library essentially for free —
and the code that talks to `api.github.com` and Homebrew benefits
from all of that.

## History

Earlier commits of this repo contained a Go library for building
daemon+proxy MCP servers. That library had a single consumer and
served a different problem (daemonising MCP servers so expensive
state would survive proxy restarts). It has been removed; the
consumer was reworked separately.

## Quick start

```sh
brew tap marcelocantos/tap
brew install mcpbridge
brew services start mcpbridge
```

Drop a per-server config file in `~/.config/mcpbridge/`
(see [docs/config-schema.md](docs/config-schema.md)) and point your
MCP client at it:

```json
{
  "mcpServers": {
    "mnemo": {
      "command": "mcpbridge",
      "args": ["connect", "~/.config/mcpbridge/mnemo.json"]
    }
  }
}
```

The full walkthrough is in [docs/packaging.md](docs/packaging.md).

Prefer to have your agent do it? Give it this prompt:

```
Install mcpbridge from https://github.com/marcelocantos/mcpbridge:
brew install, start the service, drop a config file in
~/.config/mcpbridge/ for each MCP server you want to wrap (per
docs/config-schema.md), update the MCP client config to launch
each server as `mcpbridge connect ~/.config/mcpbridge/<name>.json`,
and restart the session. Follow agents-guide.md in the repo —
installation is a four-step process and is not complete until the
client has been restarted.
```

If you use an agentic coding tool, include
[agents-guide.md](agents-guide.md) in your project context.

## Resilience

mcpbridge keeps your agent's MCP session alive across upstream
restarts. The agent's stdio session is the source of truth; the
upstream MCP server is a replaceable component behind it.

There are two restart paths, both invisible to the agent:

- **Daemon-driven reload.** When `mcpbridge-daemon` notices a newer
  version of the upstream is installed (e.g. via `brew upgrade`), it
  broadcasts a reload to every connected wrapper. The wrapper drains
  its in-flight requests, restarts the upstream, replays the cached
  `initialize` + `notifications/initialized` against the new instance,
  and resumes — without the agent ever seeing a disconnect or having
  to re-issue a request.

- **Autonomous self-reload.** When the upstream restarts on its own
  (a manual `brew services restart`, a crash, an HTTP server cycling)
  it forgets the wrapper's session id. The next request comes back as
  `400 Bad Request: Invalid session ID`. mcpbridge detects this,
  re-runs the cached handshake against the upstream (capturing a
  fresh session id), and replays the in-flight request that triggered
  the failure. The agent sees one extra round-trip of latency on that
  call and nothing else.

Both paths emit `notifications/tools/list_changed`,
`prompts/list_changed`, and `resources/list_changed` to the agent
after the re-handshake, so the agent re-fetches its tool / prompt /
resource registry against the (possibly upgraded) upstream — no
client restart needed.

What is **not** currently resilient: a permanently unreachable
upstream (TCP connect fails repeatedly). In that case mcpbridge
exits and the agent's MCP session ends. Configurable dead-upstream
timeout with stdio-client preservation is planned — see 🎯T7.1.

## Status

Work in progress. See `bullseye.yaml` for the roadmap.

## Build from source

```sh
make            # builds both wrapper and daemon
make test       # runs both test suites
./wrapper/mcpbridge --version
./daemon/mcpbridge-daemon --version
```

Requires a C11 compiler, POSIX.1-2008, and Go 1.24 or later.
Supported platforms: macOS arm64 and Linux x86_64 / arm64.

## License

Apache 2.0. See `LICENSE`. Vendored third-party code retains its
own license — see `wrapper/vendor/cjson/LICENSE` for cJSON.
