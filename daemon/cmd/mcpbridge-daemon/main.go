// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// mcpbridge-daemon is the long-lived upgrade poller for the mcpbridge
// wrapper. Exactly one instance runs per user session (typically under
// `brew services`), polls configured MCP server sources for new
// versions, performs the upgrades, and notifies connected wrappers to
// cycle their children.
//
// This build lands the UDS server and the hello/register/reload
// handshake. Source backends (brew, github) and the polling scheduler
// come in later targets. SIGHUP broadcasts a manual reload to every
// currently registered wrapper — useful both as a "check now" knob
// and as the easiest way to test the reload path end to end.
package main

import (
	"bytes"
	"context"
	_ "embed"
	"flag"
	"fmt"
	"log/slog"
	"os"
	"os/signal"
	"path/filepath"
	"syscall"

	"github.com/marcelocantos/mcpbridge/daemon/internal/config"
	"github.com/marcelocantos/mcpbridge/daemon/internal/scheduler"
	"github.com/marcelocantos/mcpbridge/daemon/internal/socket"
	"github.com/marcelocantos/mcpbridge/daemon/internal/source"
	"github.com/marcelocantos/mcpbridge/daemon/internal/watcher"
)

//go:embed help_agent.md
var helpAgentMD string

// Version is injected at build time via -ldflags "-X main.Version=...".
var Version = "0.0.0-dev"

// resolveConfigDirs returns the directories the daemon should scan
// for per-server JSON configs, in precedence order. The $MCPBRIDGE_CONFIG_DIR
// env var overrides everything (used by tests); otherwise the user
// config directory + the system share dir are searched.
func resolveConfigDirs() []string {
	if override := os.Getenv("MCPBRIDGE_CONFIG_DIR"); override != "" {
		return []string{override}
	}
	home, err := os.UserHomeDir()
	if err != nil {
		home = "/tmp"
	}
	userDir := filepath.Join(home, ".config", "mcpbridge")

	// System share dir: traditional /usr/local/share on macOS and
	// most Linux distros; Homebrew adds /opt/homebrew/share on
	// Apple Silicon. Walk both unconditionally — missing dirs are
	// skipped by config.Load.
	return []string{
		userDir,
		"/opt/homebrew/share/mcpbridge",
		"/usr/local/share/mcpbridge",
	}
}

const usageText = `Usage: mcpbridge-daemon [OPTIONS]

The mcpbridge upgrade daemon. Runs one per user session (typically
under brew services) and notifies connected wrappers when a new
version of a wrapped MCP server is available.

Options:
  --socket PATH    override the UDS path (default: platform-specific)
  -v, --verbose    extra logging
  --version        print version and exit
  --help           print this help and exit
  --help-agent     print help followed by the mcpbridge agent guide

Signals:
  SIGHUP           broadcast a manual reload to all connected wrappers
  SIGINT, SIGTERM  clean shutdown
`

func main() {
	fs := flag.NewFlagSet("mcpbridge-daemon", flag.ContinueOnError)
	fs.SetOutput(os.Stderr)
	fs.Usage = func() { fmt.Fprint(os.Stderr, usageText) }

	var (
		showVersion    bool
		showHelp       bool
		showHelpAgent  bool
		verbose        bool
		sockPath       string
	)
	fs.BoolVar(&showVersion, "version", false, "print version and exit")
	fs.BoolVar(&showHelp, "help", false, "print help and exit")
	fs.BoolVar(&showHelp, "h", false, "print help and exit")
	fs.BoolVar(&showHelpAgent, "help-agent", false, "print agent-oriented help and exit")
	fs.BoolVar(&verbose, "verbose", false, "verbose logging")
	fs.BoolVar(&verbose, "v", false, "verbose logging")
	fs.StringVar(&sockPath, "socket", "", "override socket path")

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
	if showHelpAgent {
		// Print the usage text followed by the embedded agent
		// guide so a coding agent gets both the CLI flag
		// reference and the full install + troubleshooting
		// guide in one call.
		var buf bytes.Buffer
		buf.WriteString(usageText)
		buf.WriteString("\n")
		buf.WriteString(helpAgentMD)
		fmt.Fprint(os.Stdout, buf.String())
		return
	}

	level := slog.LevelInfo
	if verbose {
		level = slog.LevelDebug
	}
	slog.SetDefault(slog.New(slog.NewTextHandler(os.Stderr,
		&slog.HandlerOptions{Level: level})))

	// Make the daemon's version visible in hello_ok replies.
	socket.DaemonVersion = Version

	if sockPath == "" {
		p, err := socket.DefaultSocketPath()
		if err != nil {
			slog.Error("resolve socket path", "err", err)
			os.Exit(1)
		}
		sockPath = p
	}

	// Load per-server config files. ~/.config/mcpbridge/ takes
	// precedence over $prefix/share/mcpbridge/; both are discovered
	// silently if missing.
	configDirs := resolveConfigDirs()
	cfgLoad := config.Load(configDirs...)
	for _, err := range cfgLoad.Errors {
		slog.Warn("config: parse error", "err", err)
	}
	slog.Info("config: loaded",
		"count", len(cfgLoad.Configs),
		"dirs", configDirs)

	lookup := func(name string) (bool, bool) {
		cfg, ok := cfgLoad.Configs[name]
		if !ok {
			return false, false
		}
		// Polling is enabled when the config exists AND its upgrade
		// mode isn't "off". The source backends (T1.12) will
		// actually consult this to decide whether to schedule a
		// poll; for now it's just reported back to the wrapper.
		return true, cfg.Upgrade != config.UpgradeOff
	}

	srv, err := socket.NewServer(sockPath, lookup)
	if err != nil {
		slog.Error("start server", "err", err)
		os.Exit(1)
	}
	slog.Info("listening", "path", srv.SocketPath(), "version", Version)

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	// Watcher: fsnotify on each registered wrapper's child_binary.
	// Detects out-of-band upgrades (user-driven brew upgrade etc.)
	// and broadcasts a targeted reload. Registrations flow through
	// the socket server's RegistrationHandler callback.
	wch, err := watcher.New(srv)
	if err != nil {
		slog.Error("start watcher", "err", err)
		os.Exit(1)
	}
	srv.SetRegistrationHandler(wch)
	go wch.Run(ctx)
	defer wch.Close()

	// Scheduler: one goroutine per config, polls on interval, uses
	// the brew and github source backends, and broadcasts targeted
	// reloads through the socket server.
	sched := scheduler.New(
		cfgLoad.Configs,
		source.NewBrew(),
		source.NewGitHub(),
		srv,
	)
	go sched.Run(ctx)

	// Signal routing. SIGHUP triggers a manual reload broadcast;
	// SIGINT/SIGTERM cancels the context which unblocks Run.
	sigs := make(chan os.Signal, 4)
	signal.Notify(sigs, syscall.SIGHUP, syscall.SIGINT, syscall.SIGTERM)
	go func() {
		for sig := range sigs {
			switch sig {
			case syscall.SIGHUP:
				slog.Info("SIGHUP: broadcasting reload")
				srv.BroadcastReload("manual")
			case syscall.SIGINT, syscall.SIGTERM:
				slog.Info("signal: shutting down", "sig", sig.String())
				srv.BroadcastShutdown("service_stop")
				cancel()
				return
			}
		}
	}()

	if err := srv.Run(ctx); err != nil {
		slog.Error("server loop", "err", err)
		os.Exit(1)
	}
	srv.Close()
	slog.Info("stopped")
}
