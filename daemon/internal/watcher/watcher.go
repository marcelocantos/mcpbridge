// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Package watcher wraps fsnotify to detect out-of-band changes to
// a wrapped MCP server's binary. This is the "user ran brew upgrade
// themselves" case: the scheduler didn't drive the upgrade, so
// nothing in the daemon's own state would otherwise notice. The
// watcher sees the file change event and pushes a targeted reload
// at the relevant wrapper.
package watcher

import (
	"context"
	"errors"
	"log/slog"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"github.com/fsnotify/fsnotify"
)

// Broadcaster is the slice of socket.Server the watcher uses. Same
// shape as the scheduler's Broadcaster; kept as its own interface
// so the packages don't import each other.
type Broadcaster interface {
	ReloadName(name, reason string) int
}

// DefaultCoalesceWindow is how long the watcher sits on rapid
// events for the same name before firing a single reload. A brew
// upgrade typically produces a burst of CREATE / CHMOD / RENAME
// events in tens of milliseconds; coalescing avoids multiple
// back-to-back reloads.
const DefaultCoalesceWindow = 300 * time.Millisecond

// watchEntry records what one connection registered. Bookkeeping is
// keyed by connection id (not by name) so two wrappers sharing a name
// each hold an independent watch reference.
type watchEntry struct {
	name string // config name this connection registered under
	abs  string // absolute path being watched
	dir  string // parent dir (what fsnotify actually watches)
}

// Watcher tracks a set of per-connection (name, absolutePath)
// registrations and broadcasts a reload to the matching name(s) when a
// watched path changes.
type Watcher struct {
	fs       *fsnotify.Watcher
	bcast    Broadcaster
	coalesce time.Duration

	mu     sync.Mutex
	byConn map[uint64]watchEntry // connection id -> what it registered
	dirs   map[string]int        // parent dir -> refcount (one per live registration)

	// pending coalesces rapid event bursts per name (reload is
	// name-targeted, so the debounce timer stays name-keyed).
	pending map[string]*time.Timer
}

// New creates a watcher. bcast may be nil for tests that only want
// to exercise event detection; the reload broadcast is then a
// no-op.
func New(bcast Broadcaster) (*Watcher, error) {
	fs, err := fsnotify.NewWatcher()
	if err != nil {
		return nil, err
	}
	return &Watcher{
		fs:       fs,
		bcast:    bcast,
		coalesce: DefaultCoalesceWindow,
		byConn:   make(map[uint64]watchEntry),
		dirs:     make(map[string]int),
		pending:  make(map[string]*time.Timer),
	}, nil
}

// SetCoalesceWindow overrides the default coalescing delay. Tests
// call this with a shorter value to keep runtimes short.
func (w *Watcher) SetCoalesceWindow(d time.Duration) {
	w.mu.Lock()
	w.coalesce = d
	w.mu.Unlock()
}

// Run consumes fsnotify events until ctx is cancelled OR the
// watcher is closed. Never returns an error on clean shutdown.
func (w *Watcher) Run(ctx context.Context) {
	go func() {
		<-ctx.Done()
		_ = w.fs.Close()
	}()
	for {
		select {
		case ev, ok := <-w.fs.Events:
			if !ok {
				return
			}
			w.handleEvent(ev)
		case err, ok := <-w.fs.Errors:
			if !ok {
				return
			}
			if errors.Is(err, fsnotify.ErrEventOverflow) {
				slog.Warn("watcher: fsnotify overflow")
				continue
			}
			slog.Warn("watcher: fsnotify error", "err", err)
		}
	}
}

// Close tears down the underlying fsnotify watcher and drops all
// tracked entries. Safe to call multiple times; safe to call
// concurrently with Run (Run will observe the channel close and
// return).
func (w *Watcher) Close() error {
	w.mu.Lock()
	for name, t := range w.pending {
		t.Stop()
		delete(w.pending, name)
	}
	w.mu.Unlock()
	return w.fs.Close()
}

// OnRegister implements socket.RegistrationHandler. It starts
// watching the wrapper's child_binary path (if any). HTTP-backend
// wrappers report their upstream URL here; there is nothing on the
// local filesystem to watch, so URL-shaped values are skipped
// silently.
func (w *Watcher) OnRegister(connID uint64, name, childBinary string) {
	if childBinary == "" {
		return
	}
	if isURL(childBinary) {
		slog.Debug("watcher: skipping URL backend",
			"name", name, "url", childBinary)
		return
	}
	if err := w.Track(connID, name, childBinary); err != nil {
		slog.Warn("watcher: track failed",
			"name", name, "path", childBinary, "err", err)
	}
}

