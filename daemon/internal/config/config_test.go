// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

package config

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

const stdioBrew = `{
	"schema": 2,
	"name": "mnemo",
	"command": "/opt/homebrew/bin/mnemo",
	"source": {"type": "brew", "formula": "marcelocantos/tap/mnemo"},
	"upgrade": "notify",
	"check_interval": "30m"
}`

const stdioBrewWithArgs = `{
	"schema": 2,
	"name": "mcp-foo",
	"command": "/usr/local/bin/mcp-foo",
	"args": ["--config", "/etc/foo.conf", "--quiet"],
	"source": {"type": "brew", "formula": "owner/tap/mcp-foo"}
}`

const httpGitHub = `{
	"schema": 2,
	"name": "vellum",
	"url": "http://localhost:9000/mcp",
	"source": {
		"type": "github",
		"repo": "owner/vellum",
		"asset": "vellum-{version}-{os}-{arch}.tar.gz",
		"binary_in_archive": "vellum",
		"checksum_asset": "SHA256SUMS"
	},
	"upgrade": "auto"
}`

func TestParseStdio(t *testing.T) {
	cfg, err := ParseBytes("test.json", []byte(stdioBrew))
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	if cfg.Backend() != BackendStdio {
		t.Errorf("backend: got %v", cfg.Backend())
	}
	if cfg.Command != "/opt/homebrew/bin/mnemo" {
		t.Errorf("command: got %q", cfg.Command)
	}
	if len(cfg.Args) != 0 {
		t.Errorf("args: got %v", cfg.Args)
	}
	if cfg.URL != "" {
		t.Errorf("url should be empty for stdio config, got %q", cfg.URL)
	}
	if cfg.Source.Formula != "marcelocantos/tap/mnemo" {
		t.Errorf("formula: got %q", cfg.Source.Formula)
	}
	if cfg.CheckInterval != 30*time.Minute {
		t.Errorf("check_interval: got %v", cfg.CheckInterval)
	}
}

func TestParseStdioWithArgs(t *testing.T) {
	cfg, err := ParseBytes("test.json", []byte(stdioBrewWithArgs))
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	want := []string{"--config", "/etc/foo.conf", "--quiet"}
	if len(cfg.Args) != len(want) {
		t.Fatalf("args length: got %d, want %d", len(cfg.Args), len(want))
	}
	for i, a := range want {
		if cfg.Args[i] != a {
			t.Errorf("args[%d]: got %q, want %q", i, cfg.Args[i], a)
		}
	}
}

func TestParseHTTP(t *testing.T) {
	cfg, err := ParseBytes("test.json", []byte(httpGitHub))
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	if cfg.Backend() != BackendHTTP {
		t.Errorf("backend: got %v", cfg.Backend())
	}
	if cfg.URL != "http://localhost:9000/mcp" {
		t.Errorf("url: got %q", cfg.URL)
	}
	if cfg.Command != "" {
		t.Errorf("command should be empty for http config, got %q", cfg.Command)
	}
	if cfg.Source.Type != SourceGitHub {
		t.Errorf("source type: got %q", cfg.Source.Type)
	}
	if cfg.Upgrade != UpgradeAuto {
		t.Errorf("upgrade: got %q", cfg.Upgrade)
	}
	if cfg.CheckInterval != DefaultCheckInterval {
		t.Errorf("default check_interval not applied: got %v", cfg.CheckInterval)
	}
}

func TestParseTildeExpansion(t *testing.T) {
	home, err := os.UserHomeDir()
	if err != nil {
		t.Skip("no HOME")
	}
	body := `{
		"schema": 2, "name": "x",
		"command": "~/bin/x",
		"source": {"type": "brew", "formula": "x"}
	}`
	cfg, err := ParseBytes("test.json", []byte(body))
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	want := home + "/bin/x"
	if cfg.Command != want {
		t.Errorf("expanded command: got %q, want %q", cfg.Command, want)
	}
}

