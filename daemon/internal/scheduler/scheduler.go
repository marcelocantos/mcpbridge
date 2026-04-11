// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Package scheduler owns the polling loop and fswatch integration
// that ties the daemon together: per-config timers call the right
// source backend, detect new versions, optionally install them, and
// push a targeted reload at the wrappers that care.
package scheduler

import (
	"context"
	"log/slog"
	"sync"
	"time"

	"github.com/marcelocantos/mcpbridge/daemon/internal/config"
	"github.com/marcelocantos/mcpbridge/daemon/internal/source"
)

// BrewSource is the slice of the brew backend the scheduler needs.
// Defined as an interface so tests can inject stubs.
type BrewSource interface {
	Outdated(ctx context.Context, formula string) (*source.OutdatedInfo, error)
	Upgrade(ctx context.Context, formula string) error
}

// GitHubSource is the slice of the github backend the scheduler
// needs.
type GitHubSource interface {
	Latest(ctx context.Context, repo string) (*source.LatestInfo, error)
	IsNewer(installed, latest string) bool
	Install(ctx context.Context, cfg *config.Config, installedVersion, destPath string) error
}

// Broadcaster is the slice of socket.Server the scheduler uses to
// notify wrappers of completed upgrades.
type Broadcaster interface {
	// ReloadName sends a reload to wrappers registered with this
	// name. Returns the number notified — the scheduler logs at
	// info when this is zero so a silent misconfiguration doesn't
	// go unnoticed.
	ReloadName(name, reason string) int

	// ChildBinaryForName returns the install path the wrapper
	// reported at register time, or "" if no wrapper is currently
	// connected for that name.
	ChildBinaryForName(name string) string
}

// Scheduler owns one goroutine per config and kicks each one on its
// configured check_interval.
type Scheduler struct {
	configs map[string]*config.Config
	brew    BrewSource
	github  GitHubSource
	bcast   Broadcaster

	// Per-config state: last-seen version from the source backend.
	// For github this is the cached tag used to detect change; for
	// brew it is the current_version from the most recent Outdated
	// call (diagnostic only — brew tells us authoritatively whether
	// there is something to do).
	mu       sync.Mutex
	lastSeen map[string]string
}

// New constructs a scheduler bound to a config map, a pair of
// source backends, and a broadcaster. Any of brew/github/bcast may
// be nil for targeted tests; the corresponding code paths short-
// circuit with an info log when they hit a nil dependency.
func New(cfgs map[string]*config.Config,
	brew BrewSource,
	github GitHubSource,
	bcast Broadcaster) *Scheduler {

	return &Scheduler{
		configs:  cfgs,
		brew:     brew,
		github:   github,
		bcast:    bcast,
		lastSeen: make(map[string]string),
	}
}

// Run starts one goroutine per config and blocks until ctx is
// cancelled. The poll loops call pollOnce immediately on entry so
// the daemon has a baseline before the first tick interval elapses.
func (s *Scheduler) Run(ctx context.Context) {
	if len(s.configs) == 0 {
		slog.Info("scheduler: no configs; nothing to poll")
		<-ctx.Done()
		return
	}
	var wg sync.WaitGroup
	for _, cfg := range s.configs {
		// Skip explicitly disabled configs — they exist to declare
		// the name but don't want any polling.
		if cfg.Upgrade == config.UpgradeOff {
			slog.Info("scheduler: upgrade=off; skipping", "name", cfg.Name)
			continue
		}
		wg.Add(1)
		go func(c *config.Config) {
			defer wg.Done()
			s.pollLoop(ctx, c)
		}(cfg)
	}
	wg.Wait()
}

// PollNow runs a single poll for the named config on the calling
// goroutine. Used by tests to exercise the check path without real
// time; not used in production (the poll loop covers that).
func (s *Scheduler) PollNow(ctx context.Context, name string) {
	cfg, ok := s.configs[name]
	if !ok {
		return
	}
	s.pollOnce(ctx, cfg)
}

func (s *Scheduler) pollLoop(ctx context.Context, cfg *config.Config) {
	slog.Info("scheduler: polling",
		"name", cfg.Name,
		"source", cfg.Source.Type,
		"interval", cfg.CheckInterval,
		"mode", cfg.Upgrade)

	s.pollOnce(ctx, cfg)

	t := time.NewTicker(cfg.CheckInterval)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			s.pollOnce(ctx, cfg)
		}
	}
}

