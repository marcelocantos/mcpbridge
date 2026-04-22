# Audit log

Maintenance actions taken on this repo by `/audit`, `/docs`,
`/release`, `/open-source`, and related skills. Newest entries at
the top. See `~/.claude/skills/audit-log-convention.md` for the
format.

## 2026-04-22 — /release v0.3.0

- **Commit**: `pending`
- **Outcome**: Shipped the HTTP backend (🎯T3) and the daemon
  brew-source launchd fix (🎯T2). `mcpbridge` now accepts
  `--url http://localhost:PORT/path` in addition to the existing
  `-- COMMAND` stdio form; HTTP scope is plain `http://` to
  loopback hosts only. Smoke-verified against live mnemo 0.21.0.
  The daemon's brew source now resolves `brew` via `$PATH` with a
  fallback scan of `/opt/homebrew/bin`, `/usr/local/bin`, and
  `/home/linuxbrew/.linuxbrew/bin`, and correctly parses the JSON
  stdout from `brew outdated` even when the command exits with
  status 1 (which it does whenever a formula IS outdated). New
  `MCPBRIDGE_BREW_PATH` env var overrides the resolver. An
  architectural overhaul (🎯T4, "dumb T-piece") was explored and
  abandoned after a design discussion concluded the existing
  0.3.x architecture was the right shape; the v2 wire-protocol
  draft was reverted in the same pass. `STABILITY.md` bumped to
  v0.3.0, settling clock reset to 2026-04-22.

## 2026-04-12 — /release v0.2.0

- **Commit**: `b9a3305`
- **Outcome**: Shipped fix for API surface continuity across
  reloads. After a successful child replay, dispatch now emits
  all three `*_list_changed` notifications (tools / prompts /
  resources) upstream so the agent refetches each list from the
  new child. Previously the wrapper silently replaced the cached
  init response and left the agent with a stale surface view —
  material regression in tool availability whenever a wrapped
  server's upgrade added, removed, renamed, or reschemaed any
  tool. `STABILITY.md` restructured to frame the fix as the
  first instalment of a broader "API surface continuity" gap;
  capabilities diff and reload_ack failure-status handling are
  still pre-1.0 prerequisites. The previously-unused
  `mcp_build_tools_list_changed` helper was refactored into a
  generic `mcp_build_list_changed(kind, out_len)`. Test coverage
  added at every layer (mcp_test for the builder, dispatch_test
  for the replay success + error paths, e2e_reload_test.sh for
  the wire-level assertion).

## 2026-04-12 — /release v0.1.0

- **Commit**: `ca306cd`
- **Outcome**: Prepared the project's first release. Added
  `NOTICES.md` (cJSON + fsnotify + golang.org/x/sys attributions),
  `agents-guide.md` (full install walkthrough including the
  restart-the-client gotcha), `STABILITY.md` (pre-1.0 interaction
  surface catalogue + gaps), `.github/workflows/release.yml`
  (matrix build + homebrew-releaser job), and `--help-agent` on
  the Go daemon (wrapper already had it). Refreshed the repo
  description on GitHub — the previous text described the deleted
  Go library architecture, not the current wrapper+daemon system.
  Version strings in both binaries are injected from the Makefile
  `VERSION` variable, so no source update beyond the tag is
  needed. Target platforms: macOS arm64, Linux x86_64, Linux arm64.
