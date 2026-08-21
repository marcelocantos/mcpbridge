# Entropy audit — mcpbridge

Date: **2026-08-22**. Mode: **full** (entropy + hygiene).
Auditor: entropy-audit owner for this repository only.

## Executive summary

- **Snapshot:** `/Users/marcelo/work/github.com/marcelocantos/mcpbridge`, branch `master`, HEAD `fd92cc2d41d6535089e3c6004d27579234c34cad` (`fd92cc2 Spurious reloads and stranded sessions (#24)`). Tag `v0.9.0`. Initial `git status --porcelain=v1 -b`: `## master...origin/master` (clean working tree). Local ignored build artifacts (`wrapper/*.o`, `wrapper/mcpbridge`, `daemon/mcpbridge-daemon`, staged `help_agent.md`) were present after `make test` and are gitignored.
- **Scope:** shipped C wrapper (`wrapper/src`), Go daemon (`daemon/`), top-level orchestration (`Makefile`, `go.work`), CI/release workflows, public docs, in-repo Homebrew template. **Excluded:** vendored `wrapper/vendor/cjson/` (third-party; LICENSE present), compiled `*.o` / test binaries, gitignored `daemon/cmd/mcpbridge-daemon/help_agent.md` (Makefile copy of `agents-guide.md`). No Python/Rust/SQL/web frontend in manifests; those language companions were not applied.
- **Headline mechanism:** the **two-process C-wrapper + Go-daemon split is coherent**, but load-bearing facts (config schema, wire-protocol constants, public 1.0 contract) are restated independently in C, Go, and prose. After v0.8.0 retired the brew/github **scheduler**, `STABILITY.md` and several comments still describe that product, while the parsers and protocol constants can already disagree on documented values.
- **Highest-consequence findings:** ENT-001 (public contract describes retired upgrade backends), ENT-002 (C/Go config validators already disagree on `::1` and on `tool_call_timeout_ms`), ENT-003 (`FSM_RESPAWN` recovery is unit-tested and unreachable from the event loop).
- **Unverified residue:** no vulnerability scanner was run (none configured); no live Homebrew-install or live-mnemo journey; no Linux CI executed on this host (macOS arm64 only); no clone detector installed.

## Scope and exclusions

| Included | Excluded / named |
|---|---|
| `wrapper/src/*.[ch]`, `wrapper/tests/*`, `wrapper/Makefile` | `wrapper/vendor/cjson/*` (vendored cJSON 1.7.18) |
| `daemon/cmd`, `daemon/internal/{config,socket,watcher}` | Build artifacts (`*.o`, `mcpbridge`, `mcpbridge-daemon`) |
| `.github/workflows/{ci,release}.yml`, `packaging/homebrew/mcpbridge.rb` | Empty untracked local dirs `wrapper/daemon/**` (not in git) |
| `README.md`, `STABILITY.md`, `agents-guide.md`, `docs/*`, `bullseye.yaml` | Prior defect audit `docs/audit/fable-2026-07.md` used as history, not as this report's denominator |

No `AGENTS.md`, `CLAUDE.md`, or `hygiene.yaml` in this repo.

## Commands run

| Command | Version / notes | Exit | Shipped path? | Limitations |
|---|---|---|---|---|
| `git status --porcelain=v1 -b`; `git rev-parse HEAD` | git; HEAD `fd92cc2d41d6535089e3c6004d27579234c34cad`, branch `master` | 0 | provenance | — |
| `git describe --tags --always`; `git tag --sort=-v:refname` | latest tag `v0.9.0` | 0 | provenance | — |
| `go version` | `go1.26.4 darwin/arm64` | 0 | toolchain | Host newer than CI `go-version: '1.24'` |
| `cc --version` | Apple clang 21.0.0 | 0 | toolchain | — |
| `make --version` | GNU Make 3.81 | 0 | toolchain | — |
| `cd daemon && gofmt -l .` | empty output | 0 | auxiliary (CI does not run gofmt; `make bullseye` does) | Format only |
| `cd daemon && go vet ./...` | — | 0 | auxiliary (same: local `make bullseye`, not CI) | — |
| `cd daemon && go test -race ./...` | packages `cmd/mcpbridge-daemon`, `internal/{config,socket,watcher}` all `ok` | 0 | **shipped** (`Makefile` `daemon-test` is `go test -race ./...`; CI `make test` runs it) | macOS arm64 only this run |
| `make test` | wrapper unit + 10 e2e scripts + daemon `-race`; all printed `ok` | 0 | **shipped** (CI Test step is `make test`) | No Linux runner here; e2e uses `fake_mcp` / `fake_http_mcp`, not a live third-party MCP server |
| `git log --name-only --since=2026-01-01` (churn rollup) | top files: `bullseye.yaml` 19, `wrapper/src/main.c` 10, `STABILITY.md` 10, `wrapper/src/dispatch.c` 8 | 0 | history | Counts commits, not complexity |
| `git blame` on `STABILITY.md` source/`MCPBRIDGE_BREW_PATH` rows | `source`/`upgrade`/`check_interval` last touched `ca306cda` (2026-04-12); `MCPBRIDGE_BREW_PATH` `057f47d8` (2026-04-22) | 0 | history | — |
| Python `urllib.parse` of `http://[::1]/mcp` | hostname `::1` | 0 | auxiliary illustration of Go `url.Parse` behaviour | Not the C parser |

