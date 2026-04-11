// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

package socket

import (
	"fmt"
	"os"
	"os/user"
	"path/filepath"
	"runtime"
)

// DefaultSocketPath returns the socket path both the daemon and the
// wrapper resolve by default, following the rules in
// docs/wire-protocol.md. The precedence is:
//
//  1. $MCPBRIDGE_SOCKET (explicit override; also used by tests)
//  2. macOS: $HOME/Library/Caches/mcpbridge/daemon.sock
//     Linux: $XDG_RUNTIME_DIR/mcpbridge/daemon.sock (if set and writable)
//  3. Fallback on any platform: $TMPDIR/mcpbridge-$UID/daemon.sock
//     (or /tmp/mcpbridge-$UID/daemon.sock when TMPDIR is unset)
//
// This is a pure path computation; it does not create the parent
// directory or the socket file. The caller does that with
// EnsureSocketDir.
func DefaultSocketPath() (string, error) {
	if v := os.Getenv("MCPBRIDGE_SOCKET"); v != "" {
		return v, nil
	}

	switch runtime.GOOS {
	case "darwin":
		home, err := os.UserHomeDir()
		if err != nil {
			return "", fmt.Errorf("resolving $HOME: %w", err)
		}
		return filepath.Join(home, "Library", "Caches", "mcpbridge", "daemon.sock"), nil
	case "linux":
		if xdg := os.Getenv("XDG_RUNTIME_DIR"); xdg != "" {
			return filepath.Join(xdg, "mcpbridge", "daemon.sock"), nil
		}
		// Fall through to the TMPDIR fallback.
	}
	return tmpSocketPath()
}

func tmpSocketPath() (string, error) {
	tmp := os.Getenv("TMPDIR")
	if tmp == "" {
		tmp = "/tmp"
	}
	u, err := user.Current()
	if err != nil {
		return "", fmt.Errorf("resolving current user: %w", err)
	}
	dir := filepath.Join(tmp, fmt.Sprintf("mcpbridge-%s", u.Uid))
	return filepath.Join(dir, "daemon.sock"), nil
}

// EnsureSocketDir creates the parent directory of socketPath with
// mode 0700 so only the current user can access the socket. Returns
// the parent directory path.
func EnsureSocketDir(socketPath string) (string, error) {
	dir := filepath.Dir(socketPath)
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return "", fmt.Errorf("creating %s: %w", dir, err)
	}
	// MkdirAll does not chmod existing directories, so tighten
	// permissions explicitly. Ignore errors — the common case is
	// that the dir was already 0700 and chmod is a no-op.
	_ = os.Chmod(dir, 0o700)
	return dir, nil
}
