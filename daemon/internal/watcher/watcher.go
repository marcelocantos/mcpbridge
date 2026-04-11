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

// Watcher tracks a set of (name, absolutePath) pairs and broadcasts
// a reload to each corresponding name when its path changes.
type Watcher struct {
	fs        *fsnotify.Watcher
	bcast     Broadcaster
	coalesce  time.Duration

	mu     sync.Mutex
	byName map[string]string   // name -> absolute path
	byFile map[string][]string // absolute path -> names (same path can front multiple wrappers)
	dirs   map[string]int      // parent dir -> refcount (fsnotify watches dirs, not files)

	// pending coalesces rapid event bursts per name.
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
		byName:   make(map[string]string),
		byFile:   make(map[string][]string),
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
// watching the wrapper's child_binary path (if any).
func (w *Watcher) OnRegister(name, childBinary string) {
	if childBinary == "" {
		return
	}
	if err := w.Track(name, childBinary); err != nil {
		slog.Warn("watcher: track failed",
			"name", name, "path", childBinary, "err", err)
	}
}

// OnDeregister implements socket.RegistrationHandler. It stops
// watching the wrapper's child_binary path.
func (w *Watcher) OnDeregister(name string) {
	if err := w.Untrack(name); err != nil {
		slog.Warn("watcher: untrack failed", "name", name, "err", err)
	}
}

// Track starts watching the absolute path associated with a
// registered wrapper name. If the same name is tracked again with
// a different path, the previous path is replaced. Not thread-safe
// with Run's event loop on the same path — but fsnotify itself is.
func (w *Watcher) Track(name, path string) error {
	abs, err := filepath.Abs(path)
	if err != nil {
		return err
	}
	dir := filepath.Dir(abs)

	w.mu.Lock()
	defer w.mu.Unlock()

	// If name is already tracked at the same path, nothing to do.
	if existing, ok := w.byName[name]; ok && existing == abs {
		return nil
	}
	// Remove any prior mapping for this name.
	if existing, ok := w.byName[name]; ok {
		w.removeNameLocked(name, existing)
	}

	w.byName[name] = abs
	w.byFile[abs] = append(w.byFile[abs], name)

	// Reference-count dir watches so two binaries in the same
	// directory don't try to add the same watch twice.
	if w.dirs[dir] == 0 {
		if err := w.fs.Add(dir); err != nil {
			delete(w.byName, name)
			w.byFile[abs] = sliceRemove(w.byFile[abs], name)
			if len(w.byFile[abs]) == 0 {
				delete(w.byFile, abs)
			}
			return err
		}
	}
	w.dirs[dir]++
	return nil
}

// Untrack stops watching a previously tracked name. Idempotent.
func (w *Watcher) Untrack(name string) error {
	w.mu.Lock()
	defer w.mu.Unlock()

	path, ok := w.byName[name]
	if !ok {
		return nil
	}
	w.removeNameLocked(name, path)
	return nil
}

// removeNameLocked tears down the bookkeeping for one (name, path)
// pair. Caller holds w.mu.
func (w *Watcher) removeNameLocked(name, path string) {
	delete(w.byName, name)
	w.byFile[path] = sliceRemove(w.byFile[path], name)
	if len(w.byFile[path]) == 0 {
		delete(w.byFile, path)
	}
	dir := filepath.Dir(path)
	w.dirs[dir]--
	if w.dirs[dir] <= 0 {
		delete(w.dirs, dir)
		_ = w.fs.Remove(dir)
	}
	if t, ok := w.pending[name]; ok {
		t.Stop()
		delete(w.pending, name)
	}
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
	names := append([]string(nil), w.byFile[ev.Name]...)
	coalesce := w.coalesce
	w.mu.Unlock()
	if len(names) == 0 {
		return
	}
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

func sliceRemove(s []string, v string) []string {
	for i, x := range s {
		if x == v {
			return append(s[:i], s[i+1:]...)
		}
	}
	return s
}
