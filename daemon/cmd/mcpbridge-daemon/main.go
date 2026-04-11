// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// mcpbridge-daemon is the long-lived upgrade poller for the mcpbridge
// wrapper. Exactly one instance runs per user session (typically under
// `brew services`), polls configured MCP server sources for new
// versions, performs the upgrades, and notifies connected wrappers to
// cycle their children.
//
// This build is a stub — CLI handling only. Real functionality lands
// in later sub-targets; see docs/targets.yaml.
package main

import (
	"flag"
	"fmt"
	"os"
)

// Version is injected at build time via -ldflags "-X main.Version=...".
var Version = "0.0.0-dev"

const usageText = `Usage: mcpbridge-daemon [OPTIONS]

The mcpbridge upgrade daemon. Runs one per user session (typically
under brew services) and notifies connected wrappers when a new version
of a wrapped MCP server is available.

Options:
  --socket PATH    override the UDS path (default: user-local)
  --config-dir DIR override the config search directory
  -v, --verbose    extra logging
  --version        print version and exit
  --help           print this help and exit
`

func main() {
	fs := flag.NewFlagSet("mcpbridge-daemon", flag.ContinueOnError)
	fs.SetOutput(os.Stderr)
	fs.Usage = func() { fmt.Fprint(os.Stderr, usageText) }

	var (
		showVersion bool
		showHelp    bool
	)
	fs.BoolVar(&showVersion, "version", false, "print version and exit")
	fs.BoolVar(&showHelp, "help", false, "print help and exit")
	fs.BoolVar(&showHelp, "h", false, "print help and exit")

	// Parse with the help/version flags recognised early so we don't
	// trip over unknown long-form options that later sub-targets will
	// add.
	if err := fs.Parse(os.Args[1:]); err != nil {
		os.Exit(2)
	}

	if showVersion {
		fmt.Println(Version)
		return
	}
	if showHelp {
		fmt.Fprint(os.Stdout, usageText)
		return
	}

	fmt.Fprintln(os.Stderr, "mcpbridge-daemon: not yet implemented (see docs/targets.yaml)")
	os.Exit(69) // EX_UNAVAILABLE
}