Not run (unavailable / out of policy): `govulncheck`, `jscpd`, Linux matrix, `hygiene_check.py` (no `hygiene.yaml`; not initialized).

## Observed architecture

### Declared (README, agents-guide, daemon `main.go` package comment)

Two deployable units, one Unix-domain-socket protocol:

```
MCP client --stdio JSON-RPC--> mcpbridge (C) --stdio or loopback HTTP--> wrapped MCP server
                                   |
                                   +-- newline JSON UDS v1 --> mcpbridge-daemon (Go)
                                                                  |
                                                                  +-- fsnotify on child_binary
                                                                  +-- SIGHUP => BroadcastReload("manual")
```

Wrapper is the session source of truth. Daemon is optional: missing daemon ⇒ standalone bridge, reconnect with 1s→5s backoff. Upgrades are **out of band** (`brew upgrade`, manual install); the daemon does not poll or install (v0.8.0 / 🎯T11, commit `b7c8ee5`).

### Observed modules

| Unit | Entry | Internals | Public surface |
|---|---|---|---|
| C wrapper | `wrapper/src/main.c` | `config`, `mcp`, `fsm`, `dispatch`, `transport_{stdio,http}`, `daemon_client`, `log`, `util` | CLI `mcpbridge connect <path>`, stdio MCP, UDS client |
| Go daemon | `daemon/cmd/mcpbridge-daemon/main.go` | `internal/config`, `internal/socket`, `internal/watcher` | CLI `mcpbridge-daemon`, UDS server, fsnotify |
| Specs | `docs/wire-protocol.md`, `docs/config-schema.md` | restated in C + Go | pre-1.0 contract also in `STABILITY.md` |

Dependency direction is acyclic at package level: daemon `cmd` → `config` / `socket` / `watcher`; `watcher` depends on a `Broadcaster` interface, not on `config`. Wrapper is a single C program with a poll loop (no threads). Go `internal/` correctly hides the daemon from external importers.

### Declared vs observed

| Rule | Status |
|---|---|
| Two-process split; C small, Go for fsnotify | **agrees** (README “Why two languages?”, code layout) |
| Config schema v2: exactly one of `command`/`url`; `name` is registration id | **agrees** in C + Go parsers; **contradicted** by `STABILITY.md` still requiring `source` |
| Daemon discovers `~/.config/mcpbridge` then Homebrew share dirs | **inferred/contradicted:** code hardcodes `/opt/homebrew/share/mcpbridge` then `/usr/local/share/mcpbridge` (`main.go`); `docs/config-schema.md` says `$HOMEBREW_PREFIX/share/mcpbridge` |
| Wire line cap 256 KiB both peers | **contradicted:** C reader 256 KiB; Go `bufio.Reader` 32 KiB |
| Register `name` from config `name` field | **agrees** in code + STABILITY item 2; **contradicted** by `docs/wire-protocol.md` (`basename(argv[1])` / `--config NAME`) |
| Exactly one daemon per socket | **enforced** (`flock` on `*.lock`, `TestNewServer_RefusesSecondDaemon`) |
| `conn.reg` published under `s.mu`; watcher keyed by `connID` | **enforced** (Fable-5 T13/T15/watcher Track) |
| `FSM_RESPAWN` recovers a crashed stdio child | **declared** in `fsm.h` + `fsm_test.c`; **contradicted** by `main.c` exiting when state is not `SWAPPING` |

## Dimension vector

| Dimension | State | Evidence summary | Change from baseline |
|---|---|---|---|
| Architecture topology | healthy | Clear two-process split, acyclic internals, UDS as the only cross-language edge | no prior entropy-audit report; Fable-5 defect audit 2026-07 |
| Redundancy / sources of truth | concern | Schema, wire constants, socket path, agent-help, and 1.0 contract each have ≥2 restatements; C vs Go already disagree on `::1` | first vector |
| Change amplification | concern | A schema or protocol tweak must land in C, Go, `docs/config-schema.md`, `docs/wire-protocol.md`, `STABILITY.md`, tests, and often `agents-guide.md` | first vector |
| Local code quality | concern | Deliberate small C modules and a pure FSM, plus unreachable `RESPAWN`, leftover `WATCH_SRC` / scheduler comments, drain-queue comment/code mismatch | first vector |
| Correctness / verification | concern | `make test` green including 10 wrapper e2e scripts and `go test -race`; no cross-peer frame/schema oracle; `RESPAWN` only unit-tested | first vector |
| Security / dependencies | concern | Apache-2.0 + `NOTICES.md`; lockfiles; UDS 0600 in 0700 dir, same-user trust; no scanner in CI; `golang.org/x/sys v0.13.0` unreviewed here | first vector |
| Build / release / operations | concern | `make` + GitHub Actions matrix (ubuntu/macos CI; darwin-arm64 + linux-amd64/arm64 release) works; in-repo `packaging/homebrew/mcpbridge.rb` is a v0.1.0 fossil | first vector |
| Documentation / governance | concern | README/agents-guide/config-schema mostly current; `STABILITY.md` 1.0 gaps still discuss a deleted scheduler; hygiene posture **not declared**; 🎯T12 still `identified` | first vector |

