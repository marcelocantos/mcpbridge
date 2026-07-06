// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

package socket

import (
	"net"
	"path/filepath"
	"testing"
)

// TestNewServer_RefusesSecondDaemon pins the "exactly one daemon per
// socket path" invariant (Fable-5 F1 / T13). A second NewServer bound
// to the same path while the first is live must FAIL rather than
// unlink the first's live socket and silently steal the endpoint.
func TestNewServer_RefusesSecondDaemon(t *testing.T) {
	dir := shortTempDir(t)
	sock := filepath.Join(dir, "d.sock")

	s1, err := NewServer(sock, nil)
	if err != nil {
		t.Fatalf("first NewServer: %v", err)
	}
	defer s1.Close()

	// The first daemon's socket must be a live, connectable endpoint.
	if c, err := net.Dial("unix", sock); err != nil {
		t.Fatalf("first daemon socket not connectable: %v", err)
	} else {
		_ = c.Close()
	}

	// A second daemon on the same path, while the first is live, must
	// be refused. Before the fix, NewServer os.Remove()s the live
	// socket and net.Listen succeeds — the singleton invariant is
	// violated and this returns a nil error.
	s2, err := NewServer(sock, nil)
	if err == nil {
		_ = s2.Close()
		t.Fatal("second NewServer succeeded while first daemon was live; " +
			"singleton invariant violated (a second daemon hijacked the socket)")
	}

	// The first daemon must still own a live, connectable socket after
	// the rejected second start.
	if c, err := net.Dial("unix", sock); err != nil {
		t.Fatalf("first daemon socket died after second start was rejected: %v", err)
	} else {
		_ = c.Close()
	}
}

// TestNewServer_ReclaimsAfterClose verifies the lock is released on
// Close so a genuine restart (or recovery after an unclean exit) can
// reclaim the socket path — the stale-socket-recovery half of the
// invariant.
func TestNewServer_ReclaimsAfterClose(t *testing.T) {
	dir := shortTempDir(t)
	sock := filepath.Join(dir, "d.sock")

	s1, err := NewServer(sock, nil)
	if err != nil {
		t.Fatalf("first NewServer: %v", err)
	}
	if err := s1.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}

	s2, err := NewServer(sock, nil)
	if err != nil {
		t.Fatalf("second NewServer after first Close should succeed, got: %v", err)
	}
	_ = s2.Close()
}
