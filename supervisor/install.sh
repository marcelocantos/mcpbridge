#!/bin/sh
# Install the Supervisor program and transfer ownership of the user socket
# from Homebrew's launchd service. Re-running the installer is safe.
set -eu

REPO="$(CDPATH='' cd "$(dirname "$0")/.." && pwd)"
CONF_DIR="${SUPERVISOR_CONF_DIR:-/opt/homebrew/etc/supervisor.d}"
DEST="$CONF_DIR/mcpbridge-daemon.ini"
TEMPLATE="$REPO/supervisor/mcpbridge-daemon.ini"

if [ -z "${HOME:-}" ]; then
  HOME="$(eval echo ~"$(id -un)")"
  export HOME
fi

mkdir -p "$CONF_DIR" "$HOME/.local/var/log"
chmod +x "$REPO/supervisor/run-mcpbridge-daemon.sh"
sed "s|@REPO@|$REPO|g" "$TEMPLATE" >"$DEST"
echo "rendered $DEST (from $TEMPLATE)"

if [ "${SUPERVISOR_SKIP_CTL:-}" = 1 ]; then
  exit 0
fi
if ! command -v supervisorctl >/dev/null 2>&1; then
  echo "supervisorctl not on PATH; start Supervisor after installing it" >&2
  exit 1
fi

if command -v brew >/dev/null 2>&1; then
  brew services stop mcpbridge >/dev/null 2>&1 || true
fi
if command -v launchctl >/dev/null 2>&1; then
  launchctl bootout "gui/$(id -u)/homebrew.mxcl.mcpbridge" 2>/dev/null || true
fi

supervisorctl reread
supervisorctl update mcpbridge-daemon
supervisorctl restart mcpbridge-daemon 2>/dev/null || supervisorctl start mcpbridge-daemon
supervisorctl status mcpbridge-daemon
