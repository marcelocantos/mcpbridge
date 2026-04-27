/* Copyright 2026 Marcelo Cantos
 * SPDX-License-Identifier: Apache-2.0 */

/* mcpbridge — wrapper entry point.
 *
 * Wires the pure modules (parser, FSM, dispatch, transport) and the
 * daemon client into a runnable binary.
 *
 * Invocation:
 *
 *   mcpbridge [OPTIONS] connect <path>
 *
 * <path> is a per-server config file (schema v2) describing how to
 * reach the wrapped MCP server. The wrapper reads the file, picks
 * the backend based on which connection field is set:
 *   - "command"+"args": stdio backend (fork/exec a child)
 *   - "url"            : http backend (localhost MCP Streamable HTTP)
 *
 * Daemon connection is best-effort: if the daemon is absent the
 * wrapper still works as a transparent proxy and periodically tries
 * to reconnect in the background with 1s -> 5s backoff. RELOAD
 * notifications from the daemon drive a full
 * DRAINING -> SWAPPING -> backend cycle -> cached-initialize replay
 * -> RUNNING -> reload_ack cycle. For stdio, "backend cycle" is a
 * child respawn; for http, it is session-id reset + re-dial on the
 * next POST.
 *
 * Shutdown is clean on upstream EOF, SIGINT, SIGTERM, or an FSM
 * FAILED transition. */

#include "config.h"
#include "daemon_client.h"
#include "dispatch.h"
#include "fsm.h"
#include "log.h"
#include "mcp.h"
#include "transport.h"
#include "transport_http.h"
#include "transport_stdio.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ---------- Help text ---------- */

static const char usage_text[] =
    "Usage: mcpbridge [OPTIONS] connect <path>\n"
    "\n"
    "Bring up the wrapped MCP server described by the schema-v2\n"
    "config file at <path>, and keep its session alive across\n"
    "upgrades. The agent always talks to mcpbridge over stdio; the\n"
    "backend (stdio child or localhost HTTP MCP endpoint) is\n"
    "determined by the config file.\n"
    "\n"
    "<path> may begin with `~` (home dir) or `~user` (named user's\n"
    "home dir), or be relative to the current directory.\n"
    "\n"
    "Options:\n"
    "  --socket PATH    override daemon socket path\n"
    "  -v, --verbose    extra logging to stderr\n"
    "  --version        print version and exit\n"
    "  --help           print this help and exit\n"
    "  --help-agent     print agent-oriented help and exit\n";

static const char agent_help_text[] =
    "mcpbridge " MCPBRIDGE_VERSION "\n"
    "\n"
    "Transparently bridges an MCP server. The agent talks to\n"
    "mcpbridge over stdio; mcpbridge reads the config file at <path>\n"
    "and bridges to either a stdio child (config has \"command\") or\n"
    "a localhost MCP Streamable HTTP endpoint (config has \"url\").\n"
    "\n"
    "MCP client config:\n"
    "\n"
    "  { \"command\": \"mcpbridge\",\n"
    "    \"args\": [\"connect\", \"~/.config/mcpbridge/<name>.json\"] }\n"
    "\n"
    "The config file's contents determine the backend transparently;\n"
    "the agent's launch command never changes when a server's\n"
    "backend type changes.\n"
    "\n"
    "mcpbridge bridges the session and cycles the backend when\n"
    "mcpbridge-daemon tells it a newer version is installed —\n"
    "invisibly to the agent's MCP session. The wrapped server is not\n"
    "aware of mcpbridge and requires no modification.\n"
    "\n"
    "See docs/config-schema.md for the file format.\n";

/* ---------- Signal handling ---------- */

static volatile sig_atomic_t shutdown_requested = 0;

static void handle_shutdown_signal(int sig) {
    (void)sig;
    shutdown_requested = 1;
}

static void install_signal_handlers(void) {
    struct sigaction sa = {0};
    sa.sa_handler = handle_shutdown_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    struct sigaction ign = {0};
    ign.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &ign, NULL);
}

/* ---------- Socket path resolution ---------- */