Do not collapse this vector to a scalar.

## Findings

### ENT-001: Public 1.0 contract still describes the retired scheduler product

- **Priority:** P1
- **Dimensions:** Documentation / governance; Redundancy / sources of truth
- **Status:** observed fact
- **Evidence:**
  - `STABILITY.md:68-73` still documents `MCPBRIDGE_BREW_PATH` and brew-source path fallbacks as **stable**. `git blame` attributes those lines to `057f47d8` (v0.3.0). Grep of `*.go` / `*.c` / `*.h` finds **no** `MCPBRIDGE_BREW_PATH`.
  - `STABILITY.md:138-140` lists `source` (required), `upgrade`, `check_interval` as **stable**. Blame: `ca306cda` (v0.1.0).
  - `STABILITY.md:238-246` and `:291-299` still treat github-source baselining and “the scheduler” reacting to `reload_ack` as 1.0 prerequisites.
  - Shipped code: `daemon/cmd/mcpbridge-daemon/main.go:5-12` and `:65-69` — daemon watches binaries via fsnotify and does not poll or install. `daemon/internal/config/config.go:57-67` has no `Source`/`Upgrade` fields. `docs/config-schema.md:51-55` states those fields were **removed in v0.8.0** and are silently ignored. `daemon/internal/config/config_test.go:194-208` `TestVestigialFieldsLoadSilently` encodes that ignore.
  - Product narrowing: commit `b7c8ee5` (“retire brew/github source backends and polling scheduler (T11)”). Docs refresh `fd2faf8` updated some docs; the STABILITY table rows above were not rewritten.
- **Mechanism:** two authorities for the public surface. An agent or human following `STABILITY.md` will implement or require brew/github polling that the daemon no longer does, and will treat `source` as required even though both parsers ignore it. 1.0 planning is aimed at a deleted subsystem.
- **Blast radius:** every consumer of the stability contract (release skill, agents following `STABILITY.md`, 1.0 gate list). Runtime of current binaries is unaffected if they follow `docs/config-schema.md`.
- **Counterevidence checked:** README and `agents-guide.md` match the fsnotify-only daemon. `register_ok.polling` is intentionally vestigial and always false (`main.go:163-165`) — that is compatibility, not a live scheduler. `bullseye.yaml` 🎯T1 acceptance still mentions brew/github/cmd sources but the target is `achieved` historical record, not a living spec.
- **Smallest coherent remediation:** rewrite `STABILITY.md` against the v0.8.0+ surface (drop `MCPBRIDGE_BREW_PATH`, `source`/`upgrade`/`check_interval` as contract items, replace scheduler 1.0 gaps with the actual remaining gaps: capabilities diff, HTTP loopback scope, etc.). Keep one sentence that old fields are ignored.
- **Verification:** `rg -n 'MCPBRIDGE_BREW_PATH|the scheduler currently' STABILITY.md` is empty (or only in a “retired in v0.8.0” note). A docs test or release-skill check that STABILITY field names ⊆ parser structs.
- **Ratchet candidate:** file-content check: `STABILITY.md` must not contain `MCPBRIDGE_BREW_PATH` unless paired with “retired”; or a small test that greps the stability catalogue against `Config` JSON tags.

### ENT-002: Schema v2 has two validators and they already disagree

- **Priority:** P1
- **Dimensions:** Redundancy / sources of truth; Change amplification; Correctness / verification
- **Status:** observed fact
- **Evidence:**
  - Dual parsers of one schema: `wrapper/src/config.c` `config_load` vs `daemon/internal/config/config.go` `ParseBytes`. Both comments name `docs/config-schema.md` as authoritative (`config.h:17`, `config.go:18-19`).
  - **`tool_call_timeout_ms`:** implemented in C (`config.c:231-241`, default `config.h:57`, applied `main.c:860`). **Absent** from the Go `Config` struct (`config.go:57-67`). **Absent** from the authoritative table `docs/config-schema.md:40-46` (`rg tool_call_timeout docs/config-schema.md` → no matches). Present in `README.md:147` and `STABILITY.md:141`.
  - **Loopback `::1`:** schema and STABILITY allow host `::1` (`docs/config-schema.md:46`, `STABILITY.md:137`). Go `validateConnection` uses `url.Parse` + `Hostname()` and `isLoopback` includes `"::1"` (`config.go:151-171`). C `validate_url` (`config.c:119-138`) and `http_url_parse` (`transport_http.c:69-74`) terminate the host at the first `:`. For the documented form `http://[::1]/mcp` the C host token is `[` (next char is `:`); for `http://::1/mcp` host length is 0. C therefore **rejects** every `::1` URL. Python `urllib.parse` (same hostname extraction as Go) yields hostname `::1` for `http://[::1]/mcp`.
  - C `config.h:13-16` still says daemon-only `source`/`upgrade`/`check_interval` are “validated on its side” — they are not.
