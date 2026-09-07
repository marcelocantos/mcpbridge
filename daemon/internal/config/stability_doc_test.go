// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

package config

import (
	"os"
	"path/filepath"
	"reflect"
	"regexp"
	"strings"
	"testing"
)

// STABILITY.md is the public 1.0 contract: the one document an
// outside consumer is told to build against. It went stale when the
// v0.8.0 scheduler retirement deleted the brew/github upgrade
// backends but left their env vars and config fields sitting in the
// table marked **stable**. Prose cannot notice that; these tests can.
//
// The rule enforced here: every environment variable and every
// config field the contract names must exist in shipped source, or
// the line must say "retired".

const (
	repoRoot     = "../../.."
	stabilityDoc = "STABILITY.md"
	// retiredMarker exempts a row that documents a field only so
	// old configs keep loading. Such a row is a compatibility note,
	// not a promise, and has no counterpart in source by design.
	retiredMarker = "retired"
)

// wrapperOnlyFields are config fields the C wrapper parses and the Go
// Config struct deliberately does not carry (the daemon never talks
// to an upstream, so it has no use for them). Named explicitly so
// "absent from the Go struct" cannot silently mean "absent from the
// product".
var wrapperOnlyFields = map[string]string{
	"tool_call_timeout_ms": "wrapper/src/config.h",
}

func readStabilityDoc(t *testing.T) []string {
	t.Helper()
	raw, err := os.ReadFile(filepath.Join(repoRoot, stabilityDoc))
	if err != nil {
		t.Fatalf("read %s: %v", stabilityDoc, err)
	}
	return strings.Split(string(raw), "\n")
}

// shippedSource concatenates every non-test Go, C, and header source
// file in the repo — what the product actually is.
func shippedSource(t *testing.T) string {
	t.Helper()
	var b strings.Builder
	for _, dir := range []string{"daemon", "wrapper/src"} {
		root := filepath.Join(repoRoot, dir)
		err := filepath.Walk(root, func(path string, info os.FileInfo, err error) error {
			if err != nil {
				return err
			}
			if info.IsDir() {
				return nil
			}
			name := info.Name()
			if strings.HasSuffix(name, "_test.go") {
				return nil
			}
			switch filepath.Ext(name) {
			case ".go", ".c", ".h":
			default:
				return nil
			}
			data, readErr := os.ReadFile(path)
			if readErr != nil {
				return readErr
			}
			b.Write(data)
			b.WriteString("\n")
			return nil
		})
		if err != nil {
			t.Fatalf("walk %s: %v", root, err)
		}
	}
	return b.String()
}

var envVarPattern = regexp.MustCompile(`MCPBRIDGE_[A-Z0-9_]+`)

// TestStabilityDocEnvVarsExist fails when the contract promises an
// environment variable no shipped binary reads.
func TestStabilityDocEnvVarsExist(t *testing.T) {
	source := shippedSource(t)
	for i, line := range readStabilityDoc(t) {
		if strings.Contains(strings.ToLower(line), retiredMarker) {
			continue
		}
		for _, name := range envVarPattern.FindAllString(line, -1) {
			if !strings.Contains(source, name) {
				t.Errorf("%s:%d promises %s but no shipped source reads it; "+
					"delete the row or mark it retired",
					stabilityDoc, i+1, name)
			}
		}
	}
}

// tableRow matches a markdown table row whose first cell is a
// backticked identifier, e.g. "| `check_interval` | Go duration | ...".
var tableRow = regexp.MustCompile("^\\|\\s*`([a-z_][a-z0-9_]*)`\\s*\\|")

// TestStabilityDocConfigFieldsExist fails when the config-schema
// table in STABILITY.md names a field that neither the Go Config
// struct nor the wrapper carries. This is the check that would have
// caught source/upgrade/check_interval surviving the v0.8.0
// scheduler retirement.
func TestStabilityDocConfigFieldsExist(t *testing.T) {
	known := map[string]bool{}
	ct := reflect.TypeOf(Config{})
	for i := 0; i < ct.NumField(); i++ {
		tag := ct.Field(i).Tag.Get("json")
		name := strings.Split(tag, ",")[0]
		if name != "" && name != "-" {
			known[name] = true
		}
	}
	for name := range wrapperOnlyFields {
		known[name] = true
	}

	lines := readStabilityDoc(t)
	start, end := -1, -1
	for i, line := range lines {
		if strings.HasPrefix(line, "### Config schema v2") {
			start = i
			continue
		}
		if start >= 0 && strings.HasPrefix(line, "### ") {
			end = i
			break
		}
	}
	if start < 0 {
		t.Fatalf("%s: no \"### Config schema v2\" section found", stabilityDoc)
	}
	if end < 0 {
		end = len(lines)
	}

	found := 0
	for i := start; i < end; i++ {
		m := tableRow.FindStringSubmatch(lines[i])
		if m == nil {
			continue
		}
		found++
		field := m[1]
		if known[field] {
			continue
		}
		if strings.Contains(strings.ToLower(lines[i]), retiredMarker) {
			continue
		}
		t.Errorf("%s:%d documents config field %q, which is in neither the Go "+
			"Config struct nor wrapperOnlyFields; delete the row or mark it retired",
			stabilityDoc, i+1, field)
	}
	if found == 0 {
		t.Errorf("%s: config schema table parsed to zero rows — the check is "+
			"no longer looking at anything", stabilityDoc)
	}
}
