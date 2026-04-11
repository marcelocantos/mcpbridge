# Third-party attributions

mcpbridge bundles the following third-party code. Each entry's
upstream license applies to the corresponding files in this repo.

## cJSON

- **Version**: 1.7.18
- **Upstream**: https://github.com/DaveGamble/cJSON
- **License**: MIT
- **Location**: `wrapper/vendor/cjson/`
- **License text**: `wrapper/vendor/cjson/LICENSE`

Used by `mcpbridge` (the C wrapper) to parse and emit JSON-RPC
messages on stdio and the daemon UDS.

---

## fsnotify

- **Version**: v1.9.0
- **Upstream**: https://github.com/fsnotify/fsnotify
- **License**: BSD 3-Clause
- **Location**: Go module dependency (`daemon/go.mod`)

Used by `mcpbridge-daemon` (the Go daemon) to watch wrapped-binary
paths for out-of-band changes.

## golang.org/x/sys

- **Version**: v0.13.0
- **Upstream**: https://pkg.go.dev/golang.org/x/sys
- **License**: BSD 3-Clause
- **Location**: indirect Go module dependency of fsnotify
  (`daemon/go.sum`)
