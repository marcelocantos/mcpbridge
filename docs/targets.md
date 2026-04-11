# Targets

## Active

### 🎯T1 A generic C wrapper transparently proxies any stdio MCP server with automatic upgrade detection and execution, keeping the agent's MCP session alive across child restarts.
- **Value**: 8
- **Cost**: 13
- **Acceptance**:
  - Any stdio MCP server invocation can be wrapped by prepending `mcpbridge` (with `--config NAME` when the binary name is ambiguous) with no other changes required
  - Upstream agent observes a continuous MCP session across child restarts — no visible disconnect, cached `initialize` replayed, `notifications/tools/list_changed` emitted when the tool list changes
  - In-flight requests are never interrupted by an upgrade — state machine drains to zero before any install/swap action runs
  - Active upgrade detection works for at least `brew`, `github`, and `cmd` source types, driven by per-server JSON configs discovered in ~/.config/mcpbridge/ and $prefix/share/mcpbridge/
  - Passive upgrade detection via fswatch (kqueue on macOS, inotify on Linux) catches out-of-band upgrades and triggers an immediate child cycle
  - Default upgrade mode is `notify` (detect + log + cycle on out-of-band change, but never auto-install); `auto` is opt-in per config
  - Child lifecycle is driven by an explicit state machine with a pure `fsm_step(state, event) -> (new_state, actions)` function unit-tested without real child processes
  - Code is C11, POSIX.1-2008, compiles with a hand-written Makefile, depends on nothing beyond libc and vendored cJSON, and shells out for HTTP/package-manager actions rather than linking libcurl or similar
  - macOS arm64 and Linux x86_64/arm64 supported; Windows deferred
- **Context**: Today mcpbridge is a Go library for building daemon+proxy MCP servers (used by mnemo). That solves daemon-side auto-upgrade but leaves stdio MCP servers unaddressed. The new direction is a generic binary `mcpbridge` (written in C for long-term zero-maintenance longevity) that a user prepends to any MCP server invocation in their client config. It speaks MCP to the agent on stdio, spawns the wrapped server as a child, forwards MCP messages protocol-aware (so it can cache `initialize` and replay on child restart), detects when a new version of the wrapped server is available (active polling primary, fswatch fallback for user-initiated upgrades), drains in-flight requests, runs the upgrade action, and cycles the child — all invisible to the upstream agent. Per-server upgrade metadata lives in JSON config files (shipped by the server author or handcrafted locally) discovered from ~/.config/mcpbridge/ and $prefix/share/mcpbridge/. Core correctness comes from an explicit child-lifecycle state machine (STARTING/RUNNING/DRAINING/UPGRADING/SWAPPING/RESPAWN/FAILED) driven by events from the upstream reader, child reader, signals, timers, and the upgrade detector. Language choice is locked to C11 + POSIX.1-2008 + plain Makefile + vendored cJSON + shell-out to curl/brew/sha256sum for external actions — chosen explicitly so the code compiles unchanged for as long as humanly possible. The existing Go library either stays under a `go/` subdirectory or moves to its own repo (decision deferred to planning).
- **Depends on**: 🎯T1.1, 🎯T1.2, 🎯T1.3, 🎯T1.4, 🎯T1.5, 🎯T1.6, 🎯T1.7, 🎯T1.6.1, 🎯T1.8, 🎯T1.9, 🎯T1.10, 🎯T1.11, 🎯T1.12, 🎯T1.13, 🎯T1.14, 🎯T1.15
- **Status**: Identified
- **Discovered**: 2026-04-11

## Achieved

