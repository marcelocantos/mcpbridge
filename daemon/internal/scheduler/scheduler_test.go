// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

package scheduler

import (
	"context"
	"errors"
	"sync"
	"testing"
	"time"

	"github.com/marcelocantos/mcpbridge/daemon/internal/config"
	"github.com/marcelocantos/mcpbridge/daemon/internal/source"
)

// ---------- fakes ----------

type fakeBrew struct {
	mu          sync.Mutex
	outdated    *source.OutdatedInfo
	outdatedErr error
	upgradeErr  error

	outdatedCalls []string
	upgradeCalls  []string
}

func (f *fakeBrew) Outdated(_ context.Context, formula string) (*source.OutdatedInfo, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.outdatedCalls = append(f.outdatedCalls, formula)
	return f.outdated, f.outdatedErr
}

func (f *fakeBrew) Upgrade(_ context.Context, formula string) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.upgradeCalls = append(f.upgradeCalls, formula)
	return f.upgradeErr
}

type fakeGitHub struct {
	mu           sync.Mutex
	latestInfo   *source.LatestInfo
	latestErr    error
	installErr   error
	installCalls []installCall
	latestCalls  int
}

type installCall struct {
	cfg              *config.Config
	installedVersion string
	destPath         string
}

func (f *fakeGitHub) Latest(_ context.Context, _ string) (*source.LatestInfo, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.latestCalls++
	return f.latestInfo, f.latestErr
}

func (f *fakeGitHub) IsNewer(installed, latest string) bool {
	return installed != latest
}

func (f *fakeGitHub) Install(_ context.Context, cfg *config.Config, installedVersion, destPath string) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.installCalls = append(f.installCalls, installCall{
		cfg: cfg, installedVersion: installedVersion, destPath: destPath,
	})
	return f.installErr
}

type fakeBroadcaster struct {
	mu          sync.Mutex
	destByName  map[string]string
	reloadCalls []reloadCall
	reloadedCnt int
}

type reloadCall struct {
	name, reason string
}

func (f *fakeBroadcaster) ReloadName(name, reason string) int {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.reloadCalls = append(f.reloadCalls, reloadCall{name, reason})
	return f.reloadedCnt
}

func (f *fakeBroadcaster) ChildBinaryForName(name string) string {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.destByName[name]
}

// ---------- tests ----------

func mustCfg(t *testing.T, name string, src config.Source, mode config.UpgradeMode) *config.Config {
	t.Helper()
	return &config.Config{
		Name:          name,
		Source:        src,
		Upgrade:       mode,
		CheckInterval: time.Hour,
	}
}

func TestBrew_Auto_OutdatedTriggersUpgradeAndReload(t *testing.T) {
	cfg := mustCfg(t, "mnemo",
		config.Source{Type: config.SourceBrew, Formula: "marcelocantos/tap/mnemo"},
		config.UpgradeAuto)

	brew := &fakeBrew{
		outdated: &source.OutdatedInfo{
			Name:             "marcelocantos/tap/mnemo",
			InstalledVersion: "0.4.2",
			CurrentVersion:   "0.5.0",
		},
	}
	bcast := &fakeBroadcaster{reloadedCnt: 1}
	s := New(map[string]*config.Config{"mnemo": cfg}, brew, nil, bcast)

	s.PollNow(context.Background(), "mnemo")

	if len(brew.upgradeCalls) != 1 || brew.upgradeCalls[0] != "marcelocantos/tap/mnemo" {
		t.Errorf("upgrade calls: %v", brew.upgradeCalls)
	}
	if len(bcast.reloadCalls) != 1 || bcast.reloadCalls[0].name != "mnemo" {
		t.Errorf("reload calls: %+v", bcast.reloadCalls)
	}
	if bcast.reloadCalls[0].reason != "brew_upgrade" {
		t.Errorf("reason: %q", bcast.reloadCalls[0].reason)
	}
}

