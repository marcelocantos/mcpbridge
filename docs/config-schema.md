# mcpbridge config schema

Version: **1**

Per-server JSON config files describe how the daemon should check
for and install a new version of a wrapped MCP server. Files live
in (in precedence order):

1. `~/.config/mcpbridge/<name>.json` — user-local, wins over system
2. `$HOMEBREW_PREFIX/share/mcpbridge/<name>.json` — system-wide,
   typically populated by a brew formula
3. `/usr/local/share/mcpbridge/<name>.json` — alternate system dir
   for non-Apple-Silicon Homebrew installs

The filename is a convention, not a requirement — the daemon reads
`name` from inside the file. Conventionally, file name matches
`name` for grep-ability.

## Envelope

```json
{
  "schema": 1,
  "name": "mnemo",
  "source": { ... },
  "upgrade": "notify",
  "check_interval": "1h"
}
```

| Field | Type | Required | Default | Description |
|---|---|---|---|---|
| `schema` | int | yes | — | Config schema version. Must be `1` for this daemon. |
| `name` | string | yes | — | Config name. Must match what the wrapper registers as (derived from `basename(argv[1])` unless the wrapper was passed `--config NAME`). |
| `source` | object | yes | — | Upgrade source backend — see [Sources](#sources). |
| `upgrade` | enum | no | `notify` | One of `off`, `notify`, `auto`. See [Upgrade modes](#upgrade-modes). |
| `check_interval` | duration | no | `1h` | How often to poll the source. Accepts Go duration strings (`30m`, `1h`, `6h`). |

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
tarball if applicable, and atomically replaces the installed binary
at the wrapper's registered `child_binary` path.

## Upgrade modes

| Mode | Behaviour |
|---|---|
| `off` | Config is parsed and the wrapper sees `config_found=true, polling=false`, but the daemon never calls the source backend. Useful for disabling auto-upgrade for a single server without deleting the file. |
| `notify` | **Default.** Detects new versions and logs them, but does not install. The wrapper still cycles its child when the user runs `brew upgrade` (or equivalent) themselves — fsnotify catches the file change and pushes a reload. |
| `auto` | Detects **and** installs new versions, then pushes a targeted reload so the wrapper cycles its child to pick up the new binary. |

## Example configs

### mnemo via brew, notify only

```json
{
  "schema": 1,
  "name": "mnemo",
  "source": {
    "type": "brew",
    "formula": "marcelocantos/tap/mnemo"
  }
}
```

(`notify` + `1h` interval are defaults.)

### mcp-foo via github, fully automatic

```json
{
  "schema": 1,
  "name": "mcp-foo",
  "source": {
    "type": "github",
    "repo": "owner/mcp-foo",
    "asset": "mcp-foo-{version}-{os}-{arch}.tar.gz",
    "binary_in_archive": "mcp-foo",
    "checksum_asset": "SHA256SUMS"
  },
  "upgrade": "auto",
  "check_interval": "30m"
}
```