- **Mechanism:** a schema change or a documented host form must be implemented twice with no shared fixture. The IPv6 case is already a split brain: daemon `Load` would accept a file the wrapper refuses to start. The timeout field can be added to docs on one side and silently ignored by the other forever (Go ignore-unknown vs C required semantics).
- **Blast radius:** HTTP configs using IPv6 loopback; any future schema field; dual-maintenance cost on every config change.
- **Counterevidence checked:** no Go test feeds `http://[::1]/…`. `transport_http_test` rejects `example.com` and `ftp://` but does not assert `::1` acceptance. Daemon does not dial URLs (watcher skips URL-shaped `child_binary`, `watcher.go:152-160`), so daemon-side accept of `::1` is discovery-only today — wrapper is the runtime gate. Extra JSON fields loading on Go is deliberate (`TestVestigialFieldsLoadSilently`).
- **Smallest coherent remediation:** pick one owner for connection validation. Practical: add a shared JSON fixture corpus (`testdata/config/*.json` + expected accept/reject) consumed by C tests and `config_test.go`. Fix C host parsing to accept `[::1]` (bracket form) **or** strike `::1` from the schema until the C parser matches. Add `tool_call_timeout_ms` to `docs/config-schema.md` (and optionally to the Go struct as `json:"-"` documented wrapper-only).
- **Verification:** fixtures `http://[::1]/mcp` and `http://[::1]:19419/mcp` produce the **same** accept/reject in `config_load` and `ParseBytes`. Schema table lists every field C or Go reads.
- **Ratchet candidate:** `command:` running a tiny Go+C fixture runner in CI; or a `file:` hygiene item once that runner exists.

### ENT-003: `FSM_RESPAWN` is a tested recovery path the event loop never drives

- **Priority:** P1
- **Dimensions:** Local code quality; Correctness / verification
- **Status:** observed fact
- **Evidence:**
  - `wrapper/src/fsm.h:32-42` — `RUNNING + CHILD_EXIT → RESPAWN`; `RESPAWN + BACKOFF_EXPIRED → SWAPPING`.
  - `wrapper/src/fsm.c:101-109` — only `FSM_EV_BACKOFF_EXPIRED` leaves `RESPAWN`.
  - `wrapper/tests/fsm_test.c:106-108` and the respawn-limit sequence (`:230-240`) assert that path.
  - `rg FSM_EV_BACKOFF_EXPIRED wrapper/src` hits **only** `fsm.c` / `fsm.h`. `main.c` never emits it.
  - `wrapper/src/main.c:692-704`: on child stdout close, emit `CHILD_EXIT`; if the new state is **not** `SWAPPING`, `break` out of `run_loop` (wrapper exits). `RUNNING → RESPAWN` therefore **exits the process**.
  - `STABILITY.md:217-219` says the wrapper exits only on stdin EOF, SIGINT, SIGTERM, or `FAILED`. `RESPAWN` is none of those; the loop still exits.
- **Mechanism:** the FSM encodes crash recovery; the event loop treats unexpected child death as terminal except during drain (`DRAINING + CHILD_EXIT → SWAPPING`, covered by `e2e_child_death_inflight_test.sh`). Maintainers reading `fsm.h` / `fsm_test` will believe stdio crash recovery exists. The product promise that the agent session stays alive “across upstream restarts” (`README.md:95-97`) holds for daemon-driven reload and HTTP 4xx self-reload, not for a stdio child that dies while `RUNNING`.
- **Blast radius:** every stdio-backed wrap; a panic/`kill` of the child tears down the MCP client session. Dead code (`respawn_limit`, `BACKOFF_EXPIRED`) continues to be unit-tested, amplifying change (every FSM edit pays for a path the product does not use).
- **Counterevidence checked:** HTTP autonomous reload does not use `RESPAWN` (it uses `RELOAD_REQUESTED`). Drain-time child death is intentionally `SWAPPING` and is e2e-tested. Exiting on unexpected stdio death may be an accepted product choice — it is not documented as such, and the FSM contradicts it.
- **Smallest coherent remediation:** either (a) emit `BACKOFF_EXPIRED` from a timer in `main.c` and e2e a stdio child kill while `RUNNING`, or (b) delete `RESPAWN` / `BACKOFF_EXPIRED` from the FSM, tests, and comments, and document “stdio child death while RUNNING ends the wrapper.”
- **Verification:** if kept: e2e that kills `fake_mcp` while `RUNNING` and asserts the agent session survives N respawns. If deleted: `rg RESPAWN wrapper/src` empty; `fsm_test` no longer names the state.
- **Ratchet candidate:** architecture test or e2e; do not leave unit tests as the only oracle for a path the loop never takes.

