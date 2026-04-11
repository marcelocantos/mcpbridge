// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Package source holds upgrade source backends used by the daemon
// to detect and install newer versions of wrapped MCP servers.
//
// Each backend is a thin shell around a specific distribution
// channel. brew.go shells out to `brew outdated --json=v2` to check
// for an upgrade and `brew upgrade <formula>` to install it; the
// github backend (later target) talks to api.github.com directly.
//
// Backends are pure with respect to timing: they do not schedule
// their own polls. The scheduler (later target) decides when to
// call them and how to react to the results.
package source

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"os/exec"
)

// OutdatedInfo describes a single formula that brew reports as
// needing an upgrade. Fields match the subset of `brew outdated
// --json=v2` output that we actually use.
type OutdatedInfo struct {
	Name             string `json:"name"`
	InstalledVersion string
	CurrentVersion   string `json:"current_version"`
}

// Brew is the upgrade-source backend for Homebrew formulas. Zero
// value is NOT usable — construct with NewBrew.
//
// The exec side is injectable via runCapture / run so tests can
// swap in a fake brew binary without going through PATH or touching
// the host's real brew install.
type Brew struct {
	// runCapture runs a command and returns its combined stdout
	// and stderr plus the error. Used for the JSON query path.
	runCapture func(ctx context.Context, name string, args ...string) (stdout, stderr []byte, err error)
	// run runs a command without capturing output (used by Upgrade).
	// Stderr is captured on error so we can surface it to the caller.
	run func(ctx context.Context, name string, args ...string) error
}

// NewBrew returns a Brew that shells out to the real `brew`
// executable on PATH.
func NewBrew() *Brew {
	return &Brew{
		runCapture: realRunCapture,
		run:        realRun,
	}
}

// Outdated asks brew whether the named formula has a newer version
// available. Returns a non-nil *OutdatedInfo when an upgrade is
// available, nil when the formula is current or not installed, and
// an error on any problem talking to brew.
//
// Implementation note: `brew outdated --json=v2 --formula
// <name>` does two things — if the formula is outdated it appears
// in the result; if it is current OR not installed at all, the
// "formulae" list is empty. For this target we treat both of those
// cases as "nothing to do." A future target can split them apart
// if we grow a "wrapped server is missing from the system entirely"
// warning.
func (b *Brew) Outdated(ctx context.Context, formula string) (*OutdatedInfo, error) {
	if formula == "" {
		return nil, fmt.Errorf("brew Outdated: empty formula")
	}
	stdout, stderr, err := b.runCapture(ctx,
		"brew", "outdated", "--json=v2", "--formula", formula)
	if err != nil {
		return nil, fmt.Errorf("brew outdated: %w (stderr: %s)",
			err, bytes.TrimSpace(stderr))
	}

	var parsed struct {
		Formulae []struct {
			Name              string   `json:"name"`
			InstalledVersions []string `json:"installed_versions"`
			CurrentVersion    string   `json:"current_version"`
		} `json:"formulae"`
	}
	if err := json.Unmarshal(stdout, &parsed); err != nil {
		return nil, fmt.Errorf("brew outdated: parse JSON: %w", err)
	}

	for _, f := range parsed.Formulae {
		if f.Name != formula {
			continue
		}
		installed := ""
		if len(f.InstalledVersions) > 0 {
			installed = f.InstalledVersions[len(f.InstalledVersions)-1]
		}
		return &OutdatedInfo{
			Name:             f.Name,
			InstalledVersion: installed,
			CurrentVersion:   f.CurrentVersion,
		}, nil
	}
	return nil, nil
}

// Upgrade runs `brew upgrade <formula>` synchronously. Returns nil
// on success; on failure the error includes brew's stderr.
func (b *Brew) Upgrade(ctx context.Context, formula string) error {
	if formula == "" {
		return fmt.Errorf("brew Upgrade: empty formula")
	}
	if err := b.run(ctx, "brew", "upgrade", formula); err != nil {
		return fmt.Errorf("brew upgrade %s: %w", formula, err)
	}
	return nil
}

// ---------- exec seam ----------

func realRunCapture(ctx context.Context, name string, args ...string) (stdout, stderr []byte, err error) {
	cmd := exec.CommandContext(ctx, name, args...)
	var sout, serr bytes.Buffer
	cmd.Stdout = &sout
	cmd.Stderr = &serr
	err = cmd.Run()
	return sout.Bytes(), serr.Bytes(), err
}

func realRun(ctx context.Context, name string, args ...string) error {
	cmd := exec.CommandContext(ctx, name, args...)
	var serr bytes.Buffer
	cmd.Stderr = &serr
	if err := cmd.Run(); err != nil {
		if serr.Len() > 0 {
			return fmt.Errorf("%w: %s", err, bytes.TrimSpace(serr.Bytes()))
		}
		return err
	}
	return nil
}