/* Resolve the daemon's default socket path per docs/wire-protocol.md:
 * $MCPBRIDGE_SOCKET, else macOS Caches / Linux XDG_RUNTIME_DIR, else
 * $TMPDIR/mcpbridge-$UID/daemon.sock. Caller does not own the
 * returned pointer — it refers to a static buffer, called once at
 * startup. */
static const char *resolve_daemon_socket_path(const char *cli_override) {
    static char buf[PATH_MAX];

    if (cli_override != NULL && cli_override[0] != '\0') {
        snprintf(buf, sizeof(buf), "%s", cli_override);
        return buf;
    }

    const char *env = getenv("MCPBRIDGE_SOCKET");
    if (env != NULL && env[0] != '\0') {
        snprintf(buf, sizeof(buf), "%s", env);
        return buf;
    }

#ifdef __APPLE__
    const char *home = getenv("HOME");
    if (home == NULL || *home == '\0') {
        home = "/tmp";
    }
    snprintf(buf, sizeof(buf), "%s/Library/Caches/mcpbridge/daemon.sock", home);
    return buf;
#else
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg != NULL && xdg[0] != '\0') {
        snprintf(buf, sizeof(buf), "%s/mcpbridge/daemon.sock", xdg);
        return buf;
    }
    const char *tmp = getenv("TMPDIR");
    if (tmp == NULL || *tmp == '\0') {
        tmp = "/tmp";
    }
    snprintf(buf, sizeof(buf), "%s/mcpbridge-%u/daemon.sock",
             tmp, (unsigned)getuid());
    return buf;
#endif
}

/* ---------- Backoff clock ---------- */

static long long monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

/* ---------- Event-loop context ---------- */

struct loop_ctx {
    struct transport     *transport;
    struct fsm           *fsm;
    struct dispatch      *dispatch;
    struct daemon_client *daemon;

    const char *config_name;
    const char *sock_path;
    const char *child_bin;       /* argv[0] of the wrapped command */

    /* Reload bookkeeping. */
    uint64_t pending_reload_seq; /* seq of the reload envelope we are
                                    currently handling, 0 if none */

    /* Daemon reconnect backoff (all in monotonic ms). */
    long long next_reconnect_at;
    int       backoff_ms;

    int exit_requested;
};

#define BACKOFF_INITIAL_MS 1000
#define BACKOFF_MAX_MS     5000

static void bump_backoff(struct loop_ctx *c) {
    if (c->backoff_ms == 0) {
        c->backoff_ms = BACKOFF_INITIAL_MS;
    } else {
        c->backoff_ms *= 2;
        if (c->backoff_ms > BACKOFF_MAX_MS) {
            c->backoff_ms = BACKOFF_MAX_MS;
        }
    }
    c->next_reconnect_at = monotonic_ms() + c->backoff_ms;
}

static void reset_backoff(struct loop_ctx *c) {
    c->backoff_ms        = 0;
    c->next_reconnect_at = 0;
}

/* ---------- Blocking write helper ---------- */

static int write_all(int fd, const void *buf, size_t n, const char *label) {
    const char *p = buf;
    size_t left = n;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w > 0) {
            p    += (size_t)w;
            left -= (size_t)w;
            continue;
        }
        if (w == -1 && errno == EINTR) {
            continue;
        }
        log_error("write(%s): %s", label, strerror(errno));
        return -1;
    }
    return 0;
}

/* ---------- Dispatch sink ---------- */

static void sink_send_upstream(void *ctx, const void *bytes, size_t n) {
    struct loop_ctx *c = ctx;
    if (write_all(STDOUT_FILENO, bytes, n, "upstream") != 0) {
        c->exit_requested = 1;
    }
}

static int sink_send_child(void *ctx, const void *bytes, size_t n) {
    struct loop_ctx *c = ctx;
    if (transport_send(c->transport, bytes, n) == 0) {
        return DISPATCH_SEND_OK;
    }
    /* The HTTP transport surfaces upstream-session-loss as ESTALE.
     * Any other failure is treated as fatal (the transport itself is
     * unusable — e.g. the stdio child is gone, or the HTTP socket is
     * persistently broken). The reload pathway recovers from the
     * STALE case transparently; FATAL kills the wrapper. */
    if (errno == ESTALE) {
        log_info("send_child: upstream session stale; "
                 "triggering self-reload");
        return DISPATCH_SEND_STALE;
    }
    log_error("send_child: transport_send failed: %s", strerror(errno));
    c->exit_requested = 1;
    return DISPATCH_SEND_FATAL;
}