### ENT-004: Wire-protocol line cap is 256 KiB in the spec and C, 32 KiB in the daemon reader

- **Priority:** P2
- **Dimensions:** Redundancy / sources of truth; Correctness / verification
- **Status:** observed fact (also Fable-5 F6, still open)
- **Evidence:**
  - Spec: `docs/wire-protocol.md:14` — max line **256 KiB**.
  - C: `wrapper/src/daemon_client.c:159` — `mcp_reader_init(&dc->reader, 256 * 1024)`.
  - Go: `daemon/internal/socket/server.go:26` `MaxLineBytes = 256 * 1024`; `:361` `bufio.NewReaderSize(raw, 32*1024)`; `:494-498` `ReadSlice` + `ErrBufferFull` reported as `"line exceeds %d bytes", MaxLineBytes` (256 KiB). `bufio.Reader` of size 32 KiB returns `ErrBufferFull` at 32 KiB.
- **Mechanism:** three restatements of one constant; the enforcement path uses a different number than the named constant and the error string. v1 envelopes (hello/register/reload) are tiny, so production is not currently hitting the wall. A spec-legal 40 KiB envelope (e.g. a long `reload_ack.detail`) drops the connection with a lying log line. Same class as ENT-002: no cross-peer conformance test.
- **Blast radius:** daemon↔wrapper connection for any oversized-but-legal line; future envelope growth; anyone debugging “line exceeds 262144 bytes” when 32768 was the real cap.
- **Counterevidence checked:** `protocol_test.go` round-trips small envelopes. `daemon_client_test` / e2e use short lines. Fable-5 recorded this; it was **not** closed by `#23`/`#24`.
- **Smallest coherent remediation:** `bufio.NewReaderSize(raw, MaxLineBytes+1)` (or accumulate until `MaxLineBytes` independently of buffer size). Add a cross-peer test: 256 KiB−1 accepted by both readers, 256 KiB+1 rejected by both.
- **Verification:** that test in CI.
- **Ratchet candidate:** Go test on `readEnvelope` plus a C test on `mcp_reader`; ideally one shared fixture.

### ENT-005: Wrapper `--help-agent` is a hardcoded fork of `agents-guide.md`

- **Priority:** P2
- **Dimensions:** Redundancy / sources of truth; Documentation / governance
- **Status:** observed fact (already 🎯T12 `identified` in `bullseye.yaml:467-482`)
- **Evidence:** `wrapper/src/main.c:75-97` `agent_help_text`; `:765-766` prints only that string. Daemon embeds the canonical guide: `Makefile:26-27` copies `agents-guide.md` → `help_agent.md`; `main.go:32-33` `//go:embed`; `:114-123` prepends usage. Wrapper copy is a short paraphrase, not the install four-step / troubleshooting guide.
- **Mechanism:** two agent-facing help surfaces. The daemon one tracks `agents-guide.md`; the wrapper one drifts silently (T12 context already notes this). An agent that invokes `mcpbridge --help-agent` (the binary the MCP client actually launches) never sees the install completeness rules.
- **Blast radius:** agent-driven install/troubleshooting; every edit to `agents-guide.md`.
- **Counterevidence checked:** `--help` usage text is separate and accurate. T12 is identified, not achieved — still open.
- **Smallest coherent remediation:** implement 🎯T12 (stage `agents-guide.md` at wrapper build, print usage + guide).
- **Verification:** `mcpbridge --help-agent` contains a known `agents-guide.md` substring (T12 acceptance).
- **Ratchet candidate:** T12’s proposed e2e/unit substring assert.

### ENT-006: In-repo Homebrew formula is a v0.1.0 fossil next to the live releaser

