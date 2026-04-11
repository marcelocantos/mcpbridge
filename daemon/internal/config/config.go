// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Package config loads per-server JSON config files from
// ~/.config/mcpbridge/ and $prefix/share/mcpbridge/. The schema is
// shared with the wrapper and documented in docs/config-schema.md
// (authoritative reference). Each file describes one wrapped MCP
// server: how to check for a new version, how to install it, and
// whether auto-upgrade is opt-in.
package config

import (
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"os"
	"path/filepath"
	"strings"
	"time"
)

// SchemaVersion is the config schema version this daemon understands.
// Bump (and keep backward-compat parsing around) when fields are
// added or renamed.
const SchemaVersion = 1

// UpgradeMode controls what the daemon does when a new version is
// detected.
type UpgradeMode string

const (
	UpgradeOff    UpgradeMode = "off"
	UpgradeNotify UpgradeMode = "notify"
	UpgradeAuto   UpgradeMode = "auto"
)

// SourceType identifies the upgrade source backend.
type SourceType string

const (
	SourceBrew   SourceType = "brew"
	SourceGitHub SourceType = "github"
)

// Source is the discriminated union of per-type source metadata.
// Only one of Brew or GitHub is populated, matching Type.
type Source struct {
	Type SourceType `json:"type"`

	// brew
	Formula string `json:"formula,omitempty"`

	// github
	Repo            string `json:"repo,omitempty"`
	Asset           string `json:"asset,omitempty"`
	BinaryInArchive string `json:"binary_in_archive,omitempty"`
	ChecksumAsset   string `json:"checksum_asset,omitempty"`
}

// Config describes a single wrapped MCP server.
//
// The wire schema is human-authored JSON, so we define an explicit
// intermediate type for unmarshaling (with string duration) and a
// validated runtime type (with time.Duration). Load is the path from
// one to the other.
type Config struct {
	Schema        int           `json:"schema"`
	Name          string        `json:"name"`
	Source        Source        `json:"source"`
	Upgrade       UpgradeMode   `json:"upgrade,omitempty"`
	CheckInterval time.Duration `json:"-"`
	RawInterval   string        `json:"check_interval,omitempty"`
	Path          string        `json:"-"` // file this came from
}

// DefaultCheckInterval is used when a config omits check_interval.
const DefaultCheckInterval = time.Hour

// ParseBytes parses one file's bytes into a validated Config. The
// `path` parameter is only used for error messages — it is not
// required to point at a real file.
func ParseBytes(path string, data []byte) (*Config, error) {
	var c Config
	if err := json.Unmarshal(data, &c); err != nil {
		return nil, fmt.Errorf("%s: invalid JSON: %w", path, err)
	}
	c.Path = path

	if c.Schema == 0 {
		return nil, fmt.Errorf("%s: missing required field \"schema\"", path)
	}
	if c.Schema != SchemaVersion {
		return nil, fmt.Errorf(
			"%s: unsupported schema version %d (this daemon speaks %d)",
			path, c.Schema, SchemaVersion)
	}

	if c.Name == "" {
		return nil, fmt.Errorf("%s: missing required field \"name\"", path)
	}

	// Upgrade defaults to notify per the plan: a config without an
	// explicit upgrade mode is interpreted as "tell me about updates,
	// don't install them for me automatically."
	switch c.Upgrade {
	case "":
		c.Upgrade = UpgradeNotify
	case UpgradeOff, UpgradeNotify, UpgradeAuto:
		// valid
	default:
		return nil, fmt.Errorf("%s: unknown upgrade mode %q", path, c.Upgrade)
	}

	if err := validateSource(path, &c.Source); err != nil {
		return nil, err
	}

	if c.RawInterval == "" {
		c.CheckInterval = DefaultCheckInterval
	} else {
		d, err := time.ParseDuration(c.RawInterval)
		if err != nil {
			return nil, fmt.Errorf("%s: bad check_interval %q: %w",
				path, c.RawInterval, err)
		}
		if d <= 0 {
			return nil, fmt.Errorf("%s: check_interval must be positive, got %q",
				path, c.RawInterval)
		}
		c.CheckInterval = d
	}

	return &c, nil
}

func validateSource(path string, s *Source) error {
	switch s.Type {
	case SourceBrew:
		if s.Formula == "" {
			return fmt.Errorf("%s: brew source requires \"formula\"", path)
		}
	case SourceGitHub:
		if s.Repo == "" {
			return fmt.Errorf("%s: github source requires \"repo\"", path)
		}
		// Asset / binary_in_archive / checksum_asset are all
		// optional here — the github source backend can fall back
		// to sensible defaults. We only enforce the minimum for
		// discovery.
	case "":
		return fmt.Errorf("%s: missing required field \"source.type\"", path)
	default:
		return fmt.Errorf("%s: unknown source type %q", path, s.Type)
	}
	return nil
}

// ParseFile reads and parses one config file.
func ParseFile(path string) (*Config, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	return ParseBytes(path, data)
}

// LoadResult summarises a Load call: the map of successfully parsed
// configs keyed by name, plus any per-file errors. One broken file
// does not prevent discovery of the rest — the daemon logs the
// errors and carries on.
type LoadResult struct {
	Configs map[string]*Config
	Errors  []error
}

// Load walks each listed directory in order, reads *.json files,
// parses them, and returns a LoadResult. Directories that do not
// exist are silently skipped. Name collisions are resolved by
// earlier-dir-wins, which implements the
// "~/.config/mcpbridge beats $prefix/share/mcpbridge" precedence
// the wire protocol spec documents.
func Load(dirs ...string) *LoadResult {
	out := &LoadResult{
		Configs: make(map[string]*Config),
	}

	for _, dir := range dirs {
		entries, err := os.ReadDir(dir)
		if err != nil {
			if errors.Is(err, os.ErrNotExist) {
				continue
			}
			out.Errors = append(out.Errors,
				fmt.Errorf("read dir %s: %w", dir, err))
			continue
		}

		// Sort happens implicitly: os.ReadDir returns entries in
		// directory order, but we sort by filename for determinism.
		// filepath.Base(file).
		sortDirEntries(entries)

		for _, e := range entries {
			if e.IsDir() {
				continue
			}
			name := e.Name()
			if !strings.HasSuffix(name, ".json") {
				continue
			}
			path := filepath.Join(dir, name)
			cfg, err := ParseFile(path)
			if err != nil {
				out.Errors = append(out.Errors, err)
				continue
			}
			if existing, ok := out.Configs[cfg.Name]; ok {
				slog.Warn("config: duplicate name; keeping earlier",
					"name", cfg.Name,
					"kept", existing.Path,
					"discarded", cfg.Path)
				continue
			}
			out.Configs[cfg.Name] = cfg
		}
	}

	return out
}

// Sort helper separated so it's easy to replace if we ever want a
// different ordering.
func sortDirEntries(entries []os.DirEntry) {
	// Stable sort by filename. Using a tiny insertion sort avoids
	// pulling in sort for a handful of entries, and the caller
	// populates the slice in-place.
	for i := 1; i < len(entries); i++ {
		j := i
		for j > 0 && entries[j-1].Name() > entries[j].Name() {
			entries[j-1], entries[j] = entries[j], entries[j-1]
			j--
		}
	}
}