func (s *Scheduler) pollOnce(ctx context.Context, cfg *config.Config) {
	switch cfg.Source.Type {
	case config.SourceBrew:
		s.pollBrew(ctx, cfg)
	case config.SourceGitHub:
		s.pollGitHub(ctx, cfg)
	default:
		slog.Warn("scheduler: unknown source type",
			"name", cfg.Name, "type", cfg.Source.Type)
	}
}

// ---------- brew ----------

func (s *Scheduler) pollBrew(ctx context.Context, cfg *config.Config) {
	if s.brew == nil {
		slog.Debug("scheduler: brew backend absent; skipping", "name", cfg.Name)
		return
	}
	info, err := s.brew.Outdated(ctx, cfg.Source.Formula)
	if err != nil {
		slog.Warn("scheduler: brew outdated failed",
			"name", cfg.Name, "err", err)
		return
	}
	if info == nil {
		// Up-to-date; nothing to do.
		return
	}
	slog.Info("scheduler: brew update available",
		"name", cfg.Name,
		"installed", info.InstalledVersion,
		"current", info.CurrentVersion,
		"mode", cfg.Upgrade)

	s.mu.Lock()
	s.lastSeen[cfg.Name] = info.CurrentVersion
	s.mu.Unlock()

	if cfg.Upgrade != config.UpgradeAuto {
		// notify mode: log and stop. The wrapper will cycle its
		// child when the user runs `brew upgrade` themselves
		// (which fswatch catches) or on client restart.
		return
	}
	if err := s.brew.Upgrade(ctx, cfg.Source.Formula); err != nil {
		slog.Warn("scheduler: brew upgrade failed",
			"name", cfg.Name, "err", err)
		return
	}
	s.notifyReload(cfg.Name, "brew_upgrade")
}

// ---------- github ----------

func (s *Scheduler) pollGitHub(ctx context.Context, cfg *config.Config) {
	if s.github == nil {
		slog.Debug("scheduler: github backend absent; skipping", "name", cfg.Name)
		return
	}
	latest, err := s.github.Latest(ctx, cfg.Source.Repo)
	if err != nil {
		slog.Warn("scheduler: github latest failed",
			"name", cfg.Name, "err", err)
		return
	}

	s.mu.Lock()
	prev := s.lastSeen[cfg.Name]
	s.lastSeen[cfg.Name] = latest.TagName
	s.mu.Unlock()

	if prev == "" {
		// First observation — record and wait. We can't tell
		// whether the wrapped binary is ahead of, at, or behind
		// this release without consulting the binary itself, so
		// we opt for "no action on the first tick" which is safer
		// than an unnecessary install.
		slog.Info("scheduler: github baseline recorded",
			"name", cfg.Name, "tag", latest.TagName)
		return
	}
	if !s.github.IsNewer(prev, latest.TagName) {
		return
	}
	slog.Info("scheduler: github update available",
		"name", cfg.Name,
		"previous", prev,
		"latest", latest.TagName,
		"mode", cfg.Upgrade)

	if cfg.Upgrade != config.UpgradeAuto {
		return
	}

	// Auto install. We need a destination path — the wrapper told
	// the daemon at register time. If no wrapper is currently
	// registered, the install has no target.
	destPath := ""
	if s.bcast != nil {
		destPath = s.bcast.ChildBinaryForName(cfg.Name)
	}
	if destPath == "" {
		slog.Info("scheduler: github upgrade deferred; no wrapper registered",
			"name", cfg.Name)
		return
	}
	if err := s.github.Install(ctx, cfg, prev, destPath); err != nil {
		slog.Warn("scheduler: github install failed",
			"name", cfg.Name, "err", err)
		return
	}
	s.notifyReload(cfg.Name, "github_release")
}

// ---------- shared ----------

func (s *Scheduler) notifyReload(name, reason string) {
	if s.bcast == nil {
		return
	}
	n := s.bcast.ReloadName(name, reason)
	if n == 0 {
		slog.Info("scheduler: reload notified but no wrappers",
			"name", name, "reason", reason)
	} else {
		slog.Info("scheduler: reload broadcast",
			"name", name, "reason", reason, "wrappers", n)
	}
}
