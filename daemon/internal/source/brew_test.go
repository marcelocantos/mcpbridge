// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

package source

import (
	"context"
	"errors"
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