func TestParseRejectsConnectionShape(t *testing.T) {
	cases := map[string]string{
		"both command and url": `{
			"schema": 2, "name": "x",
			"command": "/bin/x", "url": "http://localhost/x",
			"source": {"type": "brew", "formula": "x"}
		}`,
		"neither command nor url": `{
			"schema": 2, "name": "x",
			"source": {"type": "brew", "formula": "x"}
		}`,
		"https url": `{
			"schema": 2, "name": "x",
			"url": "https://localhost/x",
			"source": {"type": "brew", "formula": "x"}
		}`,
		"non-loopback url": `{
			"schema": 2, "name": "x",
			"url": "http://example.com/x",
			"source": {"type": "brew", "formula": "x"}
		}`,
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

func TestParseRejectsSchemaV1WithMigration(t *testing.T) {
	data := []byte(`{
		"schema": 1, "name": "x",
		"source": {"type": "brew", "formula": "x"}
	}`)
	_, err := ParseBytes("test.json", data)
	if err == nil {
		t.Fatal("expected error on schema v1")
	}
	if !strings.Contains(err.Error(), "schema v1 is not supported") {
		t.Errorf("error should mention schema v1: %v", err)
	}
	if !strings.Contains(err.Error(), `"command"`) {
		t.Errorf("error should mention command field as part of migration: %v", err)
	}
}

func TestParseRejectsBadSchema(t *testing.T) {
	data := []byte(`{
		"schema": 99, "name": "x",
		"command": "/bin/x",
		"source": {"type": "brew", "formula": "x"}
	}`)
	_, err := ParseBytes("test.json", data)
	if err == nil {
		t.Fatal("expected error on unknown schema version")
	}
}

func TestParseRejectsUnknownSource(t *testing.T) {
	data := []byte(`{
		"schema": 2, "name": "x",
		"command": "/bin/x",
		"source": {"type": "yolo"}
	}`)
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
		"missing schema": `{
			"name": "x", "command": "/bin/x",
			"source": {"type": "brew", "formula": "f"}
		}`,
		"missing name": `{
			"schema": 2, "command": "/bin/x",
			"source": {"type": "brew", "formula": "f"}
		}`,
		"brew no formula": `{
			"schema": 2, "name": "x", "command": "/bin/x",
			"source": {"type": "brew"}
		}`,
		"github no repo": `{
			"schema": 2, "name": "x", "command": "/bin/x",
			"source": {"type": "github"}
		}`,
		"empty source": `{
			"schema": 2, "name": "x", "command": "/bin/x",
			"source": {}
		}`,
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
	data := []byte(`{
		"schema": 2, "name": "x", "command": "/bin/x",
		"source": {"type": "brew", "formula": "f"},
		"upgrade": "maybe"
	}`)
	_, err := ParseBytes("test.json", data)
	if err == nil {
		t.Fatal("expected error on bad upgrade mode")
	}
}

func TestParseRejectsBadInterval(t *testing.T) {
	data := []byte(`{
		"schema": 2, "name": "x", "command": "/bin/x",
		"source": {"type": "brew", "formula": "f"},
		"check_interval": "banana"
	}`)
	_, err := ParseBytes("test.json", data)
	if err == nil {
		t.Fatal("expected error on bad duration")
	}

	data = []byte(`{
		"schema": 2, "name": "x", "command": "/bin/x",
		"source": {"type": "brew", "formula": "f"},
		"check_interval": "-5m"
	}`)
	_, err = ParseBytes("test.json", data)
	if err == nil {
		t.Fatal("expected error on negative duration")
	}
}

func TestExpandTildeNoPrefix(t *testing.T) {
	got, err := ExpandTilde("/abs/path")
	if err != nil {
		t.Fatal(err)
	}
	if got != "/abs/path" {
		t.Errorf("non-tilde path mutated: got %q", got)
	}
}

func TestExpandTildeHome(t *testing.T) {
	home, err := os.UserHomeDir()
	if err != nil {
		t.Skip("no HOME")
	}
	got, err := ExpandTilde("~/bin/foo")
	if err != nil {
		t.Fatal(err)
	}
	if got != home+"/bin/foo" {
		t.Errorf("expanded: got %q", got)
	}
	got, err = ExpandTilde("~")
	if err != nil {
		t.Fatal(err)
	}
	if got != home {
		t.Errorf("bare ~ : got %q, want %q", got, home)
	}
}

func TestExpandTildeUnknownUser(t *testing.T) {
	_, err := ExpandTilde("~no-such-user-xyz/bin")
	if err == nil {
		t.Fatal("expected error for unknown user")
	}
}

func TestLoadDiscoversMultipleDirs(t *testing.T) {
	dirA := t.TempDir()
	dirB := t.TempDir()

	if err := os.WriteFile(filepath.Join(dirA, "mnemo.json"), []byte(`{
		"schema": 2, "name": "mnemo", "command": "/bin/mnemo",
		"source": {"type": "brew", "formula": "marcelocantos/tap/mnemo"}
	}`), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dirB, "foo.json"), []byte(`{
		"schema": 2, "name": "foo", "command": "/bin/foo",
		"source": {"type": "brew", "formula": "owner/tap/foo"}
	}`), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dirB, "mnemo.json"), []byte(`{
		"schema": 2, "name": "mnemo", "command": "/bin/mnemo-other",
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
		"schema": 2, "name": "ok", "command": "/bin/ok",
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
		"schema": 2, "name": "ok", "command": "/bin/ok",
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
