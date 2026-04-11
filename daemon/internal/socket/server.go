// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Package socket implements the Unix domain socket server the
// wrapper connects to. The wire format is newline-delimited JSON
// envelopes; see docs/wire-protocol.md.
package socket

import (
	"bufio"
	"context"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net"
	"os"
	"sync"
	"sync/atomic"
)

// MaxLineBytes is the maximum size of one wire-protocol line. Bigger
// than needed for v1 message shapes, but keeps us honest if fields
// grow.
const MaxLineBytes = 256 * 1024

// Version this daemon advertises in its hello_ok replies. Injected
// at build time via -ldflags; falls back to a development marker.
var DaemonVersion = "0.0.0-dev"

// Registration records what a connected wrapper has told us about
// itself. The connection itself is kept so we can push notifications
// back (reload, shutdown).
type Registration struct {
	Name        string
	Pid         int
	ChildPid    int
	ChildBinary string
	conn        *conn
}

// ConfigLookup is the callback the server uses to answer
// register_ok's config_found / polling fields. Given a wrapper's
// registered name, the callback returns whether a matching config
// exists (and thus whether the daemon will actively poll an upgrade
// source for that wrapper's server). Daemon main.go supplies a
// closure backed by internal/config; tests supply stubs.
//
// A nil ConfigLookup is valid: every registration gets config_found
// = false, which matches the pre-T1.11 behaviour.
type ConfigLookup func(name string) (found bool, polling bool)

// Server listens on a Unix domain socket and accepts wrapper
// connections. It is safe for concurrent use: all public methods
// take the internal mutex.
type Server struct {
	sockPath string
	ln       net.Listener
	lookup   ConfigLookup

	mu     sync.Mutex
	conns  map[uint64]*conn // keyed by internal connection id
	nextID uint64
}

// NewServer creates a server bound to the given socket path. The
// parent directory is created with mode 0700 and any stale socket at
// that path is unlinked before binding. `lookup` may be nil, in
// which case every register_ok reports config_found=false.
func NewServer(sockPath string, lookup ConfigLookup) (*Server, error) {
	if _, err := EnsureSocketDir(sockPath); err != nil {
		return nil, err
	}
	// Remove a stale socket from a previous run. If it is still in
	// use by a live daemon, the bind below will fail and the user
	// can investigate.
	_ = os.Remove(sockPath)

	ln, err := net.Listen("unix", sockPath)
	if err != nil {
		return nil, fmt.Errorf("listen %s: %w", sockPath, err)
	}
	if err := os.Chmod(sockPath, 0o600); err != nil {
		ln.Close()
		return nil, fmt.Errorf("chmod %s: %w", sockPath, err)
	}
	return &Server{
		sockPath: sockPath,
		ln:       ln,
		lookup:   lookup,
		conns:    make(map[uint64]*conn),
	}, nil
}

// SocketPath returns the filesystem path the server is listening on.
func (s *Server) SocketPath() string { return s.sockPath }

// Run accepts connections until ctx is cancelled or the listener is
// closed. It never returns an error from a clean shutdown; all errors
// mean the listener is broken.
func (s *Server) Run(ctx context.Context) error {
	// Close the listener when ctx is cancelled so Accept returns.
	go func() {
		<-ctx.Done()
		s.ln.Close()
	}()

	for {
		c, err := s.ln.Accept()
		if err != nil {
			if errors.Is(err, net.ErrClosed) || ctx.Err() != nil {
				return nil
			}
			return fmt.Errorf("accept: %w", err)
		}
		s.startConn(c)
	}
}

// Close unregisters everything, tears down all live connections, and
// unlinks the socket file. Safe to call multiple times.
func (s *Server) Close() error {
	_ = s.ln.Close()

	s.mu.Lock()
	conns := s.conns
	s.conns = map[uint64]*conn{}
	s.mu.Unlock()

	for _, c := range conns {
		c.close()
	}
	_ = os.Remove(s.sockPath)
	return nil
}

// Registrations returns a snapshot of every currently registered
// wrapper, in no particular order. Used for diagnostics and tests.
func (s *Server) Registrations() []Registration {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make([]Registration, 0, len(s.conns))
	for _, c := range s.conns {
		if c.reg != nil {
			out = append(out, *c.reg)
		}
	}
	return out
}

