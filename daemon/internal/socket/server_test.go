// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

package socket

import (
	"bufio"
	"context"
	"net"
	"os"
	"path/filepath"
	"testing"
	"time"
)

// shortTempDir returns a short temporary directory suitable for
// holding a Unix domain socket. macOS caps sun_path at 104 bytes,
// and Go's default t.TempDir() lives under /var/folders/... which is
// already too long before we even add a filename. We use /tmp
// directly and clean up via t.Cleanup.
func shortTempDir(t *testing.T) string {
	t.Helper()
	dir, err := os.MkdirTemp("/tmp", "mcpb-*")
	if err != nil {
		t.Fatalf("MkdirTemp: %v", err)
	}
	t.Cleanup(func() { _ = os.RemoveAll(dir) })
	return dir
}

// dialAndHandshake opens a client connection and completes the
// hello/register exchange. Returns the reader/writer for subsequent
// operations plus the register_ok envelope.
func dialAndHandshake(t *testing.T, sockPath, name string) (net.Conn, *bufio.Reader, *Envelope) {
	t.Helper()

	// Give the listener a moment to bind if the test just started
	// the server.
	var c net.Conn
	var err error
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		c, err = net.Dial("unix", sockPath)
		if err == nil {
			break
		}
		time.Sleep(10 * time.Millisecond)
	}
	if err != nil {
		t.Fatalf("dial %s: %v", sockPath, err)
	}

	br := bufio.NewReader(c)
	send := func(e *Envelope) {
		t.Helper()
		data, err := e.Marshal()
		if err != nil {
			t.Fatalf("marshal: %v", err)
		}
		if _, err := c.Write(append(data, '\n')); err != nil {
			t.Fatalf("write: %v", err)
		}
	}
	recv := func() *Envelope {
		t.Helper()
		line, err := br.ReadBytes('\n')
		if err != nil {
			t.Fatalf("read: %v", err)
		}
		env, err := Unmarshal(line[:len(line)-1])
		if err != nil {
			t.Fatalf("unmarshal: %v", err)
		}
		return env
	}

	send(&Envelope{Type: TypeHello, Seq: 1, WrapperVersion: "test", Pid: 1})
	helloOK := recv()
	if helloOK.Type != TypeHelloOK {
		t.Fatalf("expected hello_ok, got %v", helloOK.Type)
	}

	send(&Envelope{
		Type: TypeRegister, Seq: 2,
		Name: name, ChildPid: 2, ChildBinary: "/bin/true",
	})
	registerOK := recv()
	if registerOK.Type != TypeRegisterOK {
		t.Fatalf("expected register_ok, got %v", registerOK.Type)
	}

	return c, br, registerOK
}

func newServer(t *testing.T) (*Server, func()) {
	t.Helper()
	dir := shortTempDir(t)
	sockPath := filepath.Join(dir, "d.sock")
	srv, err := NewServer(sockPath)
	if err != nil {
		t.Fatalf("NewServer: %v", err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() {
		_ = srv.Run(ctx)
		close(done)
	}()
	return srv, func() {
		cancel()
		srv.Close()
		<-done
	}
}

func TestServer_HandshakeAndRegistration(t *testing.T) {
	srv, cleanup := newServer(t)
	defer cleanup()

	conn, _, _ := dialAndHandshake(t, srv.SocketPath(), "mnemo")
	defer conn.Close()

	// Give the server a beat to record the registration.
	deadline := time.Now().Add(1 * time.Second)
	for time.Now().Before(deadline) {
		regs := srv.Registrations()
		if len(regs) == 1 && regs[0].Name == "mnemo" {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatalf("registration not recorded: %+v", srv.Registrations())
}

func TestServer_BroadcastReload(t *testing.T) {
	srv, cleanup := newServer(t)
	defer cleanup()

	conn, br, _ := dialAndHandshake(t, srv.SocketPath(), "mnemo")
	defer conn.Close()

	// Wait until the server has actually recorded our registration
	// so the broadcast has someone to hit.
	deadline := time.Now().Add(1 * time.Second)
	for time.Now().Before(deadline) {
		if len(srv.Registrations()) == 1 {
			break
		}
		time.Sleep(10 * time.Millisecond)
	}

	srv.BroadcastReload("manual")

	// Expect a reload envelope on the client socket.
	_ = conn.SetReadDeadline(time.Now().Add(1 * time.Second))
	line, err := br.ReadBytes('\n')
	if err != nil {
		t.Fatalf("read reload: %v", err)
	}
	env, err := Unmarshal(line[:len(line)-1])
	if err != nil {
		t.Fatalf("unmarshal reload: %v", err)
	}
	if env.Type != TypeReload {
		t.Errorf("got %v, want reload", env.Type)
	}
	if env.Name != "mnemo" {
		t.Errorf("got name %q", env.Name)
	}
	if env.Reason != "manual" {
		t.Errorf("got reason %q", env.Reason)
	}
}

func TestServer_DeregisterRemovesRegistration(t *testing.T) {
	srv, cleanup := newServer(t)
	defer cleanup()

	conn, _, _ := dialAndHandshake(t, srv.SocketPath(), "foo")

	// Wait for registration.
	deadline := time.Now().Add(1 * time.Second)
	for time.Now().Before(deadline) {
		if len(srv.Registrations()) == 1 {
			break
		}
		time.Sleep(10 * time.Millisecond)
	}

	// Send deregister and close.
	env := &Envelope{Type: TypeDeregister, Seq: 3, Reason: "upstream_closed"}
	data, _ := env.Marshal()
	_, _ = conn.Write(append(data, '\n'))
	_ = conn.Close()

	// The server should drop the registration.
	deadline = time.Now().Add(1 * time.Second)
	for time.Now().Before(deadline) {
		if len(srv.Registrations()) == 0 {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatalf("registration still present: %+v", srv.Registrations())
}

func TestServer_RejectsHelloWithBadVersion(t *testing.T) {
	srv, cleanup := newServer(t)
	defer cleanup()

	c, err := net.Dial("unix", srv.SocketPath())
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer c.Close()

	// Send a hello with v=99. Unmarshal on the server side will
	// reject it, the connection will be closed, and we should see
	// EOF.
	_, _ = c.Write([]byte(`{"t":"hello","v":99,"seq":1}` + "\n"))

	_ = c.SetReadDeadline(time.Now().Add(1 * time.Second))
	buf := make([]byte, 64)
	n, err := c.Read(buf)
	// Either EOF or a successful read of zero bytes indicates the
	// server dropped us.
	if err == nil && n > 0 {
		t.Errorf("expected EOF after bad version, got %q", buf[:n])
	}
}
