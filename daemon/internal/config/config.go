// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Package config loads per-server JSON config files. Each file
// describes one wrapped MCP server: how to reach it (stdio child or
// HTTP upstream), how to check for a new version, and whether
// auto-upgrade is opt-in.
//
// Two consumers read these files:
//
//   - The wrapper, which is launched as `mcpbridge connect <path>`
//     and reads the file pointed at by <path>. It cares about the
//     connection fields (command/args or url) and the name (used
//     to register with the daemon).
//   - The daemon, which scans well-known directories, reads every
//     *.json file, and uses the source/upgrade fields for upgrade
//     polling. It ignores the connection fields.
//
// Schema is shared between both consumers and documented in
// docs/config-schema.md (authoritative reference).
package config

import (
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"net/url"
	"os"
	"os/user"
	"path/filepath"
	"strings"
	"time"
)

// SchemaVersion is the config schema version this build understands.
// v2 adds connection metadata (command/args/url) to the envelope so
// the wrapper can find its backend without argv flags. v1 is no
// longer accepted; pre-1.0 release notes describe the migration.
const SchemaVersion = 2

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

// BackendKind names the connection style declared by a Config.
type BackendKind int

const (
	// BackendStdio: the wrapper fork/execs Command with Args and
	// bridges stdin/stdout. Command is an absolute path after
	// tilde expansion.
	BackendStdio BackendKind = iota + 1
	// BackendHTTP: the wrapper opens an HTTP MCP Streamable session
	// against URL.
	BackendHTTP
)

// Config describes a single wrapped MCP server.
//
// Exactly one of {Command, URL} is set after a successful parse;
// callers can distinguish via Backend().
type Config struct {
	Schema int    `json:"schema"`
	Name   string `json:"name"`

	// Connection — exactly one of Command or URL is set.
	Command string   `json:"command,omitempty"`
	Args    []string `json:"args,omitempty"`
	URL     string   `json:"url,omitempty"`

	Source        Source        `json:"source"`
	Upgrade       UpgradeMode   `json:"upgrade,omitempty"`
	CheckInterval time.Duration `json:"-"`
	RawInterval   string        `json:"check_interval,omitempty"`
	Path          string        `json:"-"` // absolute path of the source file
}

// Backend reports which connection style this config declares.
// Panics on a config that hasn't gone through ParseBytes — validation
// guarantees exactly one of Command or URL is set after parsing.
func (c *Config) Backend() BackendKind {
	switch {
	case c.Command != "":
		return BackendStdio
	case c.URL != "":
		return BackendHTTP
	default:
		panic("config: Backend() called on unvalidated Config")
	}
}

// DefaultCheckInterval is used when a config omits check_interval.
const DefaultCheckInterval = time.Hour

// migrationHintV1 is what we tell users who have a schema:1 file on
// disk. Pre-1.0, no auto-migration; the user edits the file once and
// moves on.
const migrationHintV1 = `Schema v1 is no longer supported. Migrate by:
- Setting "schema": 2
- Adding "command": "/path/to/binary" and optional "args": [...] for stdio servers,
  or "url": "http://localhost:PORT/path" for HTTP servers.
The file's existing "name" / "source" / "upgrade" / "check_interval"
fields are unchanged. See docs/config-schema.md.`

// ParseBytes parses one file's bytes into a validated Config. The
// `path` parameter is used for error messages and stored on the
// returned Config; it is not required to point at a real file.
func ParseBytes(path string, data []byte) (*Config, error) {
	var c Config
	if err := json.Unmarshal(data, &c); err != nil {
		return nil, fmt.Errorf("%s: invalid JSON: %w", path, err)
	}
	c.Path = path

	if c.Schema == 0 {
		return nil, fmt.Errorf("%s: missing required field \"schema\"", path)
	}
	if c.Schema == 1 {
		return nil, fmt.Errorf("%s: schema v1 is not supported.\n%s",
			path, migrationHintV1)
	}
	if c.Schema != SchemaVersion {
		return nil, fmt.Errorf(
			"%s: unsupported schema version %d (this build speaks %d)",
			path, c.Schema, SchemaVersion)
	}

	if c.Name == "" {
		return nil, fmt.Errorf("%s: missing required field \"name\"", path)
	}

	if err := validateConnection(path, &c); err != nil {
		return nil, err
	}

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

// validateConnection enforces exactly-one-of {Command, URL}, expands
// `~` in Command, and rejects HTTP URLs that aren't plain http:// to
// a loopback host.
func validateConnection(path string, c *Config) error {
	hasCmd := c.Command != ""
	hasURL := c.URL != ""
	switch {
	case hasCmd && hasURL:
		return fmt.Errorf("%s: \"command\" and \"url\" are mutually exclusive", path)
	case !hasCmd && !hasURL:
		return fmt.Errorf("%s: exactly one of \"command\" or \"url\" is required", path)
	}

	if hasCmd {
		expanded, err := ExpandTilde(c.Command)
		if err != nil {
			return fmt.Errorf("%s: expand command: %w", path, err)
		}
		c.Command = expanded
		// Args expand via the child's own argv handling — we don't
		// shell-expand them here. Document this in config-schema.md.
		return nil
	}

	// hasURL case.
	u, err := url.Parse(c.URL)
	if err != nil {
		return fmt.Errorf("%s: bad url %q: %w", path, c.URL, err)
	}
	if u.Scheme != "http" {
		return fmt.Errorf("%s: url scheme must be http (got %q); https and remote hosts are out of scope for v1",
			path, u.Scheme)
	}
	if !isLoopback(u.Hostname()) {
		return fmt.Errorf("%s: url host must be loopback (localhost / 127.0.0.1 / ::1); got %q",
			path, u.Hostname())
	}
	return nil
}

func isLoopback(host string) bool {
	switch host {
	case "localhost", "127.0.0.1", "::1":
		return true
	}
	return false
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

// ExpandTilde resolves a leading `~` or `~user` path segment to an
// absolute path. Paths without a leading `~` are returned unchanged.
// Errors only on a `~user` segment that doesn't resolve.
func ExpandTilde(p string) (string, error) {
	if p == "" || p[0] != '~' {
		return p, nil
	}
	// Split the leading segment from the rest.
	slash := strings.IndexByte(p, '/')
	var head, tail string
	if slash < 0 {
		head, tail = p, ""
	} else {
		head, tail = p[:slash], p[slash:]
	}
	switch {
	case head == "~":
		home, err := os.UserHomeDir()
		if err != nil {
			return "", err
		}
		return home + tail, nil
	default:
		// `~user` — look up the named user's home directory.
		u, err := user.Lookup(head[1:])
		if err != nil {
			return "", fmt.Errorf("expand %q: %w", p, err)
		}
		return u.HomeDir + tail, nil
	}
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
	for i := 1; i < len(entries); i++ {
		j := i
		for j > 0 && entries[j-1].Name() > entries[j].Name() {
			entries[j-1], entries[j] = entries[j], entries[j-1]
			j--
		}
	}
}