static void sink_emit_event(void *ctx, enum fsm_event ev) {
    struct loop_ctx *c = ctx;
    enum fsm_state old = c->fsm->state;
    enum fsm_state new_state = fsm_step(c->fsm, ev);
    if (new_state != old) {
        log_debug("fsm: %s --%s--> %s",
                  fsm_state_name(old),
                  fsm_event_name(ev),
                  fsm_state_name(new_state));
        dispatch_on_state_change(c->dispatch, new_state);

        /* Completing a reload cycle: if a reload was pending and we
         * reached RUNNING, the new child is fully initialised and
         * the agent's session is seamless. Ack the daemon. */
        if (new_state == FSM_RUNNING && c->pending_reload_seq != 0) {
            if (c->daemon != NULL) {
                int rc = daemon_client_send_reload_ack(
                    c->daemon, c->pending_reload_seq, "ok", NULL);
                if (rc != 0) {
                    log_warn("reload_ack send failed: %s", strerror(errno));
                }
                log_info("reload complete; ack sent");
            }
            c->pending_reload_seq = 0;
        }
    }

    if (new_state == FSM_FAILED) {
        c->exit_requested = 1;
    }
}

/* ---------- Daemon connection helpers ---------- */

/* Attempt to dial and handshake the daemon. Returns a connected
 * client on success, NULL on failure. Non-fatal: the caller retries. */
static struct daemon_client *try_connect_daemon(struct loop_ctx *c) {
    struct daemon_client *dc = daemon_client_new(c->sock_path);
    if (dc == NULL) {
        return NULL;
    }
    pid_t wrapper_pid = getpid();
    pid_t child_pid   = transport_stdio_pid(c->transport);
    int rc = daemon_client_do_handshake(dc,
                                        MCPBRIDGE_VERSION,
                                        wrapper_pid,
                                        c->config_name,
                                        child_pid,
                                        c->child_bin);
    if (rc != 0) {
        log_warn("daemon handshake failed: %s", strerror(errno));
        daemon_client_free(dc);
        return NULL;
    }
    log_info("daemon connected (socket=%s, config=%s)",
             c->sock_path, c->config_name);
    return dc;
}

/* Drain every available envelope from the daemon fd. Returns 0 on
 * success, -1 if the connection dropped (caller should clear the
 * client pointer and start the reconnect timer). */
static int drain_daemon(struct loop_ctx *c) {
    for (;;) {
        struct daemon_event ev = {0};
        int r = daemon_client_try_read(c->daemon, &ev);
        if (r == 0) {
            /* No more envelopes buffered. */
            return 0;
        }
        if (r < 0) {
            log_warn("daemon read failed: %s", strerror(errno));
            return -1;
        }
        switch (ev.kind) {
        case DAEMON_EV_NONE:
            daemon_event_free(&ev);
            return 0;
        case DAEMON_EV_CLOSED:
            log_info("daemon socket closed");
            daemon_event_free(&ev);
            return -1;
        case DAEMON_EV_RELOAD:
            log_info("daemon reload received (name=%s, reason=%s)",
                     ev.name != NULL ? ev.name : "",
                     ev.reason != NULL ? ev.reason : "");
            if (c->pending_reload_seq != 0) {
                log_warn("reload already in progress; ignoring new one");
            } else {
                c->pending_reload_seq = ev.seq;
                sink_emit_event(c, FSM_EV_RELOAD_REQUESTED);
            }
            daemon_event_free(&ev);
            break;
        case DAEMON_EV_SHUTDOWN:
            log_info("daemon shutting down (%s)",
                     ev.reason != NULL ? ev.reason : "");
            daemon_event_free(&ev);
            /* Stay up, but treat daemon as gone. */
            return -1;
        case DAEMON_EV_ERROR:
            log_warn("daemon error: code=%s detail=%s",
                     ev.code != NULL ? ev.code : "",
                     ev.detail != NULL ? ev.detail : "");
            daemon_event_free(&ev);
            break;
        case DAEMON_EV_HELLO_OK:
        case DAEMON_EV_REGISTER_OK:
            /* These arrive during the synchronous handshake, not
             * during the operational loop. Ignore if seen here. */
            daemon_event_free(&ev);
            break;
        }
    }
}