func TestBrew_Notify_OutdatedLogsButDoesNotUpgrade(t *testing.T) {
	cfg := mustCfg(t, "mnemo",
		config.Source{Type: config.SourceBrew, Formula: "marcelocantos/tap/mnemo"},
		config.UpgradeNotify)

	brew := &fakeBrew{
		outdated: &source.OutdatedInfo{CurrentVersion: "0.5.0"},
	}
	bcast := &fakeBroadcaster{}
	s := New(map[string]*config.Config{"mnemo": cfg}, brew, nil, bcast)

	s.PollNow(context.Background(), "mnemo")

	if len(brew.upgradeCalls) != 0 {
		t.Errorf("notify mode should not upgrade, got %v", brew.upgradeCalls)
	}
	if len(bcast.reloadCalls) != 0 {
		t.Errorf("notify mode should not reload, got %v", bcast.reloadCalls)
	}
}

func TestBrew_UpToDate_NoAction(t *testing.T) {
	cfg := mustCfg(t, "mnemo",
		config.Source{Type: config.SourceBrew, Formula: "f"},
		config.UpgradeAuto)
	brew := &fakeBrew{outdated: nil}
	bcast := &fakeBroadcaster{}
	s := New(map[string]*config.Config{"mnemo": cfg}, brew, nil, bcast)

	s.PollNow(context.Background(), "mnemo")
	if len(brew.upgradeCalls) != 0 {
		t.Errorf("up-to-date should not upgrade")
	}
	if len(bcast.reloadCalls) != 0 {
		t.Errorf("up-to-date should not reload")
	}
}

func TestBrew_ErrorSurfacesWithoutUpgrade(t *testing.T) {
	cfg := mustCfg(t, "mnemo",
		config.Source{Type: config.SourceBrew, Formula: "f"},
		config.UpgradeAuto)
	brew := &fakeBrew{outdatedErr: errors.New("network down")}
	bcast := &fakeBroadcaster{}
	s := New(map[string]*config.Config{"mnemo": cfg}, brew, nil, bcast)

	s.PollNow(context.Background(), "mnemo")
	if len(brew.upgradeCalls) != 0 || len(bcast.reloadCalls) != 0 {
		t.Errorf("error should short-circuit")
	}
}

func TestGitHub_FirstPollEstablishesBaselineNoInstall(t *testing.T) {
	cfg := mustCfg(t, "foo",
		config.Source{Type: config.SourceGitHub, Repo: "owner/foo"},
		config.UpgradeAuto)
	gh := &fakeGitHub{
		latestInfo: &source.LatestInfo{TagName: "v0.5.0"},
	}
	bcast := &fakeBroadcaster{
		destByName: map[string]string{"foo": "/usr/local/bin/foo"},
	}
	s := New(map[string]*config.Config{"foo": cfg}, nil, gh, bcast)

	s.PollNow(context.Background(), "foo")

	if len(gh.installCalls) != 0 {
		t.Errorf("first poll must not install: %v", gh.installCalls)
	}
	if len(bcast.reloadCalls) != 0 {
		t.Errorf("first poll must not broadcast: %v", bcast.reloadCalls)
	}
}

func TestGitHub_SecondPollWithNewTagInstallsAndBroadcasts(t *testing.T) {
	cfg := mustCfg(t, "foo",
		config.Source{Type: config.SourceGitHub, Repo: "owner/foo"},
		config.UpgradeAuto)
	gh := &fakeGitHub{
		latestInfo: &source.LatestInfo{TagName: "v0.5.0"},
	}
	bcast := &fakeBroadcaster{
		destByName:  map[string]string{"foo": "/usr/local/bin/foo"},
		reloadedCnt: 1,
	}
	s := New(map[string]*config.Config{"foo": cfg}, nil, gh, bcast)

	// First poll establishes the baseline.
	s.PollNow(context.Background(), "foo")
	// Upstream releases a new tag.
	gh.latestInfo = &source.LatestInfo{TagName: "v0.5.1"}
	s.PollNow(context.Background(), "foo")

	if len(gh.installCalls) != 1 {
		t.Fatalf("install calls: %d", len(gh.installCalls))
	}
	call := gh.installCalls[0]
	if call.installedVersion != "v0.5.0" {
		t.Errorf("installed version: %q", call.installedVersion)
	}
	if call.destPath != "/usr/local/bin/foo" {
		t.Errorf("dest path: %q", call.destPath)
	}
	if len(bcast.reloadCalls) != 1 ||
		bcast.reloadCalls[0].reason != "github_release" {
		t.Errorf("reload calls: %+v", bcast.reloadCalls)
	}
}

