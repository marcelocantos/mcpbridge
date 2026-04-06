// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

package mcpbridge_test

import (
	"bufio"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"time"

	"github.com/marcelocantos/mcpbridge"
)

// TestMain builds the dummyserver binary once for all e2e tests.
func TestMain(m *testing.M) {
	// Build the dummyserver binary.
	cmd := exec.Command("go", "build", "-o", "testdata/dummyserver/dummyserver", "./testdata/dummyserver/")
	cmd.Stderr = os.Stderr
	if err := cmd.Run(); err != nil {
		fmt.Fprintf(os.Stderr, "failed to build dummyserver: %v\n", err)
		os.Exit(1)
	}
	os.Exit(m.Run())
}

// daemon starts a dummyserver daemon and returns a cleanup function.
func daemon(t *testing.T, sockPath, tools string) *exec.Cmd {
	t.Helper()
	args := []string{"--socket", sockPath, "--tools", tools, "serve"}
	cmd := exec.Command("./testdata/dummyserver/dummyserver", args...)
	cmd.Stderr = os.Stderr
	if err := cmd.Start(); err != nil {
		t.Fatalf("start daemon: %v", err)
	}
	t.Cleanup(func() {
		cmd.Process.Kill()
		cmd.Wait()
	})
	// Wait for socket to appear.
	for i := 0; i < 50; i++ {
		if _, err := os.Stat(sockPath); err == nil {
			return cmd
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatal("daemon socket did not appear")
	return nil
}

// mcpClient wraps a dummyserver proxy process, speaking MCP JSON-RPC
// over its stdin/stdout.
type mcpClient struct {
	cmd    *exec.Cmd
	stdin  io.WriteCloser
	stdout *bufio.Scanner
	nextID int
}

func newMCPClient(t *testing.T, sockPath string) *mcpClient {
	t.Helper()
	cmd := exec.Command("./testdata/dummyserver/dummyserver", "--socket", sockPath)
	stdin, err := cmd.StdinPipe()
	if err != nil {
		t.Fatal(err)
	}
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		t.Fatal(err)
	}
	cmd.Stderr = os.Stderr
	if err := cmd.Start(); err != nil {
		t.Fatalf("start proxy: %v", err)
	}
	t.Cleanup(func() {
		stdin.Close()
		cmd.Process.Kill()
		cmd.Wait()
	})

	c := &mcpClient{
		cmd:    cmd,
		stdin:  stdin,
		stdout: bufio.NewScanner(stdout),
	}
	c.stdout.Buffer(make([]byte, 1<<20), 1<<20)

	// Initialize the MCP session.
	resp := c.request(t, "initialize", map[string]any{
		"protocolVersion": "2024-11-05",
		"capabilities":    map[string]any{},
		"clientInfo":      map[string]any{"name": "test", "version": "1.0"},
	})
	if _, ok := resp["result"]; !ok {
		t.Fatalf("initialize failed: %v", resp)
	}

	// Send initialized notification.
	c.notify(t, "notifications/initialized", nil)

	return c
}

func (c *mcpClient) request(t *testing.T, method string, params any) map[string]any {
	t.Helper()
	c.nextID++
	msg := map[string]any{
		"jsonrpc": "2.0",
		"id":      c.nextID,
		"method":  method,
	}
	if params != nil {
		msg["params"] = params
	}
	b, _ := json.Marshal(msg)
	if _, err := fmt.Fprintf(c.stdin, "%s\n", b); err != nil {
		t.Fatalf("write to proxy: %v", err)
	}

	// Read response lines, skipping notifications (no "id" field).
	for {
		if !c.stdout.Scan() {
			t.Fatalf("proxy closed stdout (method=%s)", method)
		}
		var resp map[string]any
		if err := json.Unmarshal(c.stdout.Bytes(), &resp); err != nil {
			t.Fatalf("decode response: %v (raw: %s)", err, c.stdout.Text())
		}
		// Skip notifications (they have no "id").
		if _, hasID := resp["id"]; hasID {
			return resp
		}
	}
}

func (c *mcpClient) notify(t *testing.T, method string, params any) {
	t.Helper()
	msg := map[string]any{
		"jsonrpc": "2.0",
		"method":  method,
	}
	if params != nil {
		msg["params"] = params
	}
	b, _ := json.Marshal(msg)
	fmt.Fprintf(c.stdin, "%s\n", b)
}

func (c *mcpClient) listTools(t *testing.T) []string {
	t.Helper()
	resp := c.request(t, "tools/list", nil)
	result, ok := resp["result"].(map[string]any)
	if !ok {
		t.Fatalf("tools/list: no result: %v", resp)
	}
	toolsRaw, ok := result["tools"].([]any)
	if !ok {
		t.Fatalf("tools/list: no tools array: %v", result)
	}
	var names []string
	for _, tr := range toolsRaw {
		tool, ok := tr.(map[string]any)
		if !ok {
			continue
		}
		if name, ok := tool["name"].(string); ok {
			names = append(names, name)
		}
	}
	return names
}

func (c *mcpClient) callTool(t *testing.T, name string, args map[string]any) (string, bool) {
	t.Helper()
	params := map[string]any{"name": name}
	if args != nil {
		params["arguments"] = args
	}
	resp := c.request(t, "tools/call", params)
	result, ok := resp["result"].(map[string]any)
	if !ok {
		t.Fatalf("tools/call %s: no result: %v", name, resp)
	}
	isError, _ := result["isError"].(bool)
	content, _ := result["content"].([]any)
	if len(content) == 0 {
		t.Fatalf("tools/call %s: empty content", name)
	}
	first, _ := content[0].(map[string]any)
	text, _ := first["text"].(string)
	return text, isError
}

// --- E2E Tests ---

func TestE2EBasic(t *testing.T) {
	sockPath := filepath.Join(t.TempDir(), "test.sock")
	daemon(t, sockPath, "greet,echo")

	c := newMCPClient(t, sockPath)

	// List tools.
	tools := c.listTools(t)
	if len(tools) != 2 {
		t.Fatalf("expected 2 tools, got %v", tools)
	}
	toolSet := map[string]bool{}
	for _, name := range tools {
		toolSet[name] = true
	}
	if !toolSet["greet"] || !toolSet["echo"] {
		t.Fatalf("expected greet and echo, got %v", tools)
	}

	// Call greet.
	text, isErr := c.callTool(t, "greet", map[string]any{"name": "alice"})
	if isErr {
		t.Fatalf("greet returned error: %s", text)
	}
	if text != "hello, alice!" {
		t.Fatalf("expected 'hello, alice!', got %q", text)
	}

	// Call echo.
	text, isErr = c.callTool(t, "echo", map[string]any{"message": "ping"})
	if isErr {
		t.Fatalf("echo returned error: %s", text)
	}
	if text != "ping" {
		t.Fatalf("expected 'ping', got %q", text)
	}
}

func TestE2EToolError(t *testing.T) {
	sockPath := filepath.Join(t.TempDir(), "test.sock")
	daemon(t, sockPath, "greet,fail")

	c := newMCPClient(t, sockPath)

	text, isErr := c.callTool(t, "fail", nil)
	if !isErr {
		t.Fatal("expected isError=true")
	}
	if text != "intentional failure" {
		t.Fatalf("expected 'intentional failure', got %q", text)
	}
}

func TestE2EDifferentToolSets(t *testing.T) {
	// Test that different tool flags produce different tool lists.
	sockPath := filepath.Join(t.TempDir(), "test.sock")

	// Start with greet only.
	cmd := daemon(t, sockPath, "greet")
	c := newMCPClient(t, sockPath)

	tools := c.listTools(t)
	if len(tools) != 1 {
		t.Fatalf("expected 1 tool, got %v", tools)
	}

	// Kill daemon, restart with all four tools.
	cmd.Process.Kill()
	cmd.Wait()
	time.Sleep(50 * time.Millisecond)
	os.Remove(sockPath)

	daemon(t, sockPath, "greet,echo,fail,upper")

	// New client (old proxy process is dead, need fresh one).
	c2 := newMCPClient(t, sockPath)
	tools = c2.listTools(t)
	if len(tools) != 4 {
		t.Fatalf("expected 4 tools, got %v", tools)
	}

	// Verify the new tool works.
	text, isErr := c2.callTool(t, "upper", map[string]any{"text": "hello"})
	if isErr {
		t.Fatalf("upper returned error: %s", text)
	}
	if text != "HELLO" {
		t.Fatalf("expected 'HELLO', got %q", text)
	}
}

func TestE2EUpgradeReconnect(t *testing.T) {
	// Test that a proxy using mcpbridge's auto-reconnect survives a
	// daemon restart. We can't test this through the stdio proxy binary
	// (it creates one connection at startup), but we can test the
	// RPC client directly against two sequential daemons.
	sockPath := filepath.Join(t.TempDir(), "test.sock")

	// Start daemon v1 with greet only.
	cmd := daemon(t, sockPath, "greet")

	// Connect RPC client directly (not through stdio proxy).
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	var client *mcpbridge.Client
	for {
		var err error
		client, err = mcpbridge.Dial(sockPath)
		if err == nil {
			break
		}
		if ctx.Err() != nil {
			t.Fatalf("failed to connect: %v", err)
		}
		time.Sleep(20 * time.Millisecond)
	}
	defer client.Close()
	proxy := mcpbridge.NewToolProxy(client)

	// Verify v1.
	tools, err := proxy.ListTools()
	if err != nil {
		t.Fatal(err)
	}
	if len(tools) != 1 {
		t.Fatalf("v1: expected 1 tool, got %d", len(tools))
	}

	// Kill daemon, restart with more tools.
	cmd.Process.Kill()
	cmd.Wait()
	time.Sleep(50 * time.Millisecond)
	os.Remove(sockPath)

	daemon(t, sockPath, "greet,echo,upper")

	// Same client should auto-reconnect.
	tools, err = proxy.ListTools()
	if err != nil {
		t.Fatalf("reconnect listTools failed: %v", err)
	}
	if len(tools) != 3 {
		t.Fatalf("v2: expected 3 tools, got %d", len(tools))
	}

	// Call a tool to verify full round-trip after reconnect.
	result, err := proxy.CallTool("upper", map[string]any{"text": "test"})
	if err != nil {
		t.Fatalf("callTool after reconnect: %v", err)
	}
	if result.IsError {
		t.Fatalf("upper returned error: %s", result.Text)
	}
	if result.Text != "TEST" {
		t.Fatalf("expected 'TEST', got %q", result.Text)
	}
}

func TestE2EPerformance(t *testing.T) {
	sockPath := filepath.Join(t.TempDir(), "test.sock")
	daemon(t, sockPath, "greet,echo")

	c := newMCPClient(t, sockPath)
	const maxLatency = 500 * time.Millisecond // generous for process-level e2e

	start := time.Now()
	c.listTools(t)
	if dur := time.Since(start); dur > maxLatency {
		t.Errorf("tools/list took %v (max %v)", dur, maxLatency)
	}

	start = time.Now()
	c.callTool(t, "greet", map[string]any{"name": "perf"})
	if dur := time.Since(start); dur > maxLatency {
		t.Errorf("tools/call took %v (max %v)", dur, maxLatency)
	}
}

