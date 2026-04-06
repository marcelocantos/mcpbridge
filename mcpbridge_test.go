// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

package mcpbridge

import (
	"encoding/json"
	"fmt"
	"path/filepath"
	"testing"
	"time"

	"github.com/mark3labs/mcp-go/mcp"
)

// testHandler is a trivial tool handler for testing.
type testHandler struct{}

func (h testHandler) Call(name string, args map[string]any) (string, bool, error) {
	switch name {
	case "greet":
		who, _ := args["name"].(string)
		if who == "" {
			who = "world"
		}
		return fmt.Sprintf("hello, %s!", who), false, nil
	case "fail":
		return "something went wrong", true, nil
	default:
		return "", false, fmt.Errorf("unknown tool: %s", name)
	}
}

func testTools() []mcp.Tool {
	return []mcp.Tool{
		mcp.NewTool("greet",
			mcp.WithDescription("Say hello"),
			mcp.WithString("name", mcp.Description("Who to greet")),
		),
		mcp.NewTool("fail",
			mcp.WithDescription("Always fails"),
		),
	}
}

func setup(t *testing.T, opts ...ServerOption) (*Client, *ToolProxy) {
	t.Helper()
	sockPath := filepath.Join(t.TempDir(), "test.sock")
	srv, err := NewServer(DaemonConfig{
		SocketPath: sockPath,
		Tools:      testTools(),
		Handler:    testHandler{},
	}, opts...)
	if err != nil {
		t.Fatal(err)
	}
	go srv.Serve()
	t.Cleanup(func() { srv.Close() })

	client, err := Dial(sockPath)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { client.Close() })

	return client, NewToolProxy(client)
}

func TestListTools(t *testing.T) {
	_, proxy := setup(t)
	tools, err := proxy.ListTools()
	if err != nil {
		t.Fatal(err)
	}
	if len(tools) != 2 {
		t.Fatalf("expected 2 tools, got %d", len(tools))
	}
	if tools[0].Name != "greet" {
		t.Fatalf("expected 'greet', got %q", tools[0].Name)
	}
}

func TestCallTool(t *testing.T) {
	_, proxy := setup(t)

	result, err := proxy.CallTool("greet", map[string]any{"name": "alice"})
	if err != nil {
		t.Fatal(err)
	}
	if result.IsError {
		t.Fatalf("unexpected error: %s", result.Text)
	}
	if result.Text != "hello, alice!" {
		t.Fatalf("expected 'hello, alice!', got %q", result.Text)
	}
}

func TestCallToolError(t *testing.T) {
	_, proxy := setup(t)

	result, err := proxy.CallTool("fail", nil)
	if err != nil {
		t.Fatal(err)
	}
	if !result.IsError {
		t.Fatal("expected isError=true")
	}
	if result.Text != "something went wrong" {
		t.Fatalf("expected 'something went wrong', got %q", result.Text)
	}
}

func TestAutoReconnect(t *testing.T) {
	sockPath := filepath.Join(t.TempDir(), "test.sock")

	// Start server v1.
	srv1, err := NewServer(DaemonConfig{
		SocketPath: sockPath,
		Tools:      testTools(),
		Handler:    testHandler{},
	})
	if err != nil {
		t.Fatal(err)
	}
	go srv1.Serve()

	client, err := Dial(sockPath)
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()
	proxy := NewToolProxy(client)

	// Verify v1 works.
	result, err := proxy.CallTool("greet", map[string]any{"name": "v1"})
	if err != nil {
		t.Fatal(err)
	}
	if result.Text != "hello, v1!" {
		t.Fatalf("v1: expected 'hello, v1!', got %q", result.Text)
	}

	// Kill v1, start v2 with an extra tool.
	srv1.Close()
	time.Sleep(50 * time.Millisecond)

	extraTool := mcp.NewTool("extra",
		mcp.WithDescription("Added in v2"),
		mcp.WithString("x", mcp.Description("dummy")),
	)
	v2Tools := append(testTools(), extraTool)

	srv2, err := NewServer(DaemonConfig{
		SocketPath: sockPath,
		Tools:      v2Tools,
		Handler:    testHandler{},
	})
	if err != nil {
		t.Fatal(err)
	}
	go srv2.Serve()
	defer srv2.Close()

	// Same client — should auto-reconnect.
	result, err = proxy.CallTool("greet", map[string]any{"name": "v2"})
	if err != nil {
		t.Fatalf("reconnect failed: %v", err)
	}
	if result.Text != "hello, v2!" {
		t.Fatalf("v2: expected 'hello, v2!', got %q", result.Text)
	}

	// Verify v2 has the extra tool.
	tools, err := proxy.ListTools()
	if err != nil {
		t.Fatal(err)
	}
	if len(tools) != 3 {
		t.Fatalf("expected 3 tools after upgrade, got %d", len(tools))
	}
}

func TestExtraMethods(t *testing.T) {
	sockPath := filepath.Join(t.TempDir(), "test.sock")
	srv, err := NewServer(DaemonConfig{
		SocketPath: sockPath,
		Tools:      testTools(),
		Handler:    testHandler{},
		ExtraMethods: map[string]MethodFunc{
			"Ping": func(params json.RawMessage) (any, error) {
				return map[string]string{"pong": "ok"}, nil
			},
		},
	})
	if err != nil {
		t.Fatal(err)
	}
	go srv.Serve()
	defer srv.Close()

	client, err := Dial(sockPath)
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()

	raw, err := client.Call("Ping", nil)
	if err != nil {
		t.Fatal(err)
	}
	var result map[string]string
	if err := json.Unmarshal(raw, &result); err != nil {
		t.Fatal(err)
	}
	if result["pong"] != "ok" {
		t.Fatalf("expected pong=ok, got %v", result)
	}
}

func TestPerformance(t *testing.T) {
	_, proxy := setup(t)
	const maxLatency = 200 * time.Millisecond

	ops := []struct {
		name string
		fn   func() error
	}{
		{"ListTools", func() error { _, err := proxy.ListTools(); return err }},
		{"CallTool", func() error { _, err := proxy.CallTool("greet", nil); return err }},
	}

	for _, op := range ops {
		t.Run(op.name, func(t *testing.T) {
			start := time.Now()
			if err := op.fn(); err != nil {
				t.Fatal(err)
			}
			if dur := time.Since(start); dur > maxLatency {
				t.Errorf("%s took %v (max %v)", op.name, dur, maxLatency)
			}
		})
	}
}