- **Priority:** P2
- **Dimensions:** Build / release / operations; Documentation / governance
- **Status:** observed fact
- **Evidence:** `packaging/homebrew/mcpbridge.rb:12-13` `url` tag `v0.1.0`, `sha256 "REPLACE_ME_ON_RELEASE"`. Caveats (`:57-62`) still show `"args": ["--", "real-mcp-server", …]` — removed in v0.4.0. Last commit on the file: `ca306cd` (2026-04-12). Live path: `.github/workflows/release.yml:87-119` `Justintime50/homebrew-releaser@v3` with `install: bin.install "mcpbridge"` / `"mcpbridge-daemon"` and a service block. Formula comments say to copy this file into the tap.
- **Mechanism:** two packaging sources. Copying the in-repo formula publishes a 0.1.0 URL and teaches the deleted CLI. The GitHub release job does not use this file, so CI stays green while the template rots.
- **Blast radius:** anyone following `packaging/homebrew/` instead of the tap; confusion during `/release`.
- **Counterevidence checked:** `docs/packaging.md` and `agents-guide.md` use `mcpbridge connect <path>`. Tap is updated by homebrew-releaser, not by this rb.
- **Smallest coherent remediation:** replace the rb with a short pointer to the tap + `release.yml`, **or** regenerate it to match v0.9.0 + `connect <path>` and add a comment that homebrew-releaser is canonical.
- **Verification:** `rg -n '"--"' packaging/homebrew/mcpbridge.rb` empty; version in the example formula is not `v0.1.0`.
- **Ratchet candidate:** file regex on `mcpbridge.rb` forbidding the old `--` argv shape, or delete the file.

### ENT-007: Daemon share-dir discovery is hardcoded, not `$HOMEBREW_PREFIX`

- **Priority:** P2
- **Dimensions:** Redundancy / sources of truth; Build / release / operations
- **Status:** observed fact
- **Evidence:** `docs/config-schema.md:19-23` discovery dir 2 is `$HOMEBREW_PREFIX/share/mcpbridge`. `daemon/cmd/mcpbridge-daemon/main.go:56-60` returns `~/.config/mcpbridge`, **`/opt/homebrew/share/mcpbridge`**, **`/usr/local/share/mcpbridge`**. Linuxbrew prefix is typically `/home/linuxbrew/.linuxbrew`; those share configs are never scanned. `STABILITY.md:151-155` matches the **hardcoded** paths, not `$HOMEBREW_PREFIX`.
- **Mechanism:** three texts, two algorithms. Apple Silicon Homebrew happens to equal `/opt/homebrew`, hiding the drift. Linux brew-managed configs in `$HOMEBREW_PREFIX/share/mcpbridge` are invisible to the daemon (`config_found=false`, no name routing).
- **Blast radius:** Linuxbrew installs that drop configs in the formula `share/mcpbridge`; any non-default prefix.
- **Counterevidence checked:** user configs in `~/.config/mcpbridge` still win and are the documented primary. Formula `share` dir is created (`mcpbridge.rb:31-32` and releaser `formula_includes`). Tests override via `MCPBRIDGE_CONFIG_DIR`.
- **Smallest coherent remediation:** resolve share dirs from `HOMEBREW_PREFIX` (env) then fall back to the two hardcoded paths; align `docs/config-schema.md` and `STABILITY.md` to the same list.
- **Verification:** unit test on `resolveConfigDirs` with `HOMEBREW_PREFIX=/tmp/brew` expecting `/tmp/brew/share/mcpbridge` in the slice.
- **Ratchet candidate:** that unit test in `main_test.go`.

### ENT-008: Mid-reload `reload` envelopes are dropped; drain-queue in-flight accounting is still best-effort

- **Priority:** P2
- **Dimensions:** Correctness / verification; Local code quality
- **Status:** observed fact (reload drop) / inference (in-flight steal after resume)
- **Evidence:**
  - `wrapper/src/main.c:397-401`: if `pending_reload_seq != 0`, log `"reload already in progress; ignoring new one"` and do not replace the seq. Watcher coalesces 300 ms (`watcher.go:37`) but a second binary change **during** a long drain is not re-armed.
  - `wrapper/src/dispatch.c:625-639` `drain_queue`: comment still says queued entries are “treat[ed] as requests for in-flight purposes”; the body only `send_raw_with_newline` — **no** `in_flight++` and no outstanding-id insert. Comment cites “🎯T1.6a”.
- **Mechanism:** lost reload ⇒ wrapper keeps the old child until the next fsnotify event or SIGHUP. Untracked queued requests after resume can decrement `in_flight` via the “remove failed but count > 0” path (`dispatch.c:577-581` region), which is the remaining edge of Fable’s premature-`IN_FLIGHT_ZERO` hazard (partially mitigated by `dispatch_settle_in_flight` on child death, v0.9.0 / `#24`).
- **Blast radius:** upgrades that rewrite the binary twice in one drain (some `brew upgrade` bursts); queued-during-drain requests after swap.
- **Counterevidence checked:** `e2e_reload_inflight_test.sh` covers **one** in-flight reload. `dispatch_settle_in_flight` + `e2e_child_death_inflight_test.sh` close the “count stuck / session dead” hole for child death. Coalesce window reduces double-fire from a single upgrade.
- **Smallest coherent remediation:** latch a `reload_again` flag when dropping a reload; on return to `RUNNING`, if set, emit `RELOAD_REQUESTED` again. For the queue: parse kind and `inflight_add` on drain, or stop decrementing on unknown ids.
- **Verification:** e2e that delivers two `reload` envelopes during one drain and asserts a second cycle. dispatch_test that queued request ids are tracked.
- **Ratchet candidate:** those tests.