/* Drop the current daemon client and start the reconnect backoff. */
static void daemon_disconnect(struct loop_ctx *c) {
    if (c->daemon != NULL) {
        daemon_client_free(c->daemon);
        c->daemon = NULL;
    }
    bump_backoff(c);
    log_info("daemon disconnected; retrying in %d ms", c->backoff_ms);
}

/* ---------- SWAPPING: cycle the transport ---------- */

/* Called when the FSM lands in SWAPPING. Stops the old child, starts
 * a new one, emits TRANSPORT_STARTED, then asks dispatch to replay
 * the cached initialize. Returns 0 on success, -1 on fatal failure. */
static int perform_swap(struct loop_ctx *c) {
    log_info("swap: stopping old child");
    transport_stop(c->transport);

    log_info("swap: starting new child");
    if (transport_start(c->transport) != 0) {
        log_error("swap: transport_start failed: %s", strerror(errno));
        sink_emit_event(c, FSM_EV_TRANSPORT_FAILED);
        return -1;
    }

    /* Move FSM SWAPPING -> STARTING. */
    sink_emit_event(c, FSM_EV_TRANSPORT_STARTED);

    /* Push the cached initialize to the new child. */
    int rc = dispatch_replay_initialize(c->dispatch);
    if (rc == 0) {
        /* No cache — the wrapper hadn't seen initialize yet. Log and
         * transition to FAILED; in practice this shouldn't happen
         * because reload only arrives when RUNNING, which requires
         * the first initialize to have already flowed through. */
        log_warn("swap: no cached initialize to replay");
        sink_emit_event(c, FSM_EV_INITIALIZE_FAILED);
        return -1;
    }
    log_info("swap: initialize replayed to new child");
    return 0;
}

/* ---------- Pump helpers ---------- */

/* Read bytes from `fd` into `reader`. On non-blocking fds, EAGAIN
 * is treated as "no data right now". Returns:
 *    0  progress or empty (call again later),
 *    1  EOF,
 *   -1  fatal read error. */
static int pump_read(int fd, struct mcp_reader *reader) {
    char buf[8192];
    ssize_t r;
    do {
        r = read(fd, buf, sizeof(buf));
    } while (r == -1 && errno == EINTR);
    if (r == 0) {
        return 1;
    }
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        log_error("read(fd=%d): %s", fd, strerror(errno));
        return -1;
    }
    mcp_reader_feed(reader, buf, (size_t)r);
    return 0;
}

/* Set O_NONBLOCK on a fd. Used for stdin so the event loop can
 * reliably probe for EOF regardless of whether the platform's poll
 * surfaces POLLHUP on the reader side of a FIFO whose writers have
 * closed — macOS doesn't always, so we rely on a bounded poll
 * timeout and an explicit non-blocking read instead. */
static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

typedef void (*dispatch_fn)(struct dispatch *, const struct mcp_msg *);

static void drain_reader(struct mcp_reader *reader,
                         struct dispatch   *dispatch,
                         dispatch_fn        handle,
                         const char        *side) {
    for (;;) {
        const char *line = NULL;
        size_t      len  = 0;
        int rc = mcp_reader_pop(reader, &line, &len);
        if (rc == MCP_READER_EMPTY) {
            return;
        }
        if (rc == MCP_READER_TOO_LONG) {
            log_warn("%s: dropped oversized line", side);
            continue;
        }
        struct mcp_msg m = {0};
        int pr = mcp_msg_parse(line, len, &m);
        if (pr != MCP_PARSE_OK) {
            log_warn("%s: parse error %d on %zu-byte line",
                     side, pr, len);
            continue;
        }
        handle(dispatch, &m);
        mcp_msg_free(&m);
    }
}

