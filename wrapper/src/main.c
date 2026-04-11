/* Copyright 2026 Marcelo Cantos
 * SPDX-License-Identifier: Apache-2.0 */

/* mcpbridge — wrapper entry point.
 *
 * This file wires the pure modules (parser, FSM, dispatch, transport)
 * into a runnable binary. Scope for this first end-to-end wiring:
 *
 *   - stdio transport only (no HTTP yet, no daemon connection yet)
 *   - transparent proxy: read upstream, forward to child; read child,
 *     forward upstream
 *   - child death detected via pipe EOF, reaped via
 *     transport_stdio_reap, transitions the FSM to FAILED/RESPAWN
 *     (RESPAWN currently also exits — the reload path comes later)
 *   - graceful shutdown on upstream EOF, SIGINT, or SIGTERM
 *
 * Out of scope for now: daemon socket, reload notifications,
 * initialize replay, tools/list diffing, HTTP transport. */

#include "dispatch.h"
#include "fsm.h"
#include "log.h"
#include "mcp.h"
#include "transport.h"
#include "transport_stdio.h"
#include "util.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/* ---------- Help text ---------- */

static const char usage_text[] =
    "Usage: mcpbridge [OPTIONS] -- COMMAND [ARGS...]\n"
    "\n"
    "Wrap an MCP server and keep its session alive across upgrades.\n"
    "\n"
    "Options:\n"
    "  -v, --verbose    extra logging to stderr\n"
    "  --version        print version and exit\n"
    "  --help           print this help and exit\n"
    "  --help-agent     print agent-oriented help and exit\n";

static const char agent_help_text[] =
    "mcpbridge " MCPBRIDGE_VERSION "\n"
    "\n"
    "Wraps any stdio MCP server transparently. In your MCP client\n"
    "config, replace the command with 'mcpbridge' and prefix the\n"
    "original command with '--':\n"
    "\n"
    "  { \"command\": \"mcpbridge\",\n"
    "    \"args\": [\"--\", \"real-mcp-server\", \"--some-flag\"] }\n"
    "\n"
    "mcpbridge forks the wrapped server, bridges stdio, and (once the\n"
    "daemon is wired up) cycles the child when a newer version is\n"
    "installed — invisibly to the agent's MCP session.\n";

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
    sa.sa_flags = 0; /* intentionally NOT SA_RESTART — we want
                        syscalls to return EINTR so the poll loop
                        can notice shutdown_requested promptly. */
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    /* Ignore SIGPIPE so a dead upstream/child write-side surfaces
     * as EPIPE on the write call rather than killing us. */
    struct sigaction ign = {0};
    ign.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &ign, NULL);
}

/* ---------- Event-loop context ---------- */

/* Glues the sink callbacks to the concrete I/O targets. Borrowed
 * pointers: the main function owns the underlying objects. */
struct loop_ctx {
    struct transport *transport;
    struct fsm       *fsm;
    struct dispatch  *dispatch;
    int               exit_requested; /* set when the loop should exit */
};

/* Blocking write that retries on EINTR and short writes. Logs and
 * returns on persistent errors; the caller treats that as fatal. */
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

static void sink_send_upstream(void *ctx, const void *bytes, size_t n) {
    struct loop_ctx *c = ctx;
    if (write_all(STDOUT_FILENO, bytes, n, "upstream") != 0) {
        c->exit_requested = 1;
    }
}

static void sink_send_child(void *ctx, const void *bytes, size_t n) {
    struct loop_ctx *c = ctx;
    int fd = transport_write_fd(c->transport);
    if (fd < 0) {
        log_warn("sink_send_child: no child write fd");
        return;
    }
    if (write_all(fd, bytes, n, "child") != 0) {
        c->exit_requested = 1;
    }
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
    }
    if (new_state == FSM_FAILED || new_state == FSM_RESPAWN) {
        /* T1.7 does not implement the respawn loop yet — both
         * states cause a clean exit. The reload work in 🎯T1.6a
         * and later targets will add the actual recovery path. */
        c->exit_requested = 1;
    }
}

/* ---------- Pump helpers ---------- */

/* Read up to one buffer's worth from `fd` and feed it into `reader`.
 * Returns 0 on data, 1 on EOF, -1 on error. */
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
        log_error("read(fd=%d): %s", fd, strerror(errno));
        return -1;
    }
    mcp_reader_feed(reader, buf, (size_t)r);
    return 0;
}

/* Drain all parseable lines from `reader` and dispatch them via
 * `handle`. Returns on the first EMPTY result. Parse errors and
 * over-long lines are logged and skipped. */
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
        /* rc == MCP_READER_OK. */
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

/* ---------- The loop ---------- */