### ENT-009: Residual scheduler / C-watcher vocabulary in live code

- **Priority:** P3
- **Dimensions:** Local code quality
- **Status:** observed fact
- **Evidence:**
  - `wrapper/Makefile:23-33` — “watcher backend” Darwin/Linux switch with `WATCH_SRC =` empty both sides; still fails other OS.
  - `daemon/internal/socket/server.go:272-276`, `:303-306` — `ReloadName` / `ChildBinaryForName` comments still say “Used by the scheduler” / “write a freshly downloaded GitHub release asset”. `ChildBinaryForName` has no production caller (`rg` hits comments + `race_test.go` only).
  - `daemon/internal/watcher/watcher.go:5-7`, `:26-27` — comments refer to “the scheduler”.
- **Mechanism:** leftover names send readers looking for `internal/scheduler` (removed in T11). `WATCH_SRC` is dead Makefile surface.
- **Blast radius:** maintainer time only; no runtime.
- **Counterevidence checked:** `WATCH_SRC` is expanded into `MAIN_SRCS` but empty, so it does not break the build. `ChildBinaryForName` still useful as a test/diagnostic helper.
- **Smallest coherent remediation:** delete `WATCH_SRC`; reword comments; keep or unexport `ChildBinaryForName` deliberately.
- **Verification:** `rg -n 'scheduler|WATCH_SRC' daemon wrapper/Makefile` limited to historical docs.
- **Ratchet candidate:** none required; comment cleanup.

## Redundancy and competing-source-of-truth inventory

| Fact | Authorities | Drift observed? |
|---|---|---|
| Config schema v2 fields | `docs/config-schema.md`, C `config.c`, Go `config.go`, `STABILITY.md`, `README.md` | **Yes** — timeout missing from schema doc + Go struct; `source` required only in STABILITY; `::1` C vs Go |
| Wire protocol v1 constants / types | `docs/wire-protocol.md`, C `daemon_client.c` string `"t"` values, Go `protocol.go` `MessageType` | **Yes** — 256 KiB vs 32 KiB reader; register `name` derivation paragraph in wire-protocol is pre-v0.4.0 |
| Socket path algorithm | `docs/wire-protocol.md:21-33`, C `main.c:127-161`, Go `path.go:14-46` | Mostly aligned; C Darwin never uses `$TMPDIR/mcpbridge-$UID` fallback (uses `$HOME` or `/tmp` + Caches). `--socket` then env then platform default matches. |
| Agent help | `agents-guide.md` (daemon embed), `main.c` `agent_help_text` | **Yes** — ENT-005 |
| Homebrew packaging | `release.yml` homebrew-releaser, `packaging/homebrew/mcpbridge.rb` | **Yes** — ENT-006 |
| Share-dir list | config-schema `$HOMEBREW_PREFIX`, STABILITY hardcoded, `main.go` hardcoded | **Yes** — ENT-007 |
| Product mission (poll vs watch) | README / daemon main vs STABILITY 1.0 gaps / 🎯T1 acceptance text | **Yes** — ENT-001 |

**Deliberate duplication (not a defect):** C wrapper vs Go daemon as two languages for two process lifetimes (README). Vestigial `register_ok.polling` always false (`main.go:163-165`, `docs/wire-protocol.md:166-168`) — compatibility with v1 envelopes. Ignoring unknown JSON fields on both config parsers so old `source` blocks still load.

## Healthy structure worth retaining

- **Process split.** Per-session C proxy + once-per-user Go daemon is documented, small, and matches how MCP clients launch servers. Do not collapse into one language without a new target.
- **FSM purity.** `fsm.c` is I/O-free and unit-tested; that boundary is real (`fsm.h:9-22`).
- **Fable-5 remediations that still hold:** singleton `flock` (`server.go:115-138`, `singleton_test.go`); `c.reg` published under `s.mu` (`server.go:422-430`) with `go test -race` in `make test` (`Makefile:36-42`); watcher keyed by `connID` (`watcher.go:39-41`, `:182-187`).
- **Shipped-path e2e.** Wrapper Makefile runs real `mcpbridge` + `mcpbridge-daemon` + `fake_mcp`/`fake_http_mcp` for reload, in-flight reload, child-death-in-drain, HTTP session rotation, dead upstream, daemon outage. `make test` on this host: all green.
- **Version injection.** `VERSION` from Make/`-ldflags` / `-DMCPBRIDGE_VERSION`; on-disk default `0.0.0-dev` is documented (`agents-guide.md` gotchas).
- **License/attribution.** Apache-2.0 `LICENSE`; `NOTICES.md` for cJSON, fsnotify, `x/sys`; vendored cJSON LICENSE file.
- **Internal API non-commitment.** `STABILITY.md:221-232` correctly marks `daemon/internal/*` and `wrapper/src/*.h` as not public.

