# mcpbridge config schema

Version: **2**

Per-server JSON config files describe one wrapped MCP server: how
to reach it. Two consumers read the same file:

- **The wrapper** (`mcpbridge connect <path>`) reads exactly the
  file it's pointed at and uses the connection fields to bring up
  the backend.
- **The daemon** (`mcpbridge-daemon`) scans well-known directories,
  reads every `*.json` file it finds, and uses the `name` field to
  route reload broadcasts to the right wrapper when the binary
  changes on disk.

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
  "args": []
}
```

| Field | Type | Required | Default | Description |
|---|---|---|---|---|
| `schema` | int | yes | — | Schema version. Must be `2`. Schema v1 is no longer accepted. |
| `name` | string | yes | — | Identifier the wrapper sends to the daemon on registration; the daemon routes reload broadcasts by this name. Must be unique across all loaded configs. |
| `command` | string | one of `command`/`url` | — | **Stdio backend.** Absolute path to the wrapped server's binary. Tilde-expanded (`~/...`, `~user/...`) at parse time. |
| `args` | array of string | no | `[]` | **Stdio backend.** Arguments passed verbatim to `execvp`; no shell expansion. |
| `url` | string | one of `command`/`url` | — | **HTTP backend.** Plain `http://` URL to a localhost MCP Streamable HTTP endpoint. Host must be `localhost`, `127.0.0.1`, or `::1`. `https://` and remote hosts are out of scope for v1. |

Exactly one of `command` or `url` must be set. The wrapper picks
the backend based on which field is populated.

> **Note on backwards compatibility**: the `source`, `upgrade`, and
> `check_interval` fields from older configs are silently ignored —
> they were removed in v0.8.0 when the daemon's upgrade-polling
> backends were retired. Existing config files with these fields
> continue to load without error.

## Example configs

### mnemo via brew, stdio backend

```json
{
  "schema": 2,
  "name": "mnemo",
  "command": "/opt/homebrew/bin/mnemo"
}
```

### vellum, HTTP backend

```json
{
  "schema": 2,
  "name": "vellum",
  "url": "http://localhost:9000/mcp"
}
```

### tilde expansion for command

```json
{
  "schema": 2,
  "name": "my-dev-server",
  "command": "~/code/my-mcp-server/bin/my-mcp-server",
  "args": ["--config", "~/code/my-mcp-server/dev.toml"]
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

The `name` field is unchanged.