static int run_loop(struct loop_ctx *ctx) {
    struct mcp_reader up_reader    = {0};
    struct mcp_reader child_reader = {0};
    mcp_reader_init(&up_reader, 0);
    mcp_reader_init(&child_reader, 0);

    int rc = 0;

    for (;;) {
        if (shutdown_requested) {
            log_info("shutdown requested");
            break;
        }
        if (ctx->exit_requested) {
            break;
        }

        int child_fd = transport_read_fd(ctx->transport);

        struct pollfd pfds[2];
        int nfds = 0;
        pfds[nfds++] = (struct pollfd){
            .fd = STDIN_FILENO, .events = POLLIN, .revents = 0
        };
        if (child_fd >= 0) {
            pfds[nfds++] = (struct pollfd){
                .fd = child_fd, .events = POLLIN, .revents = 0
            };
        }

        int pr = poll(pfds, (nfds_t)nfds, -1);
        if (pr == -1) {
            if (errno == EINTR) continue;
            log_error("poll: %s", strerror(errno));
            rc = 1;
            break;
        }

        /* Upstream stdin. */
        if (pfds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            int p = pump_read(STDIN_FILENO, &up_reader);
            if (p == 1) {
                log_info("upstream closed stdin");
                break;
            }
            if (p < 0) { rc = 1; break; }
            drain_reader(&up_reader, ctx->dispatch,
                         dispatch_on_upstream, "upstream");
        }

        /* Child stdout. */
        if (nfds > 1 && (pfds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            int p = pump_read(child_fd, &child_reader);
            if (p == 1) {
                log_info("child closed stdout");
                /* Feed CHILD_EXIT through the sink so the FSM moves
                 * and the dispatch layer is notified. */
                sink_emit_event(ctx, FSM_EV_CHILD_EXIT);
                /* Reap the child so we don't leave a zombie. */
                int status = 0;
                (void)transport_stdio_reap(ctx->transport, &status);
                break;
            }
            if (p < 0) { rc = 1; break; }
            drain_reader(&child_reader, ctx->dispatch,
                         dispatch_on_child, "child");
        }
    }

    mcp_reader_free(&up_reader);
    mcp_reader_free(&child_reader);
    return rc;
}

/* ---------- argv parsing ---------- */

/* Locate the `--` delimiter in argv and split the wrapped command
 * off. On success, `*cmd` points to argv[i+1] and `*cmd_argv` points
 * to &argv[i+1]. Returns 0 on success, nonzero on "no child command
 * given" (caller prints usage). */
static int split_child_argv(int argc, char **argv,
                            const char **cmd, char ***cmd_argv) {
    for (int i = 1; i < argc; i++) {
        if (str_eq(argv[i], "--")) {
            if (i + 1 >= argc) {
                return -1;
            }
            *cmd      = argv[i + 1];
            *cmd_argv = &argv[i + 1];
            return 0;
        }
    }
    return -1;
}

int main(int argc, char **argv) {
    /* Tiny flag pass first so --version / --help still work even
     * without a `--` in argv. */
    for (int i = 1; i < argc; i++) {
        if (str_eq(argv[i], "--version")) {
            puts(MCPBRIDGE_VERSION);
            return 0;
        }
        if (str_eq(argv[i], "--help") || str_eq(argv[i], "-h")) {
            fputs(usage_text, stdout);
            return 0;
        }
        if (str_eq(argv[i], "--help-agent")) {
            fputs(agent_help_text, stdout);
            return 0;
        }
        if (str_eq(argv[i], "-v") || str_eq(argv[i], "--verbose")) {
            log_set_level(LOG_DEBUG);
            continue;
        }
        if (str_eq(argv[i], "--")) {
            break; /* rest of argv is the wrapped command */
        }
        if (str_has_prefix(argv[i], "-")) {
            fprintf(stderr, "mcpbridge: unknown option: %s\n", argv[i]);
            fputs(usage_text, stderr);
            return 2;
        }
    }

    const char *cmd = NULL;
    char      **cmd_argv = NULL;
    if (split_child_argv(argc, argv, &cmd, &cmd_argv) != 0) {
        fputs(usage_text, stderr);
        return 2;
    }

    install_signal_handlers();

    struct transport *t = transport_stdio_new(cmd, cmd_argv);
    if (t == NULL) {
        log_error("transport_stdio_new failed");
        return 1;
    }
    if (transport_start(t) != 0) {
        log_error("transport_start failed: %s", strerror(errno));
        transport_destroy(t);
        return 1;
    }

    struct fsm fsm;
    fsm_init(&fsm);

    struct loop_ctx ctx = {
        .transport       = t,
        .fsm             = &fsm,
        .dispatch        = NULL,
        .exit_requested  = 0,
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
        return 1;
    }
    ctx.dispatch = d;

    /* The FSM stays in STARTING until the first initialize response
     * arrives. The dispatch layer recognises initialize requests and
     * forwards them immediately even while STARTING, so the natural
     * handshake drives the transition to RUNNING. */

    int rc = run_loop(&ctx);

    log_debug("shutting down (rc=%d)", rc);
    transport_stop(t);
    dispatch_free(d);
    transport_destroy(t);
    return rc;
}