## Hygiene posture

**Hygiene posture not declared.** No root `hygiene.yaml`. Per the hygiene skill, the validator was **not** run and a file was **not** initialized.

Informal map (not a held-tier vector):

| Dim | Reality in-repo | Notes |
|---|---|---|
| correctness | `make test` in CI (ubuntu + macos) | Strong for this size; no coverage upload |
| quality | `-Wall -Werror` C; `gofmt`/`go vet` only on `make bullseye`, **not** CI | Drift possible on PRs that skip local bullseye |
| security | no scanner, no CODEOWNERS, no Dependabot | UDS same-user trust is implicit |
| deps | `daemon/go.sum` present; cJSON vendored | `golang.org/x/sys v0.13.0` not scanned |
| release | `release.yml` matrix + homebrew-releaser | ENT-006 fossil formula |
| docs | README, schema, wire, packaging, STABILITY, agents-guide | ENT-001/002/005 |
| governance | LICENSE Apache-2.0 | no CODEOWNERS |
| vcs | `.gitignore` covers C/Go artifacts and `help_agent.md` | — |

Overlap with entropy: ENT-001/005/006 are documentation/release hygiene as well as competing truths. Do not double-count as hygiene drift until `hygiene.yaml` exists.

Entropy findings suitable as future hygiene items (after adoption, not in this audit): ENT-002 fixture runner; ENT-004 `MaxLineBytes` test; STABILITY-vs-parser grep; `gofmt` in CI.

## Oracle coverage and residue

| Load-bearing property | Oracle |
|---|---|
| Wrapper+daemon build | `make` (CI) |
| Wrapper unit + e2e, daemon `-race` | `make test` (CI) — **shipped path**, green 2026-08-22 on this host |
| Daemon singleton | `TestNewServer_RefusesSecondDaemon` |
| `conn.reg` data race | `TestServer_ConnRegNoRaceUnderPush` + `go test -race` |
| Watcher conn-keyed untrack | `watcher_test.go` (package tests passed) |
| Reload while idle / in-flight | `e2e_reload_test.sh`, `e2e_reload_inflight_test.sh` |
| Child death during drain | `e2e_child_death_inflight_test.sh` |
| HTTP stale session / timeout / 5xx | `e2e_http_session_invalid*.sh`, `e2e_http_dead_upstream_test.sh`, `e2e_http_upstream_error_test.sh` |
| Daemon outage does not kill stdio | `e2e_daemon_outage_test.sh` |
| Config vestigial fields load | `TestVestigialFieldsLoadSilently` |
| C/Go schema agreement (`::1`, timeout, extra fields) | **none** |
| Cross-peer 256 KiB frame cap | **none** |
| `FSM_RESPAWN` on the event loop | **none** (only `fsm_test`) |
| `mcpbridge --help-agent` tracks `agents-guide.md` | **none** (🎯T12) |
| `resolveConfigDirs` vs `$HOMEBREW_PREFIX` | **none** |
| STABILITY catalogue ⊆ shipped surface | **none** |
| Dependency CVEs | **none** |
| Live Homebrew formula / live mnemo | **none** this run (release job exists; not executed) |
| gofmt/vet | local `make bullseye` only |

**Owner residue (intent, not mechanical work):**

1. Is unexpected stdio child death while `RUNNING` supposed to respawn or end the session? ENT-003 cannot be closed without that choice.
2. Is `::1` in the HTTP v1 contract, or should it be struck until C parses bracket IPv6?
3. Should `docs/config-schema.md` remain the single schema SoT (requiring it to list wrapper-only fields the daemon ignores)?
4. Accept same-user unauthenticated UDS as the trust model, or require peer-cred later? (Fable-5 noted this; still undocumented in `wire-protocol.md`.)

## Remediation sequence

1. **Repair the contract seam (ENT-001).** Rewrite `STABILITY.md` to the v0.8.0+ fsnotify product. Until that file matches the parsers, every other doc fix will be second-guessed.
2. **One schema oracle (ENT-002).** Shared accept/reject fixtures for C and Go; fix or drop `::1`; put `tool_call_timeout_ms` in `docs/config-schema.md`.
3. **Decide `RESPAWN` (ENT-003).** Implement the timer + e2e, or delete the state. Do not leave unit tests as a fictional recovery path.
4. **Unify the line cap (ENT-004)** and add the 256 KiB±1 cross-peer test (also the Fable-5 leftover).
5. **T12 embed (ENT-005); share-dir + formula (ENT-007, ENT-006); reload-again flag (ENT-008); comment sweep (ENT-009).**
6. **Ratchet** the schema fixtures, frame-cap test, and STABILITY grep into CI. If hygiene onboarding is requested later, declare those items in `hygiene.yaml` — not as part of this audit.
7. Re-run this audit on the same finding IDs and dimension definitions.

No production code was changed in this audit.
