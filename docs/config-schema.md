# mcpbridge config schema

Version: **2**

Per-server JSON config files describe one wrapped MCP server: how
to reach it, how to check for new versions, and whether to upgrade
automatically. Two consumers read the same file:

- **The wrapper** (`mcpbridge connect <path>`) reads exactly the
  file it's pointed at and uses the connection fields to bring up
  the backend.
- **The daemon** (`mcpbridge-daemon`) scans well-known directories,
  reads every `*.json` file it finds, and uses the upgrade fields
  to poll for new versions and signal the wrapper to reload.

The wrapper takes a path on the command line, so it has no
discovery convention. The daemon's discovery dirs (in order) are:

1. `~/.config/mcpbridge/`
2. `$HOMEBREW_PREFIX/share/mcpbridge/` — typically populated by a
   brew formula on install
3. `/usr/local/share/mcpbridge/` — alternate system dir for
   non-Apple-Silicon Homebrew installs

Filename is irrelevant to identity — the daemon reads `name` from
inside the file. Conventionally, file name matches `name` for
grep-ability.

## Envelope

```json
{
  "schema": 2,
  "name": "mnemo",
  "command": "/opt/homebrew/bin/mnemo",
  "args": [],
  "source": { ... },
  "upgrade": "notify",
  "check_interval": "1h"
}
```

| Field | Type | Required | Default | Description |
|---|---|---|---|---|
| `schema` | int | yes | — | Schema version. Must be `2`. Schema v1 is no longer accepted. |
| `name` | string | yes | — | Identifier the wrapper sends to the daemon on registration; the daemon routes reload broadcasts by this name. Must be unique across all loaded configs. |
| `command` | string | one of `command`/`url` | — | **Stdio backend.** Absolute path to the wrapped server's binary. Tilde-expanded (`~/...`, `~user/...`) at parse time. |
| `args` | array of string | no | `[]` | **Stdio backend.** Arguments passed verbatim to `execvp`; no shell expansion. |
| `url` | string | one of `command`/`url` | — | **HTTP backend.** Plain `http://` URL to a localhost MCP Streamable HTTP endpoint. Host must be `localhost`, `127.0.0.1`, or `::1`. `https://` and remote hosts are out of scope for v1. |
| `source` | object | yes | — | Upgrade source backend — see [Sources](#sources). |
| `upgrade` | enum | no | `notify` | One of `off`, `notify`, `auto`. See [Upgrade modes](#upgrade-modes). |
| `check_interval` | duration | no | `1h` | How often to poll the source. Accepts Go duration strings (`30m`, `1h`, `6h`). |

Exactly one of `command` or `url` must be set. The wrapper picks
the backend based on which field is populated.

## Sources

### `brew`

Used for MCP servers installed via Homebrew.

```json
"source": {
  "type": "brew",
  "formula": "marcelocantos/tap/mnemo"
}
```

| Field | Type | Required | Description |
|---|---|---|---|
| `type` | `"brew"` | yes | Discriminator. |
| `formula` | string | yes | Full formula name, including tap prefix if needed. |

The daemon calls `brew outdated --json=v2 --formula <formula>` to
check for updates and `brew upgrade <formula>` to install them.

### `github`

Used for MCP servers distributed via GitHub releases.

```json
"source": {
  "type": "github",
  "repo": "owner/mcp-foo",
  "asset": "mcp-foo-{version}-{os}-{arch}.tar.gz",
  "binary_in_archive": "mcp-foo",
  "checksum_asset": "SHA256SUMS"
}
```

| Field | Type | Required | Description |
|---|---|---|---|
| `type` | `"github"` | yes | Discriminator. |
| `repo` | string | yes | `owner/repo`. |
| `asset` | string | no | Template for the asset name. `{version}` is the tag without a leading `v`, `{tag}` is the raw tag, `{os}` and `{arch}` are the Go runtime values (`darwin`, `linux`, `arm64`, `amd64`). |
| `binary_in_archive` | string | no | When the asset is a `.tar.gz` / `.tgz`, the name of the binary inside the archive to extract. Ignored for plain-binary assets. |
| `checksum_asset` | string | no | Name of a `SHA256SUMS`-style asset in the same release. If set, the daemon downloads this, finds the line for the chosen asset, and rejects the install on mismatch. |

The daemon fetches `/repos/<repo>/releases/latest` from
`api.github.com`, compares the tag against a cached "last seen"
value (so first-poll-per-config establishes a baseline without a
spurious install), and — in `auto` mode — downloads the asset,
optionally verifies its SHA256, extracts a named binary from a
tarball if applicable, and atomically replaces the installed binary.

## Upgrade modes

| Mode | Behaviour |
|---|---|
| `off` | Config is parsed and the wrapper sees `config_found=true, polling=false`, but the daemon never calls the source backend. Useful for disabling auto-upgrade for a single server without deleting the file. |
| `notify` | **Default.** Detects new versions and logs them, but does not install. The wrapper still cycles its child when the user runs `brew upgrade` (or equivalent) themselves — fsnotify catches the file change and pushes a reload. |
| `auto` | Detects **and** installs new versions, then pushes a targeted reload so the wrapper cycles its backend to pick up the new binary. |

## Example configs

### mnemo via brew, stdio backend, notify only

```json
{
  "schema": 2,
  "name": "mnemo",
  "command": "/opt/homebrew/bin/mnemo",
  "source": {
    "type": "brew",
    "formula": "marcelocantos/tap/mnemo"
  }
}
```

(`notify` + `1h` interval are defaults.)

### vellum via github, HTTP backend, fully automatic

```json
{
  "schema": 2,
  "name": "vellum",
  "url": "http://localhost:9000/mcp",
  "source": {
    "type": "github",
    "repo": "owner/vellum",
    "asset": "vellum-{version}-{os}-{arch}.tar.gz",
    "binary_in_archive": "vellum",
    "checksum_asset": "SHA256SUMS"
  },
  "upgrade": "auto",
  "check_interval": "30m"
}
```

### tilde expansion for command

```json
{
  "schema": 2,
  "name": "my-dev-server",
  "command": "~/code/my-mcp-server/bin/my-mcp-server",
  "args": ["--config", "~/code/my-mcp-server/dev.toml"],
  "source": {
    "type": "github",
    "repo": "me/my-mcp-server"
  }
}
```

`command` is tilde-expanded at parse time; `args` are not (no
shell expansion — they're passed verbatim to `execvp`).

## Migration from schema v1

Schema v1 (the v0.3.0 form) had no connection metadata in the
config file — connection details came from argv (`-- COMMAND` or
`--url URL`). Pre-1.0, no auto-migration is provided. Edit each
file once:

1. Set `"schema": 2`.
2. Add `"command": "/path/to/binary"` (and optional `"args": [...]`)
   for stdio servers, or `"url": "http://localhost:PORT/path"` for
   HTTP servers.
3. Update your MCP client config to use `mcpbridge connect <path>`
   instead of `mcpbridge -- COMMAND` / `mcpbridge --url URL --config NAME`.

The `name`, `source`, `upgrade`, and `check_interval` fields are
unchanged.
