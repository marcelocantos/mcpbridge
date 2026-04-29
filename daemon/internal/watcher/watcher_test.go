// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

package watcher

import (
	"context"
	"os"
	"path/filepath"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

type fakeBcast struct {
	mu         sync.Mutex
	count      atomic.Int64
	lastName   string
	lastReason string
}

func (f *fakeBcast) ReloadName(name, reason string) int {
	f.count.Add(1)
	f.mu.Lock()
	f.lastName = name
	f.lastReason = reason
	f.mu.Unlock()
	return 1
}

func shortTempDir(t *testing.T) string {
	t.Helper()
	dir, err := os.MkdirTemp("/tmp", "mcpb-wtch-*")
	if err != nil {
		t.Fatalf("MkdirTemp: %v", err)
	}
	t.Cleanup(func() { _ = os.RemoveAll(dir) })
	return dir
}

func startWatcher(t *testing.T, bc Broadcaster) (*Watcher, context.CancelFunc) {
	t.Helper()
	w, err := New(bc)
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	w.SetCoalesceWindow(50 * time.Millisecond) // fast tests
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() {
		w.Run(ctx)
		close(done)
	}()
	t.Cleanup(func() {
		cancel()
		w.Close()
		<-done
	})
	return w, cancel
}

func TestWatcher_DetectsWriteAndFiresReload(t *testing.T) {
	dir := shortTempDir(t)
	binPath := filepath.Join(dir, "mnemo")
	if err := os.WriteFile(binPath, []byte("v1"), 0o755); err != nil {
		t.Fatal(err)
	}

	bc := &fakeBcast{}
	w, _ := startWatcher(t, bc)

	if err := w.Track("mnemo", binPath); err != nil {
		t.Fatalf("Track: %v", err)
	}

	// Simulate an out-of-band upgrade: overwrite the file.
	if err := os.WriteFile(binPath, []byte("v2 (brew upgrade)"), 0o755); err != nil {
		t.Fatal(err)
	}

	// Wait up to 1s for the debounced reload to fire.
	deadline := time.Now().Add(1 * time.Second)
	for time.Now().Before(deadline) {
		if bc.count.Load() > 0 {
			break
		}
		time.Sleep(20 * time.Millisecond)
	}
	if bc.count.Load() == 0 {
		t.Fatal("ReloadName never called")
	}
	bc.mu.Lock()
	defer bc.mu.Unlock()
	if bc.lastName != "mnemo" {
		t.Errorf("name: %q", bc.lastName)
	}
	if bc.lastReason != "binary_changed" {
		t.Errorf("reason: %q", bc.lastReason)
	}
}

func TestWatcher_IgnoresUnrelatedFiles(t *testing.T) {
	dir := shortTempDir(t)
	binPath := filepath.Join(dir, "mnemo")
	otherPath := filepath.Join(dir, "README")
	os.WriteFile(binPath, []byte("v1"), 0o755)
	os.WriteFile(otherPath, []byte("docs"), 0o644)

	bc := &fakeBcast{}
	w, _ := startWatcher(t, bc)
	if err := w.Track("mnemo", binPath); err != nil {
		t.Fatal(err)
	}

	// Touch the unrelated file.
	os.WriteFile(otherPath, []byte("updated docs"), 0o644)

	// Wait a bit and confirm no broadcast fired.
	time.Sleep(300 * time.Millisecond)
	if bc.count.Load() != 0 {
		t.Errorf("unrelated file should not trigger reload (got %d)", bc.count.Load())
	}
}

func TestWatcher_CoalescesRapidEvents(t *testing.T) {
	dir := shortTempDir(t)
	binPath := filepath.Join(dir, "foo")
	os.WriteFile(binPath, []byte("v1"), 0o755)

	bc := &fakeBcast{}
	w, _ := startWatcher(t, bc)
	w.Track("foo", binPath)

	// Three rapid writes should coalesce into one reload.
	os.WriteFile(binPath, []byte("v2"), 0o755)
	os.Chmod(binPath, 0o755)
	os.WriteFile(binPath, []byte("v3"), 0o755)

	deadline := time.Now().Add(1 * time.Second)
	for time.Now().Before(deadline) {
		if bc.count.Load() > 0 {
			break
		}
		time.Sleep(20 * time.Millisecond)
	}
	// Give the coalescing window a chance to produce any extra
	// calls (there shouldn't be any — just confirming).
	time.Sleep(200 * time.Millisecond)
	if got := bc.count.Load(); got != 1 {
		t.Errorf("want 1 reload after coalescing, got %d", got)
	}
}

func TestWatcher_UntrackStopsFiring(t *testing.T) {
	dir := shortTempDir(t)
	binPath := filepath.Join(dir, "foo")
	os.WriteFile(binPath, []byte("v1"), 0o755)

	bc := &fakeBcast{}
	w, _ := startWatcher(t, bc)
	w.Track("foo", binPath)
	w.Untrack("foo")

	os.WriteFile(binPath, []byte("v2"), 0o755)
	time.Sleep(300 * time.Millisecond)
	if bc.count.Load() != 0 {
		t.Errorf("untracked name should not fire: got %d", bc.count.Load())
	}
}

func TestWatcher_OnRegisterSkipsURLBackend(t *testing.T) {
	// HTTP-backend wrappers (schema-v2 `url`, no `command`) report
	// their upstream URL as child_binary. The watcher must skip the
	// filesystem-watch step rather than lstat'ing the URL string.
	bc := &fakeBcast{}
	w, _ := startWatcher(t, bc)

	for _, url := range []string{
		"http://localhost:3030/mcp",
		"https://example.com/mcp",
	} {
		w.OnRegister("urlbackend", url)
		w.mu.Lock()
		_, tracked := w.byName["urlbackend"]
		w.mu.Unlock()
		if tracked {
			t.Errorf("OnRegister(%q) should not have tracked a URL", url)
		}
	}
}

func TestWatcher_OnRegisterAndOnDeregisterBridge(t *testing.T) {
	// Verifies the RegistrationHandler surface: OnRegister starts
	// tracking, OnDeregister stops it.
	dir := shortTempDir(t)
	binPath := filepath.Join(dir, "foo")
	os.WriteFile(binPath, []byte("v1"), 0o755)

	bc := &fakeBcast{}
	w, _ := startWatcher(t, bc)

	w.OnRegister("foo", binPath)
	os.WriteFile(binPath, []byte("v2"), 0o755)

	deadline := time.Now().Add(1 * time.Second)
	for time.Now().Before(deadline) {
		if bc.count.Load() > 0 {
			break
		}
		time.Sleep(20 * time.Millisecond)
	}
	if bc.count.Load() == 0 {
		t.Fatal("OnRegister should have enabled tracking")
	}

	w.OnDeregister("foo")
	prev := bc.count.Load()

	os.WriteFile(binPath, []byte("v3"), 0o755)
	time.Sleep(300 * time.Millisecond)
	if bc.count.Load() != prev {
		t.Errorf("OnDeregister should have stopped tracking: %d -> %d",
			prev, bc.count.Load())
	}
}