/* transport_on_message adapter: parse one framed line from the
 * child-side transport and hand the resulting mcp_msg to dispatch.
 * The transport has already done framing (line-splitting for stdio,
 * SSE event extraction for http) so this callback only sees complete
 * JSON-RPC envelopes. */
static void child_on_message(void *vctx, const char *line, size_t len) {
    struct loop_ctx *c = vctx;
    struct mcp_msg m = {0};
    int pr = mcp_msg_parse(line, len, &m);
    if (pr != MCP_PARSE_OK) {
        log_warn("child: parse error %d on %zu-byte line", pr, len);
        return;
    }
    dispatch_on_child(c->dispatch, &m);
    mcp_msg_free(&m);
}

/* ---------- The loop ---------- */

static int run_loop(struct loop_ctx *ctx) {
    struct mcp_reader up_reader = {0};
    mcp_reader_init(&up_reader, 0);

    int rc = 0;

    for (;;) {
        if (shutdown_requested) {
            log_info("shutdown requested");
            break;
        }
        if (ctx->exit_requested) {
            log_info("exit_requested, leaving loop");
            break;
        }

        log_debug("loop: state=%s in_flight=%d pending_reload=%llu",
                  fsm_state_name(ctx->fsm->state),
                  dispatch_in_flight(ctx->dispatch),
                  (unsigned long long)ctx->pending_reload_seq);

        /* Perform any pending swap before rebuilding the poll set
         * so the new child's fd is what we poll this iteration. */
        if (ctx->fsm->state == FSM_SWAPPING) {
            if (perform_swap(ctx) != 0) {
                /* Fatal; the FSM has already transitioned to FAILED
                 * or will on the next tick. */
                continue;
            }
        }

        /* Compute the poll timeout. We always set some timeout so
         * the loop wakes up periodically and can probe stdin for
         * EOF even when the platform doesn't raise POLLHUP reliably
         * on a FIFO. When a reconnect is pending, the timeout is
         * also capped by the backoff deadline. */
        int timeout_ms = 1000;
        if (ctx->daemon == NULL && ctx->sock_path != NULL) {
            long long now  = monotonic_ms();
            long long rem  = ctx->next_reconnect_at - now;
            if (rem < 0) rem = 0;
            if (rem > INT_MAX) rem = INT_MAX;
            int backoff_ms = (int)rem;
            if (backoff_ms < timeout_ms) {
                timeout_ms = backoff_ms;
            }
        }

        int child_fd = transport_poll_fd(ctx->transport);
        int daemon_fd = ctx->daemon != NULL
            ? daemon_client_fd(ctx->daemon) : -1;

        /* stdin is ALWAYS in the poll set — we need something to
         * wake poll up when upstream sends data. But we also probe
         * stdin non-blocking on every iteration for EOF detection
         * (see pump_read at the start of the event handling block),
         * so the poll's revents on stdin is only used to avoid
         * wasteful empty reads. */
        struct pollfd pfds[3];
        int idx_child = -1, idx_daemon = -1;
        int nfds = 0;
        pfds[nfds++] = (struct pollfd){
            .fd = STDIN_FILENO, .events = POLLIN, .revents = 0
        };
        if (child_fd >= 0) {
            pfds[nfds] = (struct pollfd){
                .fd = child_fd, .events = POLLIN, .revents = 0
            };
            idx_child = nfds++;
        }
        if (daemon_fd >= 0) {
            pfds[nfds] = (struct pollfd){
                .fd = daemon_fd, .events = POLLIN, .revents = 0
            };
            idx_daemon = nfds++;
        }

        int pr = poll(pfds, (nfds_t)nfds, timeout_ms);
        if (pr == -1) {
            if (errno == EINTR) continue;
            log_error("poll: %s", strerror(errno));
            rc = 1;
            break;
        }

        /* Upstream stdin. Because stdin is non-blocking and we use
         * a poll timeout, we probe it on every iteration regardless
         * of whether poll reported POLLIN — this works around macOS
         * poll not always surfacing POLLHUP on a FIFO's reader. */
        {
            int p = pump_read(STDIN_FILENO, &up_reader);
            if (p == 1) {
                log_info("upstream closed stdin");
                break;
            }
            if (p < 0) { rc = 1; break; }
            drain_reader(&up_reader, ctx->dispatch,
                         dispatch_on_upstream, "upstream");
        }

        /* Child side: pump lets the transport do whatever framing its
         * medium requires (line splitting for stdio, SSE parsing for
         * http) and invokes child_on_message per complete MCP message.
         * Framing buffers live inside the transport, so swaps reset
         * automatically on transport_stop + transport_start. */
        if (idx_child >= 0 &&
            (pfds[idx_child].revents & (POLLIN | POLLHUP | POLLERR))) {
            int p = transport_pump(ctx->transport, child_on_message, ctx);
            if (p == 1) {
                log_info("child closed stdout");
                sink_emit_event(ctx, FSM_EV_CHILD_EXIT);
                int status = 0;
                (void)transport_stdio_reap(ctx->transport, &status);
                /* If we just initiated a swap (FSM moved to
                 * SWAPPING via DRAINING+CHILD_EXIT) the next loop
                 * iteration will run perform_swap. Otherwise the
                 * child is gone for good and we should exit. */
                if (ctx->fsm->state != FSM_SWAPPING) {
                    break;
                }
                continue;
            }
            if (p < 0) { rc = 1; break; }
        }

        /* Daemon fd. */
        if (idx_daemon >= 0 &&
            (pfds[idx_daemon].revents & (POLLIN | POLLHUP | POLLERR))) {
            if (drain_daemon(ctx) != 0) {
                daemon_disconnect(ctx);
            }
        }

        /* Reconnect timer. */
        if (ctx->daemon == NULL && ctx->sock_path != NULL) {
            if (monotonic_ms() >= ctx->next_reconnect_at) {
                ctx->daemon = try_connect_daemon(ctx);
                if (ctx->daemon != NULL) {
                    reset_backoff(ctx);
                } else {
                    bump_backoff(ctx);
                }
            }
        }
    }

    mcp_reader_free(&up_reader);
    return rc;
}

