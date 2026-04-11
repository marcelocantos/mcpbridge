// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

package source

import (
	"archive/tar"
	"bytes"
	"compress/gzip"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"github.com/marcelocantos/mcpbridge/daemon/internal/config"
)

// newFakeGitHub stands up an httptest.Server and returns a GitHub
// backend whose apiBase and downloadBase point at it. The handler
// serves:
//   - /repos/<repo>/releases/latest -> JSON release metadata
//   - /dl/<name> -> the file named <name> from files
func newFakeGitHub(t *testing.T, releaseJSON string, files map[string][]byte) *GitHub {
	t.Helper()
	mux := http.NewServeMux()
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		switch {
		case strings.HasSuffix(r.URL.Path, "/releases/latest"):
			w.Header().Set("Content-Type", "application/json")
			_, _ = w.Write([]byte(releaseJSON))
		case strings.HasPrefix(r.URL.Path, "/dl/"):
			name := strings.TrimPrefix(r.URL.Path, "/dl/")
			if b, ok := files[name]; ok {
				_, _ = w.Write(b)
				return
			}
			http.NotFound(w, r)
		default:
			http.NotFound(w, r)
		}
	})
	srv := httptest.NewServer(mux)
	t.Cleanup(srv.Close)

	return &GitHub{
		apiBase:      srv.URL,
		downloadBase: srv.URL,
		http:         srv.Client(),
	}
}

func TestGitHub_Latest(t *testing.T) {
	release := `{
		"tag_name": "v0.5.0",
		"assets": [
			{"name": "mcp-foo-0.5.0-darwin-arm64", "browser_download_url": "https://example.invalid/dl/mcp-foo-0.5.0-darwin-arm64", "size": 1234},
			{"name": "SHA256SUMS", "browser_download_url": "https://example.invalid/dl/SHA256SUMS", "size": 99}
		]
	}`
	g := newFakeGitHub(t, release, nil)

	info, err := g.Latest(context.Background(), "owner/mcp-foo")
	if err != nil {
		t.Fatalf("Latest: %v", err)
	}
	if info.TagName != "v0.5.0" {
		t.Errorf("tag: got %q", info.TagName)
	}
	if len(info.Assets) != 2 {
		t.Errorf("assets: got %d", len(info.Assets))
	}
}

func TestGitHub_LatestRejectsNon2xx(t *testing.T) {
	mux := http.NewServeMux()
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		http.Error(w, `{"message":"Not Found"}`, http.StatusNotFound)
	})
	srv := httptest.NewServer(mux)
	defer srv.Close()
	g := &GitHub{apiBase: srv.URL, http: srv.Client()}
	_, err := g.Latest(context.Background(), "owner/missing")
	if err == nil {
		t.Fatal("expected error on 404")
	}
}

func TestGitHub_IsNewer(t *testing.T) {
	g := &GitHub{}
	cases := []struct {
		installed, latest string
		want              bool
	}{
		{"0.4.2", "v0.5.0", true},
		{"v0.4.2", "v0.5.0", true},
		{"0.5.0", "v0.5.0", false},
		{"v0.5.0", "0.5.0", false},
		{"", "v0.5.0", true},
	}
	for _, tc := range cases {
		t.Run(tc.installed+"->"+tc.latest, func(t *testing.T) {
			got := g.IsNewer(tc.installed, tc.latest)
			if got != tc.want {
				t.Errorf("got %v want %v", got, tc.want)
			}
		})
	}
}

func TestSubstituteAssetName(t *testing.T) {
	got := substituteAssetName(
		"mcp-foo-{version}-{os}-{arch}.tar.gz", "v0.5.0")
	want := fmt.Sprintf("mcp-foo-0.5.0-%s-%s.tar.gz", runtime.GOOS, runtime.GOARCH)
	if got != want {
		t.Errorf("got %q want %q", got, want)
	}
}

// buildTarGz builds an in-memory .tar.gz archive containing one
// file named `member` with the given content, and returns the raw
// bytes. Used by install tests that exercise the extraction path.
func buildTarGz(t *testing.T, member string, content []byte) []byte {
	t.Helper()
	var buf bytes.Buffer
	gz := gzip.NewWriter(&buf)
	tw := tar.NewWriter(gz)

	hdr := &tar.Header{
		Name: member,
		Mode: 0o755,
		Size: int64(len(content)),
	}
	if err := tw.WriteHeader(hdr); err != nil {
		t.Fatal(err)
	}
	if _, err := tw.Write(content); err != nil {
		t.Fatal(err)
	}
	if err := tw.Close(); err != nil {
		t.Fatal(err)
	}
	if err := gz.Close(); err != nil {
		t.Fatal(err)
	}
	return buf.Bytes()
}

func sha256Hex(b []byte) string {
	h := sha256.Sum256(b)
	return hex.EncodeToString(h[:])
}

