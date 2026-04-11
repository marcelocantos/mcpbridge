// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

package source

import (
	"archive/tar"
	"compress/gzip"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"time"

	"github.com/marcelocantos/mcpbridge/daemon/internal/config"
)

// GitHub is the upgrade-source backend for MCP servers distributed
// via GitHub releases. It speaks the GitHub REST API for the release
// metadata and downloads assets directly from whatever URL the API
// hands back.
//
// Unlike brew, this backend owns the full fetch -> verify -> install
// sequence in-process. It uses only net/http and stdlib archive
// packages so the daemon can stay dependency-free.
type GitHub struct {
	// apiBase is the root of the GitHub API. Overridable so tests
	// can point at an httptest.NewServer instead of api.github.com.
	apiBase string

	// downloadBase is the root assets are downloaded from. In
	// production it equals "" (the full URL from the API response
	// is used as-is), but tests that stand up their own asset
	// server can set it to rewrite the host of the URL the API
	// returns.
	downloadBase string

	http *http.Client
}

// NewGitHub constructs a backend pointed at the real GitHub API and
// a 30s-per-request http.Client. Adequate for small binaries; the
// caller can swap in a bigger timeout for large archives.
func NewGitHub() *GitHub {
	return &GitHub{
		apiBase: "https://api.github.com",
		http: &http.Client{
			Timeout: 30 * time.Second,
		},
	}
}

// ---------- Metadata ----------

// Asset mirrors the subset of GitHub's release-asset JSON shape we
// actually use.
type Asset struct {
	Name string `json:"name"`
	URL  string `json:"browser_download_url"`
	Size int64  `json:"size"`
}

// LatestInfo is a Go-native view of one /releases/latest response.
type LatestInfo struct {
	TagName string
	Assets  []Asset
}

