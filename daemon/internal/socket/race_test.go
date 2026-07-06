// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

package socket

import (
	"bufio"
	"net"
	"sync"
	"testing"
	"time"
)

// TestServer_ConnRegNoRaceUnderPush exercises the c.reg publication
// path (Fable-5 F3 / T15). serve() publishes each conn into s.conns
// (under s.mu) and only later writes c.reg; the push methods
// (BroadcastReload / ReloadName / ChildBinaryForName / Registrations)
// read c.reg under s.mu on other goroutines. If c.reg is written
// without s.mu, `go test -race` reports a data race between the
// unlocked write and the locked reads.
//
// This test is only meaningful under -race; without it the race is
// silent (which is exactly why it shipped). The daemon test job runs
// with -race (see Makefile daemon-test).
func TestServer_ConnRegNoRaceUnderPush(t *testing.T) {
	srv, cleanup := newServer(t)
	defer cleanup()
	sock := srv.SocketPath()

	done := make(chan struct{})

	// Hammer every push method that reads c.reg, concurrently with the
	// registrations below.
	var pushWG sync.WaitGroup
	pushWG.Add(1)
	go func() {
		defer pushWG.Done()
		for {
			select {
			case <-done:
				return
			default:
			}
			srv.BroadcastReload("race")
			_ = srv.Registrations()
			_ = srv.ChildBinaryForName("mnemo")
			srv.ReloadName("mnemo", "race")
		}
	}()

	// Many clients register concurrently. Each holds its connection
	// open (until done) so the pushers keep iterating live conns.
	const clients = 40
	var registered sync.WaitGroup
	registered.Add(clients)
	var held sync.WaitGroup
	held.Add(clients)
	for i := 0; i < clients; i++ {
		go func() {
			defer held.Done()
			c, err := net.Dial("unix", sock)
			if err != nil {
				registered.Done()
				return
			}
			defer c.Close()
			br := bufio.NewReader(c)
			writeEnv := func(e *Envelope) bool {
				data, err := e.Marshal()
				if err != nil {
					return false
				}
				_, err = c.Write(append(data, '\n'))
				return err == nil
			}
			readLine := func() bool {
				_, err := br.ReadBytes('\n')
				return err == nil
			}
			ok := writeEnv(&Envelope{Type: TypeHello, Seq: 1, WrapperVersion: "race", Pid: 1}) &&
				readLine() // hello_ok
			// Widen the window between the conn landing in s.conns and
			// c.reg being written, which is where the race lives.
			time.Sleep(time.Millisecond)
			ok = ok &&
				writeEnv(&Envelope{Type: TypeRegister, Seq: 2, Name: "mnemo", ChildPid: 2, ChildBinary: "/bin/true"}) &&
				readLine() // register_ok
			_ = ok
			registered.Done()
			<-done
		}()
	}

	registered.Wait()
	// Let the pushers race against the settled registrations too.
	time.Sleep(30 * time.Millisecond)
	close(done)
	held.Wait()
	pushWG.Wait()
}
