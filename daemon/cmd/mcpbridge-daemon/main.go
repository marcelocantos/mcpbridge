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
	"context"
	"flag"
	"fmt"
	"log/slog"
	"os"
	"os/signal"
	"syscall"

	"github.com/marcelocantos/mcpbridge/daemon/internal/socket"
)

// Version is injected at build time via -ldflags "-X main.Version=...".
var Version = "0.0.0-dev"

const usageText = `Usage: mcpbridge-daemon [OPTIONS]

The mcpbridge upgrade daemon. Runs one per user session (typically
under brew services) and notifies connected wrappers when a new
version of a wrapped MCP server is available.

Options:
  --socket PATH    override the UDS path (default: platform-specific)
  -v, --verbose    extra logging
  --version        print version and exit
  --help           print this help and exit

Signals:
  SIGHUP           broadcast a manual reload to all connected wrappers
  SIGINT, SIGTERM  clean shutdown
`

func main() {
	fs := flag.NewFlagSet("mcpbridge-daemon", flag.ContinueOnError)
	fs.SetOutput(os.Stderr)
	fs.Usage = func() { fmt.Fprint(os.Stderr, usageText) }

	var (
		showVersion bool
		showHelp    bool
		verbose     bool
		sockPath    string
	)
	fs.BoolVar(&showVersion, "version", false, "print version and exit")
	fs.BoolVar(&showHelp, "help", false, "print help and exit")
	fs.BoolVar(&showHelp, "h", false, "print help and exit")
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

	srv, err := socket.NewServer(sockPath)
	if err != nil {
		slog.Error("start server", "err", err)
		os.Exit(1)
	}
	slog.Info("listening", "path", srv.SocketPath(), "version", Version)

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

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