// Latest fetches the latest release metadata for the given
// owner/repo string (e.g., "marcelocantos/mnemo"). The trailing
// slash is optional. Returns an error on non-2xx responses and on
// decode failures.
func (g *GitHub) Latest(ctx context.Context, repo string) (*LatestInfo, error) {
	if repo == "" {
		return nil, fmt.Errorf("github Latest: empty repo")
	}
	url := fmt.Sprintf("%s/repos/%s/releases/latest", g.apiBase, strings.Trim(repo, "/"))
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("Accept", "application/vnd.github+json")
	req.Header.Set("User-Agent", "mcpbridge-daemon")

	resp, err := g.http.Do(req)
	if err != nil {
		return nil, fmt.Errorf("github Latest: %w", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		body, _ := io.ReadAll(io.LimitReader(resp.Body, 2048))
		return nil, fmt.Errorf("github Latest: %s: %s", resp.Status, strings.TrimSpace(string(body)))
	}

	var parsed struct {
		TagName string  `json:"tag_name"`
		Assets  []Asset `json:"assets"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&parsed); err != nil {
		return nil, fmt.Errorf("github Latest: decode: %w", err)
	}
	return &LatestInfo{TagName: parsed.TagName, Assets: parsed.Assets}, nil
}

// IsNewer returns true when `latest` names a version distinct from
// `installed`. Both tag conventions and bare semver are supported:
// leading "v"s are stripped before the comparison. For v1 we use a
// simple string inequality rather than real semver ordering, which
// means "downgrades" detected after a tag rewrite are also treated
// as upgrades. That's deliberate — the daemon's job is to keep the
// wrapped server at whatever GitHub currently considers the latest,
// not to refuse the rare corrected release.
func (g *GitHub) IsNewer(installed, latest string) bool {
	return strings.TrimPrefix(installed, "v") != strings.TrimPrefix(latest, "v")
}

// ---------- Install ----------

// Install downloads the asset named by cfg.Source.Asset (with
// {version}/{os}/{arch} substituted), optionally verifies its
// SHA256 against cfg.Source.ChecksumAsset, and atomically replaces
// destPath with the new binary. destPath must be writable by the
// user the daemon runs as. The existing file's mode is preserved.
//
// When the asset name ends in .tar.gz, the binary named by
// cfg.Source.BinaryInArchive is extracted from the archive. Bare
// binary assets are installed as-is.
func (g *GitHub) Install(ctx context.Context,
	cfg *config.Config,
	installedVersion string,
	destPath string) error {

	if cfg == nil || cfg.Source.Type != config.SourceGitHub {
		return fmt.Errorf("github Install: not a github source")
	}
	if destPath == "" {
		return fmt.Errorf("github Install: empty destPath")
	}

	latest, err := g.Latest(ctx, cfg.Source.Repo)
	if err != nil {
		return err
	}
	if !g.IsNewer(installedVersion, latest.TagName) {
		return fmt.Errorf("github Install: no upgrade available (installed %q, latest %q)",
			installedVersion, latest.TagName)
	}

	assetName := substituteAssetName(cfg.Source.Asset, latest.TagName)
	asset, ok := findAsset(latest.Assets, assetName)
	if !ok {
		return fmt.Errorf("github Install: asset %q not found in release %s",
			assetName, latest.TagName)
	}

	// Download the asset into a temp file alongside destPath so
	// the atomic rename stays on the same filesystem.
	dir := filepath.Dir(destPath)
	assetFile, err := os.CreateTemp(dir, ".mcpbridge-dl-*")
	if err != nil {
		return fmt.Errorf("github Install: create temp: %w", err)
	}
	tmpAssetPath := assetFile.Name()
	defer os.Remove(tmpAssetPath) // safe if already renamed away

	if err := g.downloadTo(ctx, asset.URL, assetFile); err != nil {
		assetFile.Close()
		return err
	}
	if err := assetFile.Close(); err != nil {
		return fmt.Errorf("github Install: close temp: %w", err)
	}

	// Verify the checksum if requested.
	if cfg.Source.ChecksumAsset != "" {
		sumAsset, ok := findAsset(latest.Assets, cfg.Source.ChecksumAsset)
		if !ok {
			return fmt.Errorf("github Install: checksum asset %q not found",
				cfg.Source.ChecksumAsset)
		}
		wantSum, err := g.fetchChecksumFor(ctx, sumAsset.URL, assetName)
		if err != nil {
			return err
		}
		gotSum, err := sha256File(tmpAssetPath)
		if err != nil {
			return err
		}
		if !strings.EqualFold(wantSum, gotSum) {
			return fmt.Errorf(
				"github Install: checksum mismatch for %s: want %s got %s",
				assetName, wantSum, gotSum)
		}
	}

	// Stage the final binary into place. For plain binaries the
	// downloaded file IS the binary; for .tar.gz we extract the
	// named member into a second temp file next to destPath.
	stagedPath := tmpAssetPath
	if strings.HasSuffix(assetName, ".tar.gz") ||
		strings.HasSuffix(assetName, ".tgz") {
		extracted, err := extractTarGzMember(tmpAssetPath, cfg.Source.BinaryInArchive, dir)
		if err != nil {
			return err
		}
		defer os.Remove(extracted)
		stagedPath = extracted
	}

	// Preserve the existing file's mode (chmod) if we can see it.
	mode := os.FileMode(0o755)
	if info, err := os.Stat(destPath); err == nil {
		mode = info.Mode().Perm()
	}
	if err := os.Chmod(stagedPath, mode); err != nil {
		return fmt.Errorf("github Install: chmod staged: %w", err)
	}

	// Atomic rename. On POSIX this is atomic when source and dest
	// are on the same filesystem, which they are because we
	// created the temp file in filepath.Dir(destPath).
	if err := os.Rename(stagedPath, destPath); err != nil {
		return fmt.Errorf("github Install: rename: %w", err)
	}
	// If we extracted, the earlier Remove(stagedPath) would have
	// failed silently because the file has been moved — that's
	// expected. Clean up the download temp explicitly in case the
	// deferred Remove above races.
	_ = os.Remove(tmpAssetPath)

	return nil
}

// ---------- Helpers ----------

// substituteAssetName fills in the three well-known placeholders in
// the asset pattern from config. We do this in one pass so the
// substitution is order-independent.
func substituteAssetName(pattern, tag string) string {
	version := strings.TrimPrefix(tag, "v")
	r := strings.NewReplacer(
		"{version}", version,
		"{tag}", tag,
		"{os}", runtime.GOOS,
		"{arch}", runtime.GOARCH,
	)
	return r.Replace(pattern)
}

func findAsset(assets []Asset, name string) (Asset, bool) {
	for _, a := range assets {
		if a.Name == name {
			return a, true
		}
	}
	return Asset{}, false
}

// downloadTo streams a URL to w with a context-aware request.
func (g *GitHub) downloadTo(ctx context.Context, url string, w io.Writer) error {
	if g.downloadBase != "" {
		// Test hook: rewrite the host.
		url = g.downloadBase + urlPathOf(url)
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return fmt.Errorf("github download: %w", err)
	}
	req.Header.Set("User-Agent", "mcpbridge-daemon")
	resp, err := g.http.Do(req)
	if err != nil {
		return fmt.Errorf("github download %s: %w", url, err)
	}
	defer resp.Body.Close()
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return fmt.Errorf("github download %s: %s", url, resp.Status)
	}
	if _, err := io.Copy(w, resp.Body); err != nil {
		return fmt.Errorf("github download %s: %w", url, err)
	}
	return nil
}

// fetchChecksumFor downloads a SHA256SUMS-style file and returns
// the hex digest for the named asset. Understands the common
// formats:
//
//	<hex>  <filename>          // two spaces (GNU sha256sum)
//	<hex> *<filename>          // binary marker
//	<hex>  ./<filename>        // leading ./ on some generators
//
// Lines that don't match the requested filename are skipped. Returns
// an error if the file can't be fetched or doesn't contain the name.
func (g *GitHub) fetchChecksumFor(ctx context.Context, url, filename string) (string, error) {
	var buf strings.Builder
	if err := g.downloadTo(ctx, url, &buf); err != nil {
		return "", err
	}
	for _, line := range strings.Split(buf.String(), "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		fields := strings.Fields(line)
		if len(fields) < 2 {
			continue
		}
		hex := fields[0]
		name := strings.TrimPrefix(strings.TrimPrefix(fields[1], "*"), "./")
		if name == filename {
			return hex, nil
		}
	}
	return "", fmt.Errorf("github checksum: no entry for %q in %s", filename, url)
}

// sha256File returns the lowercase hex SHA256 digest of a file.
func sha256File(path string) (string, error) {
	f, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

// extractTarGzMember unpacks exactly one file from a gzip-compressed
// tar archive and writes it to a temp file in dir. Returns the
// temp-file path. Caller owns cleanup.
func extractTarGzMember(archivePath, member, dir string) (string, error) {
	if member == "" {
		return "", fmt.Errorf("github extract: empty binary_in_archive")
	}
	f, err := os.Open(archivePath)
	if err != nil {
		return "", fmt.Errorf("github extract: open %s: %w", archivePath, err)
	}
	defer f.Close()
	gz, err := gzip.NewReader(f)
	if err != nil {
		return "", fmt.Errorf("github extract: gzip: %w", err)
	}
	defer gz.Close()
	tr := tar.NewReader(gz)

	for {
		hdr, err := tr.Next()
		if errors.Is(err, io.EOF) {
			return "", fmt.Errorf("github extract: %s not found in archive", member)
		}
		if err != nil {
			return "", fmt.Errorf("github extract: read: %w", err)
		}
		// Match either the exact name or the basename — tarballs
		// commonly wrap binaries in a versioned subdir.
		if hdr.Name != member && filepath.Base(hdr.Name) != member {
			continue
		}
		out, err := os.CreateTemp(dir, ".mcpbridge-ext-*")
		if err != nil {
			return "", fmt.Errorf("github extract: temp: %w", err)
		}
		if _, err := io.Copy(out, tr); err != nil {
			out.Close()
			os.Remove(out.Name())
			return "", fmt.Errorf("github extract: copy: %w", err)
		}
		if err := out.Close(); err != nil {
			os.Remove(out.Name())
			return "", fmt.Errorf("github extract: close: %w", err)
		}
		return out.Name(), nil
	}
}

// urlPathOf returns the path+query of a URL, dropping scheme/host.
// Used when test hooks want to rewrite only the host of the asset
// download URL.
func urlPathOf(url string) string {
	// Cheap parse — only used for test hooks. Locate the third "/"
	// after "://".
	const sep = "://"
	i := strings.Index(url, sep)
	if i == -1 {
		return url
	}
	rest := url[i+len(sep):]
	j := strings.Index(rest, "/")
	if j == -1 {
		return "/"
	}
	return rest[j:]
}
