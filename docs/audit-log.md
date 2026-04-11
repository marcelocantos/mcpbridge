# Audit log

Maintenance actions taken on this repo by `/audit`, `/docs`,
`/release`, `/open-source`, and related skills. Newest entries at
the top. See `~/.claude/skills/audit-log-convention.md` for the
format.

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
