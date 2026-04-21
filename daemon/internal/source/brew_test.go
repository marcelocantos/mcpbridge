// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

package source

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// fakeBrew captures what was invoked so the test can assert the
// arg list, and returns canned stdout/stderr/err.
type fakeBrew struct {
	args   []string
	stdout []byte
	stderr []byte
	err    error
}

func (f *fakeBrew) runCapture(_ context.Context, name string, args ...string) ([]byte, []byte, error) {
	f.args = append([]string{name}, args...)
	return f.stdout, f.stderr, f.err
}

func (f *fakeBrew) run(_ context.Context, name string, args ...string) error {
	f.args = append([]string{name}, args...)
	return f.err
}

func newFakeBrew() (*Brew, *fakeBrew) {
	f := &fakeBrew{}
	b := &Brew{
		runCapture: f.runCapture,
		run:        f.run,
		// Return the bare string "brew" so the existing tests can
		// assert args[0] == "brew". The real resolver (exercised
		// separately in TestResolveBrewExe) returns absolute paths.
		exe: func() (string, error) { return "brew", nil },
	}
	return b, f
}

func TestOutdated_ReportsFormulaWhenPresent(t *testing.T) {
	b, fb := newFakeBrew()
	fb.stdout = []byte(`{
		"formulae": [
			{
				"name": "marcelocantos/tap/mnemo",
				"installed_versions": ["0.4.2"],
				"current_version": "0.5.0",
				"pinned": false
			}
		],
		"casks": []
	}`)

	info, err := b.Outdated(context.Background(), "marcelocantos/tap/mnemo")
	if err != nil {
		t.Fatalf("Outdated: %v", err)
	}
	if info == nil {
		t.Fatal("expected non-nil info")
	}
	if info.Name != "marcelocantos/tap/mnemo" {
		t.Errorf("name: got %q", info.Name)
	}
	if info.InstalledVersion != "0.4.2" {
		t.Errorf("installed: got %q", info.InstalledVersion)
	}
	if info.CurrentVersion != "0.5.0" {
		t.Errorf("current: got %q", info.CurrentVersion)
	}

	// Arg list sanity.
	wantSubstr := []string{"brew", "outdated", "--json=v2", "--formula"}
	for _, w := range wantSubstr {
		found := false
		for _, a := range fb.args {
			if a == w {
				found = true
				break
			}
		}
		if !found {
			t.Errorf("arg %q missing from %v", w, fb.args)
		}
	}
}

func TestOutdated_EmptyWhenFormulaCurrent(t *testing.T) {
	// brew returns an empty formulae list when the formula is
	// already at the current version OR when it isn't installed.
	// Both map to (nil, nil) in our contract.
	b, fb := newFakeBrew()
	fb.stdout = []byte(`{"formulae": [], "casks": []}`)

	info, err := b.Outdated(context.Background(), "foo")
	if err != nil {
		t.Fatalf("err: %v", err)
	}
	if info != nil {
		t.Errorf("want nil, got %+v", info)
	}
}

func TestOutdated_IgnoresUnrelatedFormulae(t *testing.T) {
	// If brew reports other formulae but not ours, we treat it as
	// nothing-to-do. (Probably shouldn't happen when we pass
	// --formula <name>, but be defensive.)
	b, fb := newFakeBrew()
	fb.stdout = []byte(`{
		"formulae": [{"name": "someone-else", "current_version": "9.9.9"}],
		"casks": []
	}`)

	info, err := b.Outdated(context.Background(), "foo")
	if err != nil {
		t.Fatal(err)
	}
	if info != nil {
		t.Errorf("want nil, got %+v", info)
	}
}

func TestOutdated_RejectsMalformedJSON(t *testing.T) {
	b, fb := newFakeBrew()
	fb.stdout = []byte(`{not json`)
	if _, err := b.Outdated(context.Background(), "foo"); err == nil {
		t.Fatal("expected parse error")
	}
}

