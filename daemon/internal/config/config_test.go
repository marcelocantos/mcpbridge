// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

package config

import (
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestParseBrew(t *testing.T) {
	data := []byte(`{
		"schema": 1,
		"name": "mnemo",
		"source": {"type": "brew", "formula": "marcelocantos/tap/mnemo"},
		"upgrade": "notify",
		"check_interval": "30m"
	}`)
	cfg, err := ParseBytes("test.json", data)
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	if cfg.Name != "mnemo" {
		t.Errorf("name: got %q", cfg.Name)
	}
	if cfg.Source.Type != SourceBrew {
		t.Errorf("type: got %q", cfg.Source.Type)
	}
	if cfg.Source.Formula != "marcelocantos/tap/mnemo" {
		t.Errorf("formula: got %q", cfg.Source.Formula)
	}
	if cfg.Upgrade != UpgradeNotify {
		t.Errorf("upgrade: got %q", cfg.Upgrade)
	}
	if cfg.CheckInterval != 30*time.Minute {
		t.Errorf("check_interval: got %v", cfg.CheckInterval)
	}
}

func TestParseGitHub(t *testing.T) {
	data := []byte(`{
		"schema": 1,
		"name": "mcp-foo",
		"source": {
			"type": "github",
			"repo": "owner/mcp-foo",
			"asset": "mcp-foo-{version}-{os}-{arch}.tar.gz",
			"binary_in_archive": "mcp-foo",
			"checksum_asset": "SHA256SUMS"
		},
		"upgrade": "auto"
	}`)
	cfg, err := ParseBytes("test.json", data)
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	if cfg.Source.Type != SourceGitHub {
		t.Errorf("type: got %q", cfg.Source.Type)
	}
	if cfg.Source.Repo != "owner/mcp-foo" {
		t.Errorf("repo: got %q", cfg.Source.Repo)
	}
	if cfg.Source.Asset == "" || cfg.Source.BinaryInArchive == "" || cfg.Source.ChecksumAsset == "" {
		t.Errorf("github optional fields not populated: %+v", cfg.Source)
	}
	if cfg.Upgrade != UpgradeAuto {
		t.Errorf("upgrade: got %q", cfg.Upgrade)
	}
	if cfg.CheckInterval != DefaultCheckInterval {
		t.Errorf("default check_interval not applied: got %v", cfg.CheckInterval)
	}
}

func TestParseRejectsBadSchema(t *testing.T) {
	data := []byte(`{"schema": 99, "name": "x", "source": {"type": "brew", "formula": "x"}}`)
	_, err := ParseBytes("test.json", data)
	if err == nil {
		t.Fatal("expected error on unknown schema version")
	}
}

func TestParseRejectsUnknownSource(t *testing.T) {
	data := []byte(`{"schema": 1, "name": "x", "source": {"type": "yolo"}}`)
	_, err := ParseBytes("test.json", data)
	if err == nil {
		t.Fatal("expected error on unknown source type")
	}
}

func TestParseRejectsMalformedJSON(t *testing.T) {
	_, err := ParseBytes("test.json", []byte(`{not json`))
	if err == nil {
		t.Fatal("expected error on malformed JSON")
	}
}

func TestParseRejectsMissingFields(t *testing.T) {
	cases := map[string]string{
		"missing schema": `{"name": "x", "source": {"type": "brew", "formula": "f"}}`,
		"missing name":   `{"schema": 1, "source": {"type": "brew", "formula": "f"}}`,
		"brew no formula": `{"schema": 1, "name": "x", "source": {"type": "brew"}}`,
		"github no repo":  `{"schema": 1, "name": "x", "source": {"type": "github"}}`,
		"empty source":    `{"schema": 1, "name": "x", "source": {}}`,
	}
	for label, body := range cases {
		t.Run(label, func(t *testing.T) {
			_, err := ParseBytes("test.json", []byte(body))
			if err == nil {
				t.Fatalf("expected error for %s", label)
			}
		})
	}
}

func TestParseRejectsBadUpgradeMode(t *testing.T) {
	data := []byte(`{"schema": 1, "name": "x", "source": {"type": "brew", "formula": "f"}, "upgrade": "maybe"}`)
	_, err := ParseBytes("test.json", data)
	if err == nil {
		t.Fatal("expected error on bad upgrade mode")
	}
}

func TestParseRejectsBadInterval(t *testing.T) {
	data := []byte(`{"schema": 1, "name": "x", "source": {"type": "brew", "formula": "f"}, "check_interval": "banana"}`)
	_, err := ParseBytes("test.json", data)
	if err == nil {
		t.Fatal("expected error on bad duration")
	}

	data = []byte(`{"schema": 1, "name": "x", "source": {"type": "brew", "formula": "f"}, "check_interval": "-5m"}`)
	_, err = ParseBytes("test.json", data)
	if err == nil {
		t.Fatal("expected error on negative duration")
	}
}

func TestLoadDiscoversMultipleDirs(t *testing.T) {
	dirA := t.TempDir()
	dirB := t.TempDir()

	// dirA has mnemo; dirB has foo and also mnemo (collision).
	if err := os.WriteFile(filepath.Join(dirA, "mnemo.json"), []byte(`{
		"schema": 1, "name": "mnemo",
		"source": {"type": "brew", "formula": "marcelocantos/tap/mnemo"}
	}`), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dirB, "foo.json"), []byte(`{
		"schema": 1, "name": "foo",
		"source": {"type": "brew", "formula": "owner/tap/foo"}
	}`), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dirB, "mnemo.json"), []byte(`{
		"schema": 1, "name": "mnemo",
		"source": {"type": "brew", "formula": "other/tap/mnemo"}
	}`), 0o644); err != nil {
		t.Fatal(err)
	}

	res := Load(dirA, dirB)
	if len(res.Errors) != 0 {
		t.Errorf("unexpected errors: %v", res.Errors)
	}
	if len(res.Configs) != 2 {
		t.Errorf("want 2 configs, got %d: %v", len(res.Configs), res.Configs)
	}
	mnemo, ok := res.Configs["mnemo"]
	if !ok {
		t.Fatal("mnemo missing")
	}
	if mnemo.Source.Formula != "marcelocantos/tap/mnemo" {
		t.Errorf("earlier-dir-wins violated: got %q", mnemo.Source.Formula)
	}
	if _, ok := res.Configs["foo"]; !ok {
		t.Error("foo missing from second dir")
	}
}

func TestLoadCollectsPerFileErrors(t *testing.T) {
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "ok.json"), []byte(`{
		"schema": 1, "name": "ok",
		"source": {"type": "brew", "formula": "x"}
	}`), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "bad.json"),
		[]byte(`{not json`), 0o644); err != nil {
		t.Fatal(err)
	}

	res := Load(dir)
	if len(res.Configs) != 1 {
		t.Errorf("want 1 config, got %d", len(res.Configs))
	}
	if _, ok := res.Configs["ok"]; !ok {
		t.Error("ok config should be discovered despite bad neighbour")
	}
	if len(res.Errors) != 1 {
		t.Errorf("want 1 error, got %d", len(res.Errors))
	}
}

func TestLoadSkipsMissingDirs(t *testing.T) {
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "ok.json"), []byte(`{
		"schema": 1, "name": "ok",
		"source": {"type": "brew", "formula": "x"}
	}`), 0o644); err != nil {
		t.Fatal(err)
	}
	res := Load("/nonexistent/xyz", dir)
	if len(res.Errors) != 0 {
		t.Errorf("missing dir should be silent, got %v", res.Errors)
	}
	if len(res.Configs) != 1 {
		t.Errorf("want 1 config, got %d", len(res.Configs))
	}
}