func TestGitHub_Install_TarGz_HappyPath(t *testing.T) {
	binary := []byte("#!/bin/sh\necho ok\n")
	assetName := "foo-0.5.0-" + runtime.GOOS + "-" + runtime.GOARCH + ".tar.gz"
	archive := buildTarGz(t, "foo", binary)

	release := fmt.Sprintf(`{
		"tag_name": "v0.5.0",
		"assets": [
			{"name": "%s",         "browser_download_url": "https://example.invalid/dl/%s"},
			{"name": "SHA256SUMS", "browser_download_url": "https://example.invalid/dl/SHA256SUMS"}
		]
	}`, assetName, assetName)

	sumLine := sha256Hex(archive) + "  " + assetName + "\n"
	files := map[string][]byte{
		assetName:    archive,
		"SHA256SUMS": []byte(sumLine),
	}
	g := newFakeGitHub(t, release, files)

	dir := t.TempDir()
	dest := filepath.Join(dir, "foo")
	if err := os.WriteFile(dest, []byte("old binary"), 0o755); err != nil {
		t.Fatal(err)
	}

	cfg := &config.Config{
		Name: "foo",
		Source: config.Source{
			Type:            config.SourceGitHub,
			Repo:            "owner/foo",
			Asset:           "foo-{version}-{os}-{arch}.tar.gz",
			BinaryInArchive: "foo",
			ChecksumAsset:   "SHA256SUMS",
		},
	}
	if err := g.Install(context.Background(), cfg, "v0.4.2", dest); err != nil {
		t.Fatalf("Install: %v", err)
	}
	got, err := os.ReadFile(dest)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got, binary) {
		t.Errorf("binary not replaced: got %q want %q", got, binary)
	}
	info, err := os.Stat(dest)
	if err != nil {
		t.Fatal(err)
	}
	if info.Mode().Perm()&0o100 == 0 {
		t.Errorf("executable bit lost: %v", info.Mode())
	}
}

func TestGitHub_Install_PlainBinary(t *testing.T) {
	binary := []byte("binary-contents-v2")
	assetName := "foo-0.5.0-" + runtime.GOOS + "-" + runtime.GOARCH
	release := fmt.Sprintf(`{
		"tag_name": "v0.5.0",
		"assets": [
			{"name": "%s", "browser_download_url": "https://example.invalid/dl/%s"}
		]
	}`, assetName, assetName)
	files := map[string][]byte{assetName: binary}
	g := newFakeGitHub(t, release, files)

	dir := t.TempDir()
	dest := filepath.Join(dir, "foo")
	if err := os.WriteFile(dest, []byte("v1"), 0o755); err != nil {
		t.Fatal(err)
	}
	cfg := &config.Config{
		Name: "foo",
		Source: config.Source{
			Type:  config.SourceGitHub,
			Repo:  "owner/foo",
			Asset: "foo-{version}-{os}-{arch}",
			// No checksum_asset -> skip verification.
		},
	}
	if err := g.Install(context.Background(), cfg, "v0.4.2", dest); err != nil {
		t.Fatalf("Install: %v", err)
	}
	got, _ := os.ReadFile(dest)
	if !bytes.Equal(got, binary) {
		t.Errorf("plain-binary install: %q", got)
	}
}

func TestGitHub_Install_ChecksumMismatch(t *testing.T) {
	binary := []byte("the real binary")
	assetName := "foo-0.5.0-" + runtime.GOOS + "-" + runtime.GOARCH
	release := fmt.Sprintf(`{
		"tag_name": "v0.5.0",
		"assets": [
			{"name": "%s",         "browser_download_url": "https://example.invalid/dl/%s"},
			{"name": "SHA256SUMS", "browser_download_url": "https://example.invalid/dl/SHA256SUMS"}
		]
	}`, assetName, assetName)
	badSum := strings.Repeat("0", 64)
	files := map[string][]byte{
		assetName:    binary,
		"SHA256SUMS": []byte(badSum + "  " + assetName + "\n"),
	}
	g := newFakeGitHub(t, release, files)

	dir := t.TempDir()
	dest := filepath.Join(dir, "foo")
	if err := os.WriteFile(dest, []byte("v1"), 0o755); err != nil {
		t.Fatal(err)
	}
	cfg := &config.Config{
		Name: "foo",
		Source: config.Source{
			Type:          config.SourceGitHub,
			Repo:          "owner/foo",
			Asset:         "foo-{version}-{os}-{arch}",
			ChecksumAsset: "SHA256SUMS",
		},
	}
	err := g.Install(context.Background(), cfg, "v0.4.2", dest)
	if err == nil {
		t.Fatal("expected checksum mismatch")
	}
	if !strings.Contains(err.Error(), "checksum mismatch") {
		t.Errorf("wrong error: %v", err)
	}
	// Destination must be untouched.
	got, _ := os.ReadFile(dest)
	if string(got) != "v1" {
		t.Errorf("destination was clobbered on mismatch: %q", got)
	}
}

func TestGitHub_Install_NoUpgradeWhenSameTag(t *testing.T) {
	release := `{"tag_name":"v0.5.0","assets":[]}`
	g := newFakeGitHub(t, release, nil)

	cfg := &config.Config{
		Name: "foo",
		Source: config.Source{
			Type: config.SourceGitHub,
			Repo: "owner/foo",
		},
	}
	dir := t.TempDir()
	dest := filepath.Join(dir, "foo")
	os.WriteFile(dest, []byte("x"), 0o755)

	err := g.Install(context.Background(), cfg, "0.5.0", dest)
	if err == nil || !strings.Contains(err.Error(), "no upgrade available") {
		t.Errorf("expected no-upgrade error, got %v", err)
	}
}