// BroadcastReload pushes a reload envelope to every registered
// wrapper with the given reason. Used by SIGHUP as a "reload
// everything" knob.
func (s *Server) BroadcastReload(reason string) {
	s.mu.Lock()
	conns := make([]*conn, 0, len(s.conns))
	for _, c := range s.conns {
		if c.reg != nil {
			conns = append(conns, c)
		}
	}
	s.mu.Unlock()

	for _, c := range conns {
		env := &Envelope{
			Type:   TypeReload,
			Seq:    c.nextSeq(),
			Name:   c.reg.Name,
			Reason: reason,
		}
		if err := c.send(env); err != nil {
			slog.Warn("broadcast reload: send failed",
				"name", c.reg.Name, "err", err)
		}
	}
}

// ReloadName sends a reload envelope only to the wrappers currently
// registered with the given name. Used by the scheduler when one
// specific config has detected a new version. Returns the number
// of wrappers that were notified (0 when no wrapper is registered
// for that name — the scheduler treats that as "no one cares").
func (s *Server) ReloadName(name, reason string) int {
	s.mu.Lock()
	matches := make([]*conn, 0, 2)
	for _, c := range s.conns {
		if c.reg != nil && c.reg.Name == name {
			matches = append(matches, c)
		}
	}
	s.mu.Unlock()

	for _, c := range matches {
		env := &Envelope{
			Type:   TypeReload,
			Seq:    c.nextSeq(),
			Name:   name,
			Reason: reason,
		}
		if err := c.send(env); err != nil {
			slog.Warn("reload: send failed", "name", name, "err", err)
		}
	}
	return len(matches)
}

// ChildBinaryForName returns the child_binary path reported by a
// wrapper registered with the given name, or "" if no wrapper is
// currently registered. The scheduler uses this to know where to
// write a freshly downloaded GitHub release asset. If multiple
// wrappers are registered for the same name (unusual but legal),
// the first one encountered wins.
func (s *Server) ChildBinaryForName(name string) string {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, c := range s.conns {
		if c.reg != nil && c.reg.Name == name {
			return c.reg.ChildBinary
		}
	}
	return ""
}

// BroadcastShutdown pushes a shutdown envelope to every wrapper and
// then closes their connections. Called at daemon exit.
func (s *Server) BroadcastShutdown(reason string) {
	s.mu.Lock()
	conns := make([]*conn, 0, len(s.conns))
	for _, c := range s.conns {
		conns = append(conns, c)
	}
	s.mu.Unlock()

	for _, c := range conns {
		env := &Envelope{
			Type:   TypeShutdown,
			Seq:    c.nextSeq(),
			Reason: reason,
		}
		_ = c.send(env) // best-effort
	}
}

// ---------- per-connection ----------

type conn struct {
	id     uint64
	server *Server
	raw    net.Conn
	br     *bufio.Reader
	bw     *bufio.Writer

	writeMu sync.Mutex // serializes writes (multiple goroutines may send)
	seq     atomic.Uint64

	reg *Registration // non-nil after successful register
}

func (s *Server) startConn(raw net.Conn) {
	s.mu.Lock()
	s.nextID++
	id := s.nextID
	c := &conn{
		id:     id,
		server: s,
		raw:    raw,
		br:     bufio.NewReaderSize(raw, 32*1024),
		bw:     bufio.NewWriter(raw),
	}
	s.conns[id] = c
	s.mu.Unlock()

	go c.serve()
}

func (c *conn) nextSeq() uint64 {
	return c.seq.Add(1)
}

