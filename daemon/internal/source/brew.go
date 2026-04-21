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
	"os"
	"os/exec"
	"sync"
)

// knownBrewPaths are the install locations we probe when `brew` is
// not reachable via $PATH. Ordering matters: Apple Silicon Homebrew
// first (the common case on developer machines), then Intel Homebrew,
// then Linuxbrew. This mirrors the usual $PATH search order on a
// correctly-configured interactive shell, which is specifically NOT
// what launchd gives us when the daemon runs under `brew services`.
var knownBrewPaths = []string{
	"/opt/homebrew/bin/brew",
	"/usr/local/bin/brew",
	"/home/linuxbrew/.linuxbrew/bin/brew",
}

// resolveBrewExe returns an absolute path to the `brew` executable,
// honouring an explicit override from the environment first, then
// a $PATH lookup, then a fallback scan of well-known install
// locations. Returns an error only when none of those succeed —
// which in practice means brew is not installed.
//
// Why this exists: when the daemon runs under launchd (via
// `brew services start mcpbridge`), launchd's default $PATH is
// /usr/bin:/bin:/usr/sbin:/sbin — it does NOT include
// /opt/homebrew/bin. Relying on bare `exec.LookPath("brew")`
// silently disables the entire auto-upgrade path for brew-backed
// configs. This resolver restores the behaviour a user expects:
// "if brew is installed anywhere normal, find it."
func resolveBrewExe() (string, error) {
	if p := os.Getenv("MCPBRIDGE_BREW_PATH"); p != "" {
		if _, err := os.Stat(p); err == nil {
			return p, nil
		}
		// Override set but not executable: fall through to the
		// normal search rather than fail loudly. The user clearly
		// intended brew to be available; if their override is
		// wrong we'd rather still work.
	}
	if p, err := exec.LookPath("brew"); err == nil {
		return p, nil
	}
	for _, p := range knownBrewPaths {
		if info, err := os.Stat(p); err == nil && !info.IsDir() {
			return p, nil
		}
	}
	return "", fmt.Errorf("brew executable not found on $PATH or in " +
		"any known install location (/opt/homebrew/bin, " +
		"/usr/local/bin, /home/linuxbrew/.linuxbrew/bin); " +
		"set MCPBRIDGE_BREW_PATH to override")
}

// brewExeCache memoises the resolver result for the lifetime of the
// daemon process. Resolving is cheap, but doing it on every poll
// would turn a handful of stat()s into thousands per day. Callers
// that need to force re-resolution (tests, reconfiguration) should
// instantiate a fresh Brew.
type brewExeCache struct {
	once sync.Once
	path string
	err  error
}

func (c *brewExeCache) get() (string, error) {
	c.once.Do(func() {
		c.path, c.err = resolveBrewExe()
	})
	return c.path, c.err
}

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
// the host's real brew install. The executable resolver is
// similarly injectable via `exe` — tests can hand it a canned
// string without requiring a real brew install on the test host.
type Brew struct {
	// runCapture runs a command and returns its combined stdout
	// and stderr plus the error. Used for the JSON query path.
	runCapture func(ctx context.Context, name string, args ...string) (stdout, stderr []byte, err error)
	// run runs a command without capturing output (used by Upgrade).
	// Stderr is captured on error so we can surface it to the caller.
	run func(ctx context.Context, name string, args ...string) error
	// exe returns the absolute path to the brew executable to
	// invoke. Memoised by the default implementation; a test can
	// swap in a stub that returns any string.
	exe func() (string, error)
}

// NewBrew returns a Brew that shells out to the real `brew`
// executable, resolved via resolveBrewExe so the daemon works
// under launchd where /opt/homebrew/bin is not on $PATH.
func NewBrew() *Brew {
	cache := &brewExeCache{}
	return &Brew{
		runCapture: realRunCapture,
		run:        realRun,
		exe:        cache.get,
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
	exe, err := b.exe()
	if err != nil {
		return nil, fmt.Errorf("brew outdated: %w", err)
	}
	stdout, stderr, runErr := b.runCapture(ctx,
		exe, "outdated", "--json=v2", "--formula", formula)

	// `brew outdated` uses exit 1 (not 0) when the named formula
	// IS outdated, and still prints valid JSON on stdout. We must
	// attempt to parse the JSON regardless of exit status and only
	// surface runErr when parsing fails — otherwise the daemon
	// correctly detects an outdated formula but treats the signal
	// as an error and ignores it. Only exec failures (e.g.
	// "brew: not found") produce neither JSON nor stderr context
	// and legitimately deserve to propagate.
	var parsed struct {
		Formulae []struct {
			Name              string   `json:"name"`
			InstalledVersions []string `json:"installed_versions"`
			CurrentVersion    string   `json:"current_version"`
		} `json:"formulae"`
	}
	if jsonErr := json.Unmarshal(stdout, &parsed); jsonErr != nil {
		if runErr != nil {
			return nil, fmt.Errorf("brew outdated: %w (stderr: %s)",
				runErr, bytes.TrimSpace(stderr))
		}
		return nil, fmt.Errorf("brew outdated: parse JSON: %w", jsonErr)
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
	exe, err := b.exe()
	if err != nil {
		return fmt.Errorf("brew upgrade %s: %w", formula, err)
	}
	if err := b.run(ctx, exe, "upgrade", formula); err != nil {
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
