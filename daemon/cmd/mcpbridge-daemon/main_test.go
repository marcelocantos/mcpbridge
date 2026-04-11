// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

package main

import "testing"

// TestVersionDefined guards against someone zeroing out the Version
// constant without thinking about it. The value is replaced at build
// time via -ldflags; the default must remain a recognisable
// placeholder.
func TestVersionDefined(t *testing.T) {
	if Version == "" {
		t.Fatal("Version must not be empty")
	}
}