func (c *conn) serve() {
	defer c.cleanup()

	// Read the hello first. If anything goes wrong before we have a
	// registered wrapper, log and drop.
	hello, err := c.readEnvelope()
	if err != nil {
		slog.Warn("conn: hello read failed", "id", c.id, "err", err)
		return
	}
	if hello.Type != TypeHello {
		c.sendError("bad_envelope",
			fmt.Sprintf("expected hello, got %s", hello.Type))
		return
	}
	// Reply with hello_ok.
	if err := c.send(&Envelope{
		Type:          TypeHelloOK,
		Seq:           c.nextSeq(),
		DaemonVersion: DaemonVersion,
		AckSeq:        hello.Seq,
	}); err != nil {
		slog.Warn("conn: hello_ok send failed", "id", c.id, "err", err)
		return
	}
	slog.Info("conn: hello",
		"id", c.id,
		"wrapper_version", hello.WrapperVersion,
		"pid", hello.Pid)

	// Next should be register. Anything else is a protocol error.
	reg, err := c.readEnvelope()
	if err != nil {
		slog.Warn("conn: register read failed", "id", c.id, "err", err)
		return
	}
	if reg.Type != TypeRegister {
		c.sendError("bad_envelope",
			fmt.Sprintf("expected register, got %s", reg.Type))
		return
	}
	c.reg = &Registration{
		Name:        reg.Name,
		Pid:         hello.Pid,
		ChildPid:    reg.ChildPid,
		ChildBinary: reg.ChildBinary,
		conn:        c,
	}
	var configFound, polling bool
	if c.server.lookup != nil {
		configFound, polling = c.server.lookup(reg.Name)
	}
	if err := c.send(&Envelope{
		Type:        TypeRegisterOK,
		Seq:         c.nextSeq(),
		AckSeq:      reg.Seq,
		ConfigFound: configFound,
		Polling:     polling,
	}); err != nil {
		slog.Warn("conn: register_ok send failed", "id", c.id, "err", err)
		return
	}
	slog.Info("conn: registered",
		"id", c.id,
		"name", c.reg.Name,
		"child_pid", c.reg.ChildPid,
		"child_binary", c.reg.ChildBinary)

	// Operational loop: the wrapper may send deregister, reload_ack,
	// or error. Anything else is logged and ignored.
	for {
		env, err := c.readEnvelope()
		if err != nil {
			if errors.Is(err, io.EOF) {
				slog.Info("conn: wrapper closed", "id", c.id, "name", c.reg.Name)
			} else {
				slog.Info("conn: read ended", "id", c.id, "err", err)
			}
			return
		}
		switch env.Type {
		case TypeDeregister:
			slog.Info("conn: deregister", "id", c.id, "reason", env.Reason)
			return
		case TypeReloadAck:
			slog.Info("conn: reload_ack",
				"id", c.id,
				"status", env.Status,
				"detail", env.Detail)
		case TypeError:
			slog.Warn("conn: wrapper reported error",
				"id", c.id,
				"code", env.Code,
				"detail", env.Detail)
		default:
			slog.Warn("conn: unexpected message from wrapper",
				"id", c.id, "type", env.Type)
		}
	}
}

// readEnvelope reads one line and decodes it. Enforces the line cap.
func (c *conn) readEnvelope() (*Envelope, error) {
	line, err := c.br.ReadSlice('\n')
	if errors.Is(err, bufio.ErrBufferFull) {
		return nil, fmt.Errorf("line exceeds %d bytes", MaxLineBytes)
	}
	if err != nil {
		return nil, err
	}
	// ReadSlice keeps the bytes in the bufio buffer; copy before the
	// next read invalidates them.
	payload := make([]byte, len(line)-1) // strip '\n'
	copy(payload, line[:len(line)-1])
	return Unmarshal(payload)
}

func (c *conn) send(env *Envelope) error {
	data, err := env.Marshal()
	if err != nil {
		return err
	}
	c.writeMu.Lock()
	defer c.writeMu.Unlock()
	if _, err := c.bw.Write(data); err != nil {
		return err
	}
	if err := c.bw.WriteByte('\n'); err != nil {
		return err
	}
	return c.bw.Flush()
}

func (c *conn) sendError(code, detail string) {
	_ = c.send(&Envelope{
		Type:   TypeError,
		Seq:    c.nextSeq(),
		Code:   code,
		Detail: detail,
	})
}

func (c *conn) close() {
	_ = c.raw.Close()
}

func (c *conn) cleanup() {
	c.server.mu.Lock()
	delete(c.server.conns, c.id)
	c.server.mu.Unlock()
	_ = c.raw.Close()
}
