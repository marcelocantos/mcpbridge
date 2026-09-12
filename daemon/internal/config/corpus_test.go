// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

package config

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
)

// corpusDir is the shared config-validation corpus at the repo root.
// wrapper/tests/config_test.c reads the same directory and asserts the
// same verdicts, so the two independent validators cannot drift apart
// without turning the build red.
const corpusDir = "../../../testdata/config-validation"

type corpusCase struct {
	File  string `json:"file"`
	Valid bool   `json:"valid"`
	Why   string `json:"why"`
}

// TestSharedCorpus runs every case in testdata/config-validation and
// asserts the daemon's parser agrees with the corpus verdict.
func TestSharedCorpus(t *testing.T) {
	raw, err := os.ReadFile(filepath.Join(corpusDir, "cases.json"))
	if err != nil {
		t.Fatalf("read corpus manifest: %v", err)
	}
	var manifest struct {
		Cases []corpusCase `json:"cases"`
	}
	if err := json.Unmarshal(raw, &manifest); err != nil {
		t.Fatalf("parse corpus manifest: %v", err)
	}
	if len(manifest.Cases) == 0 {
		t.Fatal("corpus manifest has no cases")
	}

	for _, tc := range manifest.Cases {
		t.Run(tc.File, func(t *testing.T) {
			path := filepath.Join(corpusDir, "configs", tc.File)
			data, err := os.ReadFile(path)
			if err != nil {
				t.Fatalf("read fixture: %v", err)
			}
			_, err = ParseBytes(path, data)
			switch {
			case tc.Valid && err != nil:
				t.Errorf("corpus says valid (%s) but daemon rejected it: %v", tc.Why, err)
			case !tc.Valid && err == nil:
				t.Errorf("corpus says invalid (%s) but daemon accepted it", tc.Why)
			}
		})
	}
}
