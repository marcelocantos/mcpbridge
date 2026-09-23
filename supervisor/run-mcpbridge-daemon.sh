#!/bin/sh
# Run the installed daemon on the shared user socket. A checkout build must
# not silently replace the version used by every active MCP client.
set -eu

if [ -z "${HOME:-}" ]; then
  HOME="$(eval echo ~"$(id -un)")"
  export HOME
fi
export USER="${USER:-$(id -un)}"
export PATH="/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:${HOME}/.cargo/bin:${HOME}/.local/bin:${HOME}/.py/bin:${HOME}/go/bin:/usr/bin:/bin:/usr/sbin:/sbin"

if [ -n "${MCPBRIDGE_DAEMON_BIN:-}" ]; then
  BIN="$MCPBRIDGE_DAEMON_BIN"
else
  if ! command -v brew >/dev/null 2>&1; then
    echo "mcpbridge-daemon: Homebrew is required; set MCPBRIDGE_DAEMON_BIN to override" >&2
    exit 1
  fi
  BIN="$(brew --prefix)/opt/mcpbridge/bin/mcpbridge-daemon"
fi

if [ ! -x "$BIN" ]; then
  echo "mcpbridge-daemon: binary is not executable: $BIN" >&2
  exit 1
fi
if [ "${1:-}" = "--print-bin" ]; then
  printf '%s\n' "$BIN"
  exit 0
fi

echo "mcpbridge-daemon: running $BIN" >&2
exec "$BIN"
