// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

package socket

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestDefaultSocketPath_EnvOverride(t *testing.T) {
	t.Setenv("MCPBRIDGE_SOCKET", "/tmp/custom.sock")
	got, err := DefaultSocketPath()
	if err != nil {
		t.Fatalf("err: %v", err)
	}
	if got != "/tmp/custom.sock" {
		t.Errorf("got %q, want /tmp/custom.sock", got)
	}
}

func TestDefaultSocketPath_PlatformDefault(t *testing.T) {
	t.Setenv("MCPBRIDGE_SOCKET", "")
	got, err := DefaultSocketPath()
	if err != nil {
		t.Fatalf("err: %v", err)
	}
	if !strings.HasSuffix(got, "/daemon.sock") {
		t.Errorf("path should end in daemon.sock, got %q", got)
	}
	// The path should always live somewhere under the current
	// user's control — either a Caches dir, a runtime dir, or a
	// per-user tmp subdir. Absolute path is the main invariant.
	if !strings.HasPrefix(got, "/") {
		t.Errorf("path should be absolute, got %q", got)
	}
}

func TestEnsureSocketDir_CreatesWith0700(t *testing.T) {
	tmp := t.TempDir()
	sockPath := filepath.Join(tmp, "nested", "dir", "daemon.sock")
	dir, err := EnsureSocketDir(sockPath)
	if err != nil {
		t.Fatalf("EnsureSocketDir: %v", err)
	}
	info, err := os.Stat(dir)
	if err != nil {
		t.Fatalf("stat: %v", err)
	}
	// Check permission bits — any group/other perms would be a
	// security regression.
	perm := info.Mode().Perm()
	if perm&0o077 != 0 {
		t.Errorf("socket dir has non-user-only perms: %v", perm)
	}
}