### 🎯T1.11 The daemon loads per-server JSON config files from ~/.config/mcpbridge and wires config lookups into the socket server so register_ok reports config_found=true when a matching name exists.
- **Value**: 5
- **Cost**: 5
- **Acceptance**:
  - daemon/internal/config/config.go defines a Config struct matching the schema (schema int, name string, source discriminated union on type, upgrade enum off|notify|auto, check_interval Go duration string)
  - Supports source types brew (formula) and github (repo, asset, binary_in_archive, checksum_asset). Unknown source types and schema versions are rejected with a descriptive error including the file path
  - config.Load(dirs ...) walks each listed directory, reads *.json files, parses and validates each, returns a map[string]*Config keyed by name and a slice of per-file errors (so one bad file doesn't break discovery of the rest)
  - If two files in the same or different directories define the same name, the earlier directory wins and a warning is logged
  - daemon/internal/config/config_test.go covers: a valid brew config, a valid github config, rejection of a bad schema version, rejection of an unknown source type, rejection of malformed JSON, and discovery of multiple configs across two directories
  - daemon main.go calls config.Load with the user's ~/.config/mcpbridge/ directory at startup and passes a lookup closure into the socket server
  - socket.Server.NewServer grows an optional ConfigLookup parameter; register_ok's config_found is populated from the lookup
  - Existing daemon tests updated to use a stub lookup so no regressions
- **Context**: First piece of daemon config loading. The wire protocol already reserves config_found in register_ok, but until now the daemon always returned false because there was no loader. This target lands a small config package (parse, validate, discover), plus the integration with the socket server so wrappers get the truthful answer. Source backends (brew, github) and the polling scheduler come in later targets; this one only gets to "the daemon knows which servers have configs."
- **Depends on**: 🎯T1.8
- **Status**: Achieved
- **Discovered**: 2026-04-12
- **Achieved**: 2026-04-12

### 🎯T1.12 The daemon has a brew upgrade source backend (daemon/internal/source/brew.go) that can detect when a wrapped formula is outdated and execute the upgrade, backed by `brew outdated --json=v2` and `brew upgrade`.
- **Value**: 5
- **Cost**: 3
- **Acceptance**:
  - daemon/internal/source/brew.go defines a Brew struct with Outdated(ctx, formula) (*OutdatedInfo, error) and Upgrade(ctx, formula) error methods
  - OutdatedInfo carries the installed_version and current_version strings as parsed from `brew outdated --json=v2`; if the formula is not outdated the method returns (nil, nil)
  - The exec side is behind a function-pointer seam so tests can inject a fake brew binary without touching PATH or the host's real brew
  - brew_test.go covers: outdated response with the formula present, outdated response with the formula absent (-> nil), malformed JSON (-> error), brew exit nonzero (-> error), and the upgrade happy path
  - The package doc comment names the backing commands and notes that standard-out is parsed as JSON while standard-error is surfaced in error messages
- **Context**: First real source backend. Pure library for now — it exposes Outdated and Upgrade methods that take a formula name and shell out to the real brew binary. The scheduler that actually calls them (on a timer) and the fsnotify integration for out-of-band detection come in T1.14. Github source lands separately in T1.13 because it needs HTTPS + download + SHA256 machinery that has nothing in common with the brew path.
- **Depends on**: 🎯T1.11
- **Status**: Achieved
- **Discovered**: 2026-04-12
- **Achieved**: 2026-04-12

### 🎯T1.13 The daemon has a GitHub releases source backend that fetches /releases/latest over HTTPS, downloads and SHA256-verifies the release asset, and atomically replaces the wrapped binary at its installed path.
- **Value**: 5
- **Cost**: 5
- **Acceptance**:
  - daemon/internal/source/github.go defines a GitHub struct with Latest(ctx, repo) returning tag name + assets, IsNewer(installed, latest), and Install(ctx, cfg, installedVersion, destPath)
  - Install substitutes {version}, {os}, {arch} in the asset pattern, downloads over HTTPS with a per-request timeout, verifies the SHA256 when the config names a checksum_asset, optionally unpacks a single binary out of a .tar.gz, and atomically renames into destPath (preserving the existing file's mode + ownership)
  - HTTP side is injectable so tests can stand up an httptest.Server and point the backend at it instead of api.github.com
  - github_test.go covers: latest-release JSON parsing, IsNewer table-driven cases, asset template substitution, plain-binary install happy path, checksum mismatch rejection, tar.gz extraction happy path
  - No third-party dependencies — uses only net/http, encoding/json, archive/tar, compress/gzip, crypto/sha256
- **Context**: Second source backend. Unlike brew, github hits the network directly from Go — api.github.com for the release metadata and github.com for asset downloads. Supports plain-binary assets and .tar.gz archives (single-file extraction). Tests use httptest.NewServer to stand up a fake GitHub API so the test suite never touches the real network.
- **Depends on**: 🎯T1.11
- **Status**: Achieved
- **Discovered**: 2026-04-12
- **Achieved**: 2026-04-12

### 🎯T1.14 The daemon has a polling scheduler that calls every configured source backend on its own check_interval, triggers the install path when a new version is detected (in auto mode), and broadcasts a targeted reload to the right wrapper.
- **Value**: 8
- **Cost**: 5
- **Acceptance**:
  - daemon/internal/scheduler/scheduler.go defines a Scheduler with Run(ctx) and per-config poll loops
  - Brew path: calls Outdated; if a new version exists AND upgrade mode is auto, calls Upgrade; then broadcasts a targeted reload; in notify mode logs and does not broadcast
  - GitHub path: caches the last-seen tag so first-run establishes baseline without spurious upgrades; when the tag changes AND mode is auto, calls Install with the registered wrapper's child_binary as destPath; broadcasts a targeted reload
  - socket.Server grows ReloadName(name, reason) int that only sends reload envelopes to wrappers registered with that exact name; returns the count notified
  - socket.Server grows ChildBinaryForName(name) string so the scheduler can resolve where to install github-sourced binaries
  - Source backends are accepted via interfaces so scheduler_test.go can drive them with stubs; no network, no brew, no files touched in tests
  - scheduler_test.go covers: brew auto upgrade broadcasts, brew notify-only does not broadcast, github first-run baselines without action, github second-run with new tag broadcasts, broadcast skipped when no wrapper is registered for the name, and clean shutdown on context cancel
  - daemon main.go constructs and runs the scheduler in a goroutine after the socket server starts, and stops cleanly on SIGINT/SIGTERM
- **Context**: Glue layer that makes T1.11 (configs) + T1.12 (brew) + T1.13 (github) actually do something. One goroutine per config, ticks on cfg.CheckInterval, uses interface seams so tests can inject fake sources and fake broadcasters. Adds a targeted ReloadName method to socket.Server so the scheduler only reloads the wrappers for the upgraded config, not every connected wrapper.
- **Depends on**: 🎯T1.11, 🎯T1.12, 🎯T1.13
- **Status**: Achieved
- **Discovered**: 2026-04-12
- **Achieved**: 2026-04-12

### 🎯T1.15 The daemon watches each registered wrapper's child_binary path via fsnotify and broadcasts a targeted reload when the file changes out-of-band (e.g. a user-initiated brew upgrade).
- **Value**: 5
- **Cost**: 5
- **Acceptance**:
  - daemon/internal/watcher/watcher.go defines a Watcher with Start(ctx), Track(name, path), Untrack(name), and a Broadcaster seam for the reload push
  - Uses github.com/fsnotify/fsnotify (added to daemon/go.mod + go.sum) — no other new deps
  - Watches the parent directory of each tracked path and matches events against exact basenames so unrelated files in the same dir are ignored
  - Debounces rapid event bursts (e.g. create -> chmod -> rename) with a short coalescing window so one brew upgrade produces exactly one reload broadcast
  - socket.Server grows a RegistrationHandler callback so the scheduler/watcher can react to register / deregister events; the existing tests keep working because the callback is optional (nil = ignored)
  - watcher_test.go touches a real file in t.TempDir() and asserts ReloadName is called exactly once with reason='binary_changed' via a stub Broadcaster
  - daemon main.go constructs the watcher, passes its OnRegister/OnDeregister callback into NewServer, and runs the watcher alongside the scheduler
- **Context**: Complements the scheduler: the scheduler catches upgrades we drove ourselves, the watcher catches upgrades the user drove via their own tooling. Uses fsnotify to watch the parent directory of each registered binary (fsnotify doesn't reliably catch writes to a single file), filters events to tracked names, and calls ReloadName on match. First third-party dependency in the daemon.
- **Depends on**: 🎯T1.14
- **Status**: Achieved
- **Discovered**: 2026-04-12
- **Achieved**: 2026-04-12

### 🎯T1.1 Repo is reshaped for the wrapper+daemon split: Go library deleted; wrapper/ (C) builds a stub mcpbridge binary; daemon/ (Go) builds a stub mcpbridge-daemon binary; top-level Makefile orchestrates both.
- **Value**: 5
- **Cost**: 3
- **Acceptance**:
  - All old Go library files deleted (mcpbridge.go, proxy.go, server.go, client.go, e2e_test.go, mcpbridge_test.go, go.mod, go.sum, testdata/)
  - Top-level Makefile orchestrates both subdirs; `make` builds both the C wrapper and the Go daemon
  - wrapper/ contains the C scaffolding (Makefile, src/main.c, src/log.*, src/util.*, tests/smoke_test.c, vendor/cjson/)
  - wrapper/ `make` produces a stub mcpbridge binary that responds to --version / --help / --help-agent
  - wrapper/ `make test` passes
  - daemon/ contains a Go module (go.mod) with cmd/mcpbridge-daemon/main.go producing a stub that responds to --version / --help
  - daemon/ `go test ./...` passes
  - top-level `make test` runs both test suites
  - .gitignore covers C and Go build artifacts
  - README.md explains the two-process model at a high level
- **Context**: First sub-target of 🎯T1. After the strategy pivot, mcpbridge is split into two processes: a C stdio wrapper (per MCP server) and a Go daemon (one per user session, run under brew services). This target establishes the two-subdirectory layout and proves both build. Real behavior lands in later sub-targets.
- **Status**: Achieved
- **Discovered**: 2026-04-11
- **Achieved**: 2026-04-11

### 🎯T1.10 Wrapper handles the full reload cycle: daemon-client integrated into the event loop, RELOAD drives DRAINING -> SWAPPING -> child respawn -> cached-initialize replay -> RUNNING -> reload_ack to daemon. Session stays alive across the child cycle.
- **Value**: 13
- **Cost**: 8
- **Acceptance**:
  - Dispatch caches the upstream initialize request and the notifications/initialized notification on first sighting, and has a dispatch_replay_initialize() that sends them to the current child via the sink
  - Dispatch tracks a replay_pending flag so the initialize response arriving from a replayed initialize is consumed by the wrapper (not forwarded upstream) and emits INITIALIZE_OK to transition the FSM
  - main.c dials the daemon at startup using a platform-default socket path (env MCPBRIDGE_SOCKET overrides). Missing daemon is logged once and the wrapper runs in standalone mode with periodic reconnect attempts.
  - main.c polls the daemon fd alongside stdin and the child, routes RELOAD to FSM_EV_RELOAD_REQUESTED, and remembers the reload seq so reload_ack can reference it
  - On SWAPPING entry the wrapper calls transport_stop, transport_start on the same command, emits TRANSPORT_STARTED to move SWAPPING -> STARTING, then calls dispatch_replay_initialize to push the cached handshake at the new child
  - When the new child's initialize response arrives, dispatch consumes it (no upstream forward) and emits INITIALIZE_OK. main.c then sends reload_ack{status=ok, ack_seq=remembered seq} to the daemon
  - Reconnect backoff matches the spec: 1s, 2s, 4s, capped at 5s, retries forever until the daemon comes up
  - A new e2e test spawns the real daemon, wraps fake_mcp through it, sends SIGHUP to the daemon, verifies the session survives (initialize response replayed to the new child, no error propagated upstream), and cleans up
- **Context**: This is the target that makes all the earlier plumbing pay off. Before this, T1.8 and T1.9 both work in isolation but nothing ties them together end-to-end: the wrapper doesn't dial the daemon, and even if it did, a reload notification would kill the session because no initialize replay exists. Splits cleanly into two pieces: T1.10a (dispatch caching + replay, pure and unit-testable) and T1.10b (main.c integration: dial, poll, reload handling, child respawn).
- **Depends on**: 🎯T1.7, 🎯T1.9, 🎯T1.10.1, 🎯T1.10.2
- **Status**: Achieved
- **Discovered**: 2026-04-11
- **Achieved**: 2026-04-11

### 🎯T1.10.1 Dispatch layer caches the upstream initialize handshake and exposes a replay entry point, so the event loop can transparently re-initialise a freshly spawned child without telling the upstream agent anything changed.
- **Value**: 5
- **Cost**: 3
- **Acceptance**:
  - dispatch.c captures the raw bytes of the first upstream initialize request and the first upstream notifications/initialized, owned by the dispatch struct and freed in dispatch_free
  - dispatch_replay_initialize(d) sends the cached initialize request to the child via the sink (+ the initialized notification if one was captured) and sets replay_pending
  - When replay_pending is set, the next child response is consumed: not forwarded upstream, in_flight is decremented if it was tracking the replayed request, INITIALIZE_OK is emitted, and replay_pending is cleared
  - dispatch_replay_initialize is a no-op (returns 0) if no initialize has been cached yet
  - New dispatch_test cases cover: caching on first sighting, replay sends bytes to child, replay response consumed not forwarded, second replay works (cache survives) and emits INITIALIZE_OK again
  - All existing dispatch tests still pass
- **Context**: First half of T1.10. Pure change to dispatch.{h,c} + dispatch_test.c — no I/O, no main.c, no daemon. Adds: init-cache state, dispatch_replay_initialize entry point, replay_pending flag that consumes exactly one subsequent child response. Second half (main.c integration, transport cycling, reload ack) lands in T1.10.2 once this is green.
- **Depends on**: 🎯T1.6.1
- **Status**: Achieved
- **Discovered**: 2026-04-11
- **Achieved**: 2026-04-11

### 🎯T1.10.2 main.c integrates the daemon client into the event loop: dials at startup (with reconnect-backoff), polls the daemon fd, routes RELOAD through FSM DRAINING->SWAPPING, cycles the transport, triggers the cached-initialize replay, and sends reload_ack on completion.
- **Value**: 13
- **Cost**: 8
- **Acceptance**:
  - wrapper/src/main.c has a resolve_daemon_socket_path() that honours $MCPBRIDGE_SOCKET and falls back to the platform default matching docs/wire-protocol.md
  - main.c dials the daemon at startup; connection failure is logged once and the wrapper continues in standalone mode (no crash)
  - Daemon fd is polled alongside stdin and the transport read fd; events are routed via daemon_client_try_read
  - RELOAD event is stored (ack_seq + name) and FSM_EV_RELOAD_REQUESTED is emitted, moving FSM RUNNING -> DRAINING
  - On entry to SWAPPING, main.c calls transport_stop + transport_start and emits TRANSPORT_STARTED then calls dispatch_replay_initialize
  - When the replay's INITIALIZE_OK bubbles back through sink_emit_event and the FSM reaches RUNNING, main.c sends reload_ack{status='ok', ack_seq=<stored>} to the daemon
  - Reconnect backoff: poll timeout is set to the remaining backoff interval when the daemon is disconnected; dial is retried on timeout with the sequence 1s -> 2s -> 4s -> 5s (capped), forever
  - wrapper/tests/e2e_reload_test.sh spawns the real daemon, wraps fake_mcp through the built wrapper, does initialize/initialized/tools/list, sends SIGHUP to the daemon, follows with another tools/list, and verifies BOTH tools/list responses come back to the agent side unmodified
- **Context**: Second half of T1.10. Depends on T1.10.1 (dispatch replay). Adds socket path resolution, reconnect-backoff timer, daemon event routing, SWAPPING-driven transport cycling, and the reload-ack path. Culminates in a new e2e test that exercises the full wrapper+daemon+reload loop against fake_mcp.
- **Depends on**: 🎯T1.10.1, 🎯T1.9
- **Status**: Achieved
- **Discovered**: 2026-04-11
- **Achieved**: 2026-04-11

### 🎯T1.2 Wire protocol between wrapper and daemon is specified in docs/wire-protocol.md: socket path, framing, message shapes for register / deregister / reload / shutdown.
- **Value**: 3
- **Cost**: 2
- **Acceptance**:
  - docs/wire-protocol.md exists and specifies: socket path resolution (per-platform), line framing, message envelope, and every message type the wrapper and daemon exchange in v1
  - Socket path rules documented: $XDG_RUNTIME_DIR/mcpbridge/daemon.sock on Linux, ~/Library/Caches/mcpbridge/daemon.sock on macOS, with explicit fallback to $TMPDIR/mcpbridge-$UID/daemon.sock when the preferred location is unavailable
  - Messages specified: register (wrapper -> daemon), deregister (wrapper -> daemon), reload (daemon -> wrapper), shutdown (daemon -> wrapper), plus a version/hello exchange on connect
  - Schema version field documented so the protocol can evolve without breaking older wrappers/daemons
  - Reconnect/backoff behaviour documented: wrapper retries the socket with backoff starting at 1s capped at 5s, logs once on first failure
- **Context**: Both the C wrapper and the Go daemon need to agree on a wire format before either can implement its side. Writing this down first prevents drift and gives both implementations something to test against. The protocol is newline-delimited JSON-RPC-ish (not strict JSON-RPC 2.0 — we don't need method/id/result complexity for a few message types) over a user-local Unix domain socket. Keep it tiny: four message types in v1, schema version field for forward compat.
- **Depends on**: 🎯T1.1
- **Status**: Achieved
- **Discovered**: 2026-04-11
- **Achieved**: 2026-04-11

### 🎯T1.3 The wrapper has a complete MCP framing + parsing module (mcp.c / mcp.h) with unit tests covering initialize, tools/list, tools/call, notifications, id tracking, and framing edge cases.
- **Value**: 5
- **Cost**: 5
- **Acceptance**:
  - wrapper/src/mcp.h declares types and functions for: parsing one newline-delimited JSON-RPC message from a byte buffer, identifying message kind (request/response/notification), extracting method name + id, pulling out the initialize-response payload for caching, and emitting a notifications/tools/list_changed message
  - wrapper/src/mcp.c implements the above using vendored cJSON with strict validation (no trust of upstream)
  - JSON-RPC ids handled as either number or string (stored as a tagged union or string-normalised form)
  - Streaming line reader caps individual messages at 4 MB; longer messages are rejected cleanly with a descriptive error
  - wrapper/tests/mcp_test.c unit-tests every function with golden inputs: well-formed initialize, well-formed tools/list, well-formed tools/call, a notification with no id, a malformed message, a message at the size cap, an id that is a string, and an id that is a float (rejected cleanly)
  - make test passes the mcp_test binary
- **Context**: The wrapper needs to parse MCP messages to do its job: it must recognise initialize (for caching + replay), tools/list (for diffing), tools/call (for in-flight id tracking), notifications (pass-through vs emit), and it must produce well-formed MCP messages of its own (replay initialize, emit tools/list_changed). This module is pure: no I/O, no state machine dependencies, just bytes in -> structured view out, and structured form -> bytes out. First real C module after the skeleton; unblocks the state machine (T1.4).
- **Depends on**: 🎯T1.1
- **Status**: Achieved
- **Discovered**: 2026-04-11
- **Achieved**: 2026-04-11

### 🎯T1.4 The wrapper has a transport-agnostic child-lifecycle state machine (fsm.c / fsm.h) driven by a pure fsm_step(fsm*, event) -> new_state function, unit-tested without any real I/O.
- **Value**: 5
- **Cost**: 3
- **Acceptance**:
  - wrapper/src/fsm.h defines enum fsm_state, enum fsm_event, struct fsm, and a pure fsm_step function with no I/O dependency
  - wrapper/src/fsm.c implements fsm_init, fsm_step, fsm_state_name, fsm_event_name, plus respawn-attempt bookkeeping that caps retries and transitions to FAILED on exhaustion
  - Every valid transition from every non-terminal state is unit-tested
  - Invalid transitions (e.g., RELOAD while SWAPPING) are unit-tested and leave the state unchanged rather than crashing
  - Respawn exhaustion is tested: repeated CHILD_EXIT while in RESPAWN eventually transitions to FAILED
  - make test passes the fsm_test binary with no failures
- **Context**: The state machine is the coordination spine of the wrapper. It enforces "no upgrade with requests in flight" and "no forwarding while between children" by construction, not by scattered boolean flags. After the daemon-does-the-upgrading pivot, the UPGRADING state disappeared — the wrapper only needs STARTING/RUNNING/DRAINING/SWAPPING/RESPAWN/FAILED. This sub-target lands the pure machine; wiring it into real transports comes later (🎯T1.5).
- **Depends on**: 🎯T1.1
- **Status**: Achieved
- **Discovered**: 2026-04-11
- **Achieved**: 2026-04-11

### 🎯T1.5 The wrapper has a transport abstraction and a stdio implementation that fork/exec a child MCP server with stdin/stdout pipes, exposing read/write fds for the event loop, plus a non-blocking child-reap helper.
- **Value**: 5
- **Cost**: 5
- **Acceptance**:
  - wrapper/src/transport.h defines struct transport_ops (vtable: start, stop, read_fd, write_fd, read, write, destroy) and struct transport with inline wrapper helpers
  - wrapper/src/transport_stdio.{h,c} implements the vtable by forking a child, setting up stdin/stdout pipes (stderr inherited), exec'ing the given command+argv, and exposing the parent-side fds
  - transport_stdio_pid() returns the child pid after start; transport_stdio_reap() does a non-blocking waitpid and reports running/exited/error so the event loop can feed CHILD_EXIT to the FSM
  - Start-side failure (fork fails, exec fails, pipe fails) is propagated cleanly; the transport object is in a well-defined state after a failed start and can be destroyed safely
  - Stop closes stdin (so the child sees EOF and exits gracefully), waits briefly for exit, SIGTERMs, waits again, SIGKILLs as last resort
  - tests/fake_echo.c is a tiny standalone child that reads bytes from stdin and writes them back to stdout verbatim (no libc buffering), used by the integration tests
  - wrapper/tests/transport_stdio_test.c spawns fake_echo through the transport, round-trips bytes, and verifies clean shutdown with the expected exit status
  - make test passes the transport_stdio_test binary
- **Context**: The transport abstraction is the boundary between the event loop and the concrete way of talking to the wrapped MCP server. Two implementations are planned: stdio (fork a child process) and localhost HTTP (connect to an HTTP endpoint with SSE). This target lands the interface plus the stdio backend. The HTTP backend comes later (🎯T1.9). The event loop that actually uses these comes in 🎯T1.7.
- **Depends on**: 🎯T1.1
- **Status**: Achieved
- **Discovered**: 2026-04-11
- **Achieved**: 2026-04-11

### 🎯T1.6 The wrapper has a minimum-viable dispatch module that forwards MCP messages between upstream and child based on FSM state, tracks in-flight requests, queues while not RUNNING, and emits INITIALIZE_OK / IN_FLIGHT_ZERO events to the FSM.
- **Value**: 5
- **Cost**: 5
- **Acceptance**:
  - wrapper/src/dispatch.{h,c} exposes dispatch_new / dispatch_free, plus dispatch_on_upstream / dispatch_on_child / dispatch_on_state_change entry points
  - Dispatch uses an injected sink (send_upstream / send_child / emit_event callbacks) so it is unit-testable without real I/O
  - Messages forwarded verbatim (using the raw bytes cached in mcp_msg) — no re-serialisation
  - When fsm state is RUNNING: upstream requests forward to child and increment in_flight; child responses forward to upstream and decrement in_flight
  - When fsm state is not RUNNING: upstream messages queue in a FIFO for later replay; child responses still forward through (a dying child may still answer queued requests)
  - State transition into RUNNING drains the queued upstream messages in order
  - When in_flight hits zero while fsm state is DRAINING, emit FSM_EV_IN_FLIGHT_ZERO
  - First initialize response seen from the child emits FSM_EV_INITIALIZE_OK (or FSM_EV_INITIALIZE_FAILED if it contains an error)
  - wrapper/tests/dispatch_test.c exercises: simple round-trip in RUNNING, queuing while STARTING and drain on RUNNING transition, in-flight counter math, DRAIN -> IN_FLIGHT_ZERO emission, initialize response emits INITIALIZE_OK exactly once
  - make test passes dispatch_test
- **Context**: The dispatch layer is the "business logic" that sits between the parsed MCP messages and the event loop. This target is the minimum-viable slice: message routing based on state, in-flight counter that drives DRAINING, and first-initialize-response detection. Initialize replay on child restart, tools/list caching and diffing, and the notifications/tools/list_changed emission path are deferred to 🎯T1.6a so this target stays focused and testable in isolation.
- **Depends on**: 🎯T1.3, 🎯T1.4
- **Status**: Achieved
- **Discovered**: 2026-04-11
- **Achieved**: 2026-04-11

### 🎯T1.6.1 The dispatch layer intercepts the initialize handshake: initialize requests forward immediately even in STARTING, so the FSM transitions STARTING -> RUNNING naturally via INITIALIZE_OK instead of being force-stepped by main.c.
- **Value**: 5
- **Cost**: 3
- **Acceptance**:
  - wrapper/src/dispatch.c forwards initialize requests immediately regardless of FSM state (STARTING / RUNNING / SWAPPING)
  - All other upstream messages continue to queue while not RUNNING
  - wrapper/tests/dispatch_test.c has a test case that feeds an initialize request in STARTING and verifies it is forwarded, not queued
  - wrapper/tests/dispatch_test.c's existing test_initialize_ok_emitted is rewritten to start from STARTING and exercise the real flow
  - wrapper/tests/fake_mcp.c is a tiny MCP server that handles initialize / notifications/initialized / tools/list / tools/call with canned responses, built as a test helper like fake_echo
  - wrapper/src/main.c no longer force-steps the FSM to RUNNING; startup is driven by the real initialize handshake
  - wrapper/tests/e2e_wrapper_test.sh drives fake_mcp through the wrapper with a proper initialize handshake followed by a tools/list request, asserting both responses come back intact
- **Context**: T1.7 landed with a known hack: main.c force-steps the FSM to RUNNING at startup because otherwise the initialize request would be queued while the FSM is STARTING, the child would never see it, no response would ever come, and the FSM would never reach RUNNING. This target fixes that deadlock at its root by making the dispatch layer aware that initialize requests are special: during STARTING, an initialize request is forwarded immediately instead of queued. The response then flows back, dispatch emits INITIALIZE_OK, and the FSM reaches RUNNING through the natural protocol. Also adds tests/fake_mcp.c (a minimal proper MCP server) and switches the e2e test to use it, so the smoke test exercises a real initialize handshake.
- **Depends on**: 🎯T1.6, 🎯T1.7
- **Status**: Achieved
- **Discovered**: 2026-04-11
- **Achieved**: 2026-04-11

### 🎯T1.7 The wrapper has a working main event loop in main.c that wires argv parsing + transport + FSM + dispatch into a runnable binary that transparently proxies an MCP child, enabling a first end-to-end smoke test.
- **Value**: 8
- **Cost**: 5
- **Acceptance**:
  - wrapper/src/main.c parses argv for the `--` delimiter and treats everything after as the wrapped command + argv
  - main() constructs transport_stdio, fsm, dispatch, and a sink that blocking-writes to STDOUT (upstream) and to the transport write_fd (child)
  - The sink's emit_event callback feeds the FSM and then calls dispatch_on_state_change on the new state
  - The main poll() loop watches upstream stdin and the transport read_fd, feeds bytes into mcp_readers, and dispatches each popped line through mcp_msg_parse + dispatch_on_upstream / dispatch_on_child
  - Child pipe EOF triggers FSM_EV_CHILD_EXIT; the wrapper reaps the child via transport_stdio_reap and then stops cleanly
  - Upstream stdin EOF triggers a graceful transport_stop and exit
  - Parse errors on either side are logged and do not crash
  - wrapper/tests/e2e_wrapper_test.sh spawns the built mcpbridge wrapping tests/fake_echo, writes a JSON-RPC line to it, reads the identical line back from mcpbridge's stdout, asserts success, and is run via `make test`
- **Context**: First end-to-end wiring. This takes the pure modules (parser, FSM, dispatch, transport) and glues them into a runnable mcpbridge binary that an MCP client can launch. Minimum-viable: no daemon connection yet, no reload path, no signal handling beyond SIGTERM/SIGINT cleanup. Child death (detected via pipe EOF, not SIGCHLD yet) exits the wrapper. An integration test sanity-checks the pipeline by running the wrapper against fake_echo.
- **Depends on**: 🎯T1.5, 🎯T1.6
- **Status**: Achieved
- **Discovered**: 2026-04-11
- **Achieved**: 2026-04-11

### 🎯T1.8 The daemon has a working UDS server that accepts wrapper connections, performs the hello/register handshake, tracks live wrappers, and can broadcast reload notifications on SIGHUP — following the wire protocol spec verbatim.
- **Value**: 8
- **Cost**: 8
- **Acceptance**:
  - daemon/internal/socket/path.go resolves the socket path per the wire-protocol spec (MCPBRIDGE_SOCKET env override, ~/Library/Caches/mcpbridge/daemon.sock on macOS, $XDG_RUNTIME_DIR/mcpbridge/daemon.sock on Linux, $TMPDIR/mcpbridge-$UID/daemon.sock fallback)
  - daemon/internal/socket/protocol.go defines the Envelope and per-type message structs matching the wire spec (hello, hello_ok, register, register_ok, deregister, reload, reload_ack, shutdown, error), with JSON marshal/unmarshal round-trip tests
  - daemon/internal/socket/server.go implements a Server that listens on the UDS, enforces line framing, speaks the handshake, and exposes Register/Deregister and a Broadcast method for the reload path
  - Daemon creates the socket parent dir with mode 0700 and the socket with mode 0600
  - daemon/internal/socket/server_test.go starts a real listener on a temp socket and drives it through a fake client goroutine: hello/register, broadcast reload, deregister, clean shutdown
  - daemon/cmd/mcpbridge-daemon/main.go wires up the server, installs SIGHUP to broadcast reload, SIGINT/SIGTERM for clean shutdown, runs until stopped
  - `go test ./...` in daemon/ passes all new tests
- **Context**: First real daemon functionality. Scope: socket path resolution per platform, wire-protocol message types matching docs/wire-protocol.md, a listener + connection handler that speaks the protocol, and a registry tracking live wrappers. SIGHUP broadcasts a reload to all connected wrappers with reason=manual — both a useful "check now" knob and the easiest way to test the reload path without source backends or a scheduler. Source backends and the polling scheduler come in later targets.
- **Depends on**: 🎯T1.1, 🎯T1.2
- **Status**: Achieved
- **Discovered**: 2026-04-11
- **Achieved**: 2026-04-11

### 🎯T1.9 The wrapper has a daemon client module (daemon_client.c/h) that dials the daemon's UDS, performs the hello/register handshake, and can read and write protocol envelopes. Integration test uses the real daemon binary.
- **Value**: 5
- **Cost**: 5
- **Acceptance**:
  - wrapper/src/daemon_client.{h,c} exposes daemon_client_new, daemon_client_do_handshake, daemon_client_send_deregister, daemon_client_send_reload_ack, daemon_client_try_read, daemon_client_fd, daemon_client_free
  - The read path accumulates bytes via mcp_reader (or an equivalent line reader) and parses each complete line as a JSON envelope with cJSON
  - Socket path can be specified explicitly (or picked up from $MCPBRIDGE_SOCKET); no platform-default resolution yet — that can live in main.c later
  - wrapper/tests/daemon_client_test.c fork/execs the real mcpbridge-daemon binary into a private /tmp socket, connects as a client, does hello + register, asserts register_ok is received, sends a deregister, and tears the daemon down cleanly
  - make test passes daemon_client_test
- **Context**: Second half of the wire protocol: the wrapper side of the socket. Standalone module for now — main.c integration and the full reload-cycles-child flow come in 🎯T1.10. This target isolates the connect/handshake/envelope plumbing so it can be tested in isolation against a real spawned daemon binary, which also exercises the daemon side (T1.8) on the same integration path.
- **Depends on**: 🎯T1.2, 🎯T1.8
- **Status**: Achieved
- **Discovered**: 2026-04-11
- **Achieved**: 2026-04-11

## Graph

```mermaid
graph TD
    T1["A generic C wrapper transpare…"]
```