func TestOutdated_SurfacesBrewError(t *testing.T) {
	b, fb := newFakeBrew()
	fb.err = errors.New("exit status 1")
	fb.stderr = []byte("brew: formula not found: xyz\n")
	_, err := b.Outdated(context.Background(), "xyz")
	if err == nil {
		t.Fatal("expected error")
	}
	if !strings.Contains(err.Error(), "formula not found") {
		t.Errorf("stderr should surface: got %v", err)
	}
}

func TestOutdated_RejectsEmptyFormula(t *testing.T) {
	b, _ := newFakeBrew()
	if _, err := b.Outdated(context.Background(), ""); err == nil {
		t.Fatal("expected error on empty formula")
	}
}

// TestOutdated_ParsesJSONOnExitOne: `brew outdated --formula FOO`
// exits with status 1 when FOO is outdated, and still prints the
// structured JSON on stdout. Prior to the fix we treated the exit
// code as a hard failure and ignored the JSON — so the daemon
// silently missed every real upgrade signal.
func TestOutdated_ParsesJSONOnExitOne(t *testing.T) {
	b, fb := newFakeBrew()
	fb.stdout = []byte(`{
		"formulae": [
			{
				"name": "marcelocantos/tap/mnemo",
				"installed_versions": ["0.4.2"],
				"current_version": "0.5.0",
				"pinned": false
			}
		],
		"casks": []
	}`)
	fb.err = errors.New("exit status 1")

	info, err := b.Outdated(context.Background(), "marcelocantos/tap/mnemo")
	if err != nil {
		t.Fatalf("Outdated: %v", err)
	}
	if info == nil {
		t.Fatal("expected non-nil info despite exit 1")
	}
	if info.CurrentVersion != "0.5.0" {
		t.Errorf("CurrentVersion = %q, want 0.5.0", info.CurrentVersion)
	}
}

// TestOutdated_PropagatesExecError: when the process fails hard
// (exec not found, context cancelled, whatever) AND produces no
// parseable JSON, we must still surface the exec error — not
// pretend everything is fine.
func TestOutdated_PropagatesExecError(t *testing.T) {
	b, fb := newFakeBrew()
	fb.stdout = nil
	fb.stderr = []byte(`Error: something went wrong`)
	fb.err = errors.New(`exec: "brew": executable file not found in $PATH`)

	_, err := b.Outdated(context.Background(), "foo")
	if err == nil {
		t.Fatal("expected error when runErr occurs and stdout is empty")
	}
	if !strings.Contains(err.Error(), "brew outdated") {
		t.Errorf("error lacks context: %v", err)
	}
}

func TestUpgrade_HappyPath(t *testing.T) {
	b, fb := newFakeBrew()
	if err := b.Upgrade(context.Background(), "foo"); err != nil {
		t.Fatalf("Upgrade: %v", err)
	}
	if len(fb.args) < 3 || fb.args[0] != "brew" ||
		fb.args[1] != "upgrade" || fb.args[2] != "foo" {
		t.Errorf("args: %v", fb.args)
	}
}

func TestUpgrade_PropagatesError(t *testing.T) {
	b, fb := newFakeBrew()
	fb.err = errors.New("exit status 1")
	if err := b.Upgrade(context.Background(), "foo"); err == nil {
		t.Fatal("expected error")
	}
}

func TestUpgrade_RejectsEmptyFormula(t *testing.T) {
	b, _ := newFakeBrew()
	if err := b.Upgrade(context.Background(), ""); err == nil {
		t.Fatal("expected error")
	}
}

// ---------- brew executable resolver ----------
//
// The resolver has to find `brew` in three situations:
//   1. A user-supplied override via MCPBRIDGE_BREW_PATH.
//   2. A normal $PATH lookup.
//   3. A fallback scan of well-known install locations (the
//      launchd case — this is the bug that motivated 🎯T2).
//
// Tests below exercise each, isolated via t.Setenv + t.TempDir so
// the host's real brew install is never touched and no global
// state leaks.