func TestGitHub_Notify_DoesNotInstall(t *testing.T) {
	cfg := mustCfg(t, "foo",
		config.Source{Type: config.SourceGitHub, Repo: "owner/foo"},
		config.UpgradeNotify)
	gh := &fakeGitHub{
		latestInfo: &source.LatestInfo{TagName: "v0.5.0"},
	}
	bcast := &fakeBroadcaster{}
	s := New(map[string]*config.Config{"foo": cfg}, nil, gh, bcast)

	s.PollNow(context.Background(), "foo")
	gh.latestInfo = &source.LatestInfo{TagName: "v0.5.1"}
	s.PollNow(context.Background(), "foo")

	if len(gh.installCalls) != 0 {
		t.Errorf("notify mode should not install")
	}
	if len(bcast.reloadCalls) != 0 {
		t.Errorf("notify mode should not broadcast")
	}
}

func TestGitHub_SecondPollDeferredWhenNoWrapperRegistered(t *testing.T) {
	cfg := mustCfg(t, "foo",
		config.Source{Type: config.SourceGitHub, Repo: "owner/foo"},
		config.UpgradeAuto)
	gh := &fakeGitHub{
		latestInfo: &source.LatestInfo{TagName: "v0.5.0"},
	}
	// destByName empty -> ChildBinaryForName returns ""
	bcast := &fakeBroadcaster{destByName: map[string]string{}}
	s := New(map[string]*config.Config{"foo": cfg}, nil, gh, bcast)

	s.PollNow(context.Background(), "foo")
	gh.latestInfo = &source.LatestInfo{TagName: "v0.5.1"}
	s.PollNow(context.Background(), "foo")

	if len(gh.installCalls) != 0 {
		t.Errorf("install should be deferred when no wrapper is registered")
	}
	if len(bcast.reloadCalls) != 0 {
		t.Errorf("no reload when install was deferred")
	}
}

func TestRun_SkipsOffMode(t *testing.T) {
	cfg := mustCfg(t, "foo",
		config.Source{Type: config.SourceBrew, Formula: "f"},
		config.UpgradeOff)
	brew := &fakeBrew{
		outdated: &source.OutdatedInfo{CurrentVersion: "99"},
	}
	bcast := &fakeBroadcaster{}
	s := New(map[string]*config.Config{"foo": cfg}, brew, nil, bcast)

	ctx, cancel := context.WithTimeout(context.Background(), 200*time.Millisecond)
	defer cancel()
	s.Run(ctx)

	if len(brew.outdatedCalls) != 0 {
		t.Errorf("off mode should not poll: %v", brew.outdatedCalls)
	}
}

func TestRun_StopsOnContextCancel(t *testing.T) {
	cfg := mustCfg(t, "foo",
		config.Source{Type: config.SourceBrew, Formula: "f"},
		config.UpgradeAuto)
	cfg.CheckInterval = time.Hour // avoid second tick racing the cancel
	brew := &fakeBrew{outdated: nil}
	bcast := &fakeBroadcaster{}
	s := New(map[string]*config.Config{"foo": cfg}, brew, nil, bcast)

	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() {
		s.Run(ctx)
		close(done)
	}()
	// Give the first pollOnce time to happen.
	time.Sleep(50 * time.Millisecond)
	cancel()
	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatal("Run did not return after cancel")
	}
	if len(brew.outdatedCalls) == 0 {
		t.Errorf("expected at least one outdated call before cancel")
	}
}
