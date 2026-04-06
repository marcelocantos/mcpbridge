// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// dummyserver is a minimal MCP server built on mcpbridge for e2e testing.
// It supports both daemon and stdio proxy modes, with configurable tools.
//
// Usage:
//
//	dummyserver serve --socket /path/to/sock [--tools greet,echo,fail]
//	dummyserver --socket /path/to/sock
package main

import (
	"context"
	"flag"
	"fmt"
	"log/slog"
	"os"
	"strings"

	"github.com/mark3labs/mcp-go/mcp"
	"github.com/marcelocantos/mcpbridge"
)

func main() {
	socketPath := flag.String("socket", "", "Unix domain socket path (required)")
	toolList := flag.String("tools", "greet,echo", "Comma-separated list of tools to advertise: greet, echo, fail, upper")
	flag.Parse()

	if *socketPath == "" {
		fmt.Fprintln(os.Stderr, "error: --socket is required")
		os.Exit(1)
	}

	args := flag.Args()
	if len(args) > 0 && args[0] == "serve" {
		runServe(*socketPath, *toolList)
	} else {
		runProxy(*socketPath)
	}
}

func runServe(socketPath, toolList string) {
	slog.SetDefault(slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{
		Level: slog.LevelInfo,
	})))

	tools := buildTools(toolList)

	srv, err := mcpbridge.NewServer(mcpbridge.DaemonConfig{
		SocketPath: socketPath,
		Tools:      tools,
		Handler:    &handler{},
	})
	if err != nil {
		slog.Error("failed to start server", "err", err)
		os.Exit(1)
	}
	defer srv.Close()

	slog.Info("dummyserver daemon starting", "socket", socketPath, "tools", toolList)
	if err := srv.Serve(); err != nil {
		slog.Error("server failed", "err", err)
		os.Exit(1)
	}
}

func runProxy(socketPath string) {
	slog.SetDefault(slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{
		Level: slog.LevelWarn,
	})))

	if err := mcpbridge.RunProxy(context.Background(), mcpbridge.ProxyConfig{
		SocketPath: socketPath,
		ServerName: "dummyserver",
		Version:    "1.0.0",
	}); err != nil {
		fmt.Fprintf(os.Stderr, "dummyserver: %v\n", err)
		os.Exit(1)
	}
}

func buildTools(toolList string) []mcp.Tool {
	var tools []mcp.Tool
	for _, name := range strings.Split(toolList, ",") {
		name = strings.TrimSpace(name)
		switch name {
		case "greet":
			tools = append(tools, mcp.NewTool("greet",
				mcp.WithDescription("Say hello"),
				mcp.WithString("name", mcp.Description("Who to greet (default: world)")),
			))
		case "echo":
			tools = append(tools, mcp.NewTool("echo",
				mcp.WithDescription("Echo back the input"),
				mcp.WithString("message", mcp.Required(), mcp.Description("Message to echo")),
			))
		case "fail":
			tools = append(tools, mcp.NewTool("fail",
				mcp.WithDescription("Always returns an error"),
			))
		case "upper":
			tools = append(tools, mcp.NewTool("upper",
				mcp.WithDescription("Convert to uppercase"),
				mcp.WithString("text", mcp.Required(), mcp.Description("Text to uppercase")),
			))
		default:
			fmt.Fprintf(os.Stderr, "unknown tool: %s\n", name)
			os.Exit(1)
		}
	}
	return tools
}

type handler struct{}

func (h *handler) Call(name string, args map[string]any) (string, bool, error) {
	switch name {
	case "greet":
		who, _ := args["name"].(string)
		if who == "" {
			who = "world"
		}
		return fmt.Sprintf("hello, %s!", who), false, nil
	case "echo":
		msg, _ := args["message"].(string)
		return msg, false, nil
	case "fail":
		return "intentional failure", true, nil
	case "upper":
		text, _ := args["text"].(string)
		return strings.ToUpper(text), false, nil
	default:
		return "", false, fmt.Errorf("unknown tool: %s", name)
	}
}