func writeFakeBrew(t *testing.T, dir, name string) string {
	t.Helper()
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatalf("mkdir: %v", err)
	}
	p := filepath.Join(dir, name)
	if err := os.WriteFile(p, []byte("#!/bin/sh\nexit 0\n"), 0o755); err != nil {
		t.Fatalf("write fake brew: %v", err)
	}
	return p
}

func TestResolveBrewExe_HonoursEnvOverride(t *testing.T) {
	dir := t.TempDir()
	fake := writeFakeBrew(t, dir, "brew")
	t.Setenv("MCPBRIDGE_BREW_PATH", fake)
	t.Setenv("PATH", "") // nothing findable via PATH

	got, err := resolveBrewExe()
	if err != nil {
		t.Fatalf("resolveBrewExe: %v", err)
	}
	if got != fake {
		t.Errorf("got %q, want %q", got, fake)
	}
}

func TestResolveBrewExe_FindsViaPATH(t *testing.T) {
	dir := t.TempDir()
	fake := writeFakeBrew(t, dir, "brew")
	t.Setenv("MCPBRIDGE_BREW_PATH", "")
	t.Setenv("PATH", dir)

	got, err := resolveBrewExe()
	if err != nil {
		t.Fatalf("resolveBrewExe: %v", err)
	}
	if got != fake {
		t.Errorf("got %q, want %q", got, fake)
	}
}

// TestResolveBrewExe_FallsBackWhenPATHEmpty covers the launchd case
// that motivated 🎯T2: $PATH does NOT contain a brew-installing
// directory, but brew exists at one of the well-known locations.
// We simulate this by overriding knownBrewPaths to point at a
// temp directory and clearing $PATH + $MCPBRIDGE_BREW_PATH.
func TestResolveBrewExe_FallsBackWhenPATHEmpty(t *testing.T) {
	dir := t.TempDir()
	fake := writeFakeBrew(t, dir, "brew")

	saved := knownBrewPaths
	knownBrewPaths = []string{fake}
	t.Cleanup(func() { knownBrewPaths = saved })

	t.Setenv("MCPBRIDGE_BREW_PATH", "")
	t.Setenv("PATH", "")

	got, err := resolveBrewExe()
	if err != nil {
		t.Fatalf("resolveBrewExe: %v", err)
	}
	if got != fake {
		t.Errorf("got %q, want %q", got, fake)
	}
}

func TestResolveBrewExe_ReturnsErrorWhenNotFound(t *testing.T) {
	saved := knownBrewPaths
	knownBrewPaths = []string{"/nonexistent/path/brew"}
	t.Cleanup(func() { knownBrewPaths = saved })

	t.Setenv("MCPBRIDGE_BREW_PATH", "")
	t.Setenv("PATH", "")

	_, err := resolveBrewExe()
	if err == nil {
		t.Fatal("expected error when brew cannot be found anywhere")
	}
	if !strings.Contains(err.Error(), "brew executable not found") {
		t.Errorf("error message lacks expected phrase: %v", err)
	}
}

func TestResolveBrewExe_IgnoresBadEnvOverride(t *testing.T) {
	// If MCPBRIDGE_BREW_PATH points at a non-existent path, the
	// resolver should fall through to the normal search rather
	// than fail loudly. The user clearly wanted brew to work.
	dir := t.TempDir()
	fake := writeFakeBrew(t, dir, "brew")

	t.Setenv("MCPBRIDGE_BREW_PATH", "/nonexistent/brew")
	t.Setenv("PATH", dir)

	got, err := resolveBrewExe()
	if err != nil {
		t.Fatalf("resolveBrewExe: %v", err)
	}
	if got != fake {
		t.Errorf("got %q, want %q", got, fake)
	}
}