// isURL reports whether s is an http(s) URL rather than a filesystem
// path. The watcher uses this to skip tracking HTTP-backend wrappers,
// whose register envelope reports the upstream URL in child_binary.
func isURL(s string) bool {
	return strings.HasPrefix(s, "http://") || strings.HasPrefix(s, "https://")
}

// OnDeregister implements socket.RegistrationHandler. It stops
// watching the path the given connection registered.
func (w *Watcher) OnDeregister(connID uint64, name string) {
	if err := w.Untrack(connID); err != nil {
		slog.Warn("watcher: untrack failed", "name", name, "err", err)
	}
}

// Track starts watching the absolute path a registered connection
// reported. Bookkeeping is keyed by connection id, so two wrappers
// sharing a name each hold an independent watch reference and
// deregistering one leaves the others' watches intact. A repeated
// (connID, path) is a no-op; the same connection re-registering a
// different path replaces its own prior reference.
func (w *Watcher) Track(connID uint64, name, path string) error {
	abs, err := filepath.Abs(path)
	if err != nil {
		return err
	}
	dir := filepath.Dir(abs)

	w.mu.Lock()
	defer w.mu.Unlock()

	if e, ok := w.byConn[connID]; ok {
		if e.abs == abs && e.name == name {
			return nil
		}
		// This connection re-registered under a different name/path:
		// drop its old reference before taking the new one.
		w.removeConnLocked(connID)
	}

	// Reference-count dir watches so multiple registrations in the
	// same directory add the fsnotify watch exactly once.
	if w.dirs[dir] == 0 {
		if err := w.fs.Add(dir); err != nil {
			return err
		}
	}
	w.dirs[dir]++
	w.byConn[connID] = watchEntry{name: name, abs: abs, dir: dir}
	return nil
}

// Untrack stops watching the path a connection registered. Idempotent.
func (w *Watcher) Untrack(connID uint64) error {
	w.mu.Lock()
	defer w.mu.Unlock()
	w.removeConnLocked(connID)
	return nil
}

// removeConnLocked tears down one connection's watch reference. The
// fsnotify dir watch is dropped only when the LAST registration in
// that directory is gone, so a same-directory (or same-name) peer
// keeps its watch. Caller holds w.mu.
func (w *Watcher) removeConnLocked(connID uint64) {
	e, ok := w.byConn[connID]
	if !ok {
		return
	}
	delete(w.byConn, connID)

	w.dirs[e.dir]--
	if w.dirs[e.dir] <= 0 {
		delete(w.dirs, e.dir)
		_ = w.fs.Remove(e.dir)
	}

	// Cancel any pending coalesced reload for this name only if no
	// other connection is still registered under it.
	if !w.nameTrackedLocked(e.name) {
		if t, ok := w.pending[e.name]; ok {
			t.Stop()
			delete(w.pending, e.name)
		}
	}
}

// nameTrackedLocked reports whether any connection is still registered
// under name. Caller holds w.mu.
func (w *Watcher) nameTrackedLocked(name string) bool {
	for _, e := range w.byConn {
		if e.name == name {
			return true
		}
	}
	return false
}

// handleEvent is called for every fsnotify event on any directory
// we're watching. It ignores events that don't match a tracked
// path and schedules a debounced reload for matches.
func (w *Watcher) handleEvent(ev fsnotify.Event) {
	// Event names are absolute or dir-relative depending on
	// platform quirks. fsnotify gives us the full path via Name
	// in practice (matches the path we Add'd).
	if ev.Op&(fsnotify.Write|fsnotify.Create|fsnotify.Rename|fsnotify.Chmod) == 0 {
		return
	}
	w.mu.Lock()
	var names []string
	seen := make(map[string]bool)
	for _, e := range w.byConn {
		if e.abs == ev.Name && !seen[e.name] {
			seen[e.name] = true
			names = append(names, e.name)
		}
	}
	coalesce := w.coalesce
	w.mu.Unlock()
	for _, name := range names {
		w.scheduleReload(name, coalesce)
	}
}

func (w *Watcher) scheduleReload(name string, coalesce time.Duration) {
	w.mu.Lock()
	if t, ok := w.pending[name]; ok {
		t.Stop()
	}
	t := time.AfterFunc(coalesce, func() {
		w.mu.Lock()
		delete(w.pending, name)
		w.mu.Unlock()
		slog.Info("watcher: binary changed; broadcasting reload", "name", name)
		if w.bcast != nil {
			w.bcast.ReloadName(name, "binary_changed")
		}
	})
	w.pending[name] = t
	w.mu.Unlock()
}