/* ---------- argv parsing ---------- */

/* migration_die prints a one-line migration message for an obsolete
 * v0.3.0 flag and exits non-zero. */
static void migration_die(const char *flag) {
    fprintf(stderr,
            "mcpbridge: %s is no longer supported. Use\n"
            "  mcpbridge connect <path-to-config-file>\n"
            "and put the connection details in the config file. See\n"
            "docs/config-schema.md.\n",
            flag);
    exit(2);
}

int main(int argc, char **argv) {
    const char *cli_sock = NULL;
    const char *cfg_path = NULL;

    int i = 1;
    /* Top-level options. Stop on `connect`. */
    while (i < argc) {
        const char *a = argv[i];
        if (str_eq(a, "--version")) {
            puts(MCPBRIDGE_VERSION);
            return 0;
        }
        if (str_eq(a, "--help") || str_eq(a, "-h")) {
            fputs(usage_text, stdout);
            return 0;
        }
        if (str_eq(a, "--help-agent")) {
            fputs(agent_help_text, stdout);
            return 0;
        }
        if (str_eq(a, "-v") || str_eq(a, "--verbose")) {
            log_set_level(LOG_DEBUG);
            i++;
            continue;
        }
        if (str_eq(a, "--socket")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "mcpbridge: --socket requires an argument\n");
                return 2;
            }
            cli_sock = argv[i + 1];
            i += 2;
            continue;
        }
        if (str_eq(a, "--")) {
            migration_die("`-- COMMAND`");
        }
        if (str_eq(a, "--url") || str_has_prefix(a, "--url=")) {
            migration_die("--url URL");
        }
        if (str_eq(a, "--config") || str_has_prefix(a, "--config=")) {
            migration_die("--config NAME");
        }
        if (str_eq(a, "connect")) {
            i++;
            break;
        }
        if (str_has_prefix(a, "-")) {
            fprintf(stderr, "mcpbridge: unknown option: %s\n", a);
            fputs(usage_text, stderr);
            return 2;
        }
        /* Bare positional before `connect` is a usage error — the
         * subcommand is mandatory. */
        fprintf(stderr,
                "mcpbridge: unknown subcommand %s (expected `connect`)\n", a);
        fputs(usage_text, stderr);
        return 2;
    }

    /* Path argument for `connect`. */
    if (i >= argc) {
        fputs(usage_text, stderr);
        return 2;
    }
    cfg_path = argv[i++];
    if (i < argc) {
        fprintf(stderr, "mcpbridge: trailing arguments after <path>\n");
        fputs(usage_text, stderr);
        return 2;
    }

    /* Resolve `~`/`~user` in the path. */
    char *expanded_path = expand_tilde(cfg_path);
    if (expanded_path == NULL) {
        return 1;
    }
    struct config *cfg = config_load(expanded_path);
    free(expanded_path);
    if (cfg == NULL) {
        return 1;
    }

    install_signal_handlers();

    /* Non-blocking stdin is required by the event loop's EOF probe
     * (see pump_read). */
    if (set_nonblock(STDIN_FILENO) != 0) {
        log_warn("set_nonblock(stdin) failed: %s", strerror(errno));
    }

    struct transport *t;
    const char       *child_bin;
    switch (cfg->backend) {
    case CONFIG_BACKEND_STDIO:
        t = transport_stdio_new(cfg->command, cfg->args);
        if (t == NULL) {
            log_error("transport_stdio_new failed");
            config_free(cfg);
            return 1;
        }
        child_bin = cfg->command;
        break;
    case CONFIG_BACKEND_HTTP:
        t = transport_http_new(cfg->url);
        if (t == NULL) {
            log_error("transport_http_new(%s) failed: %s",
                      cfg->url, strerror(errno));
            config_free(cfg);
            return 1;
        }
        child_bin = cfg->url;
        break;
    default:
        log_error("invalid config backend %d (this should not happen)",
                  cfg->backend);
        config_free(cfg);
        return 1;
    }

    if (transport_start(t) != 0) {
        log_error("transport_start failed: %s", strerror(errno));
        transport_destroy(t);
        config_free(cfg);
        return 1;
    }

    struct fsm fsm;
    fsm_init(&fsm);

    struct loop_ctx ctx = {
        .transport          = t,
        .fsm                = &fsm,
        .dispatch           = NULL,
        .daemon             = NULL,
        .config_name        = cfg->name,
        .sock_path          = resolve_daemon_socket_path(cli_sock),
        .child_bin          = child_bin,
        .pending_reload_seq = 0,
        .next_reconnect_at  = 0,
        .backoff_ms         = 0,
        .exit_requested     = 0,
    };
    struct dispatch_sink sink = {
        .send_upstream = sink_send_upstream,
        .send_child    = sink_send_child,
        .emit_event    = sink_emit_event,
        .ctx           = &ctx,
    };
    struct dispatch *d = dispatch_new(&fsm, &sink);
    if (d == NULL) {
        log_error("dispatch_new failed");
        transport_stop(t);
        transport_destroy(t);
        config_free(cfg);
        return 1;
    }
    ctx.dispatch = d;

    /* Best-effort daemon connect at startup. Failure is logged once
     * and the wrapper runs in standalone mode with reconnect backoff. */
    ctx.daemon = try_connect_daemon(&ctx);
    if (ctx.daemon == NULL) {
        log_info("daemon unreachable at %s (%s); continuing without it",
                 ctx.sock_path, strerror(errno));
        bump_backoff(&ctx);
    }

    int rc = run_loop(&ctx);

    log_debug("shutting down (rc=%d)", rc);
    if (ctx.daemon != NULL) {
        daemon_client_send_deregister(ctx.daemon, "upstream_closed");
        daemon_client_free(ctx.daemon);
    }
    transport_stop(t);
    dispatch_free(d);
    transport_destroy(t);
    config_free(cfg);
    return rc;
}
