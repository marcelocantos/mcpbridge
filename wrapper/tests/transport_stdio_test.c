/* Copyright 2026 Marcelo Cantos
 * SPDX-License-Identifier: Apache-2.0 */

/* Integration test for the stdio transport.
 *
 * Spawns the fake_echo child via the transport, sends a newline-
 * framed MCP-shaped message, then pumps to collect the echoed
 * message via the transport's on_message callback. Verifies the
 * round-trip preserves payload bytes (without the wire-level
 * newline, which framing consumes). Then stops cleanly and checks
 * the exit status. If anything hangs for longer than a short
 * deadline, the test aborts the process itself so a regression
 * shows up as a timeout rather than a silent hang. */

#include "../src/transport.h"
#include "../src/transport_stdio.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fail_count = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);  \
            fail_count++;                                                  \
        }                                                                  \
    } while (0)

/* Global hang-buster. If the test hangs for more than 5 seconds,
 * SIGALRM terminates us so the failure surfaces as a timeout. */
static void install_watchdog(void) {
    struct sigaction sa = {0};
    sa.sa_handler = SIG_DFL; /* default action for SIGALRM is termination */
    sigaction(SIGALRM, &sa, NULL);
    alarm(5);
}

/* Collector for pump() callbacks. Concatenates every message body
 * delivered (each one already stripped of its trailing newline) into
 * a fixed buffer with inter-message '\n' separators, so the test can
 * assert exact expected payloads. */
struct collector {
    char   buf[256];
    size_t len;
    int    count;
};

static void collect_message(void *vctx, const char *line, size_t len) {
    struct collector *c = vctx;
    if (c->count > 0 && c->len < sizeof(c->buf)) {
        c->buf[c->len++] = '\n';
    }
    size_t room = sizeof(c->buf) - c->len;
    size_t take = len < room ? len : room;
    memcpy(c->buf + c->len, line, take);
    c->len += take;
    c->count++;
}

/* Pump until the collector has seen at least `want_messages` complete
 * messages or the deadline elapses. Returns the number of messages
 * collected. */
static int pump_until(struct transport *t, struct collector *c,
                      int want_messages, int timeout_ms) {
    int fd = transport_poll_fd(t);
    const int step_ms = timeout_ms;
    while (c->count < want_messages) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, step_ms);
        if (pr == 0) {
            break; /* timeout */
        }
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        int r = transport_pump(t, collect_message, c);
        if (r == 1 || r < 0) {
            break;
        }
    }
    return c->count;
}

static void test_round_trip(void) {
    char *argv[] = { (char *)"./tests/fake_echo", NULL };
    struct transport *t = transport_stdio_new("./tests/fake_echo", argv);
    CHECK(t != NULL, "transport created");

    int rc = transport_start(t);
    CHECK(rc == 0, "transport started");
    CHECK(transport_poll_fd(t) >= 0, "poll_fd set");
    CHECK(transport_stdio_pid(t) > 0, "pid recorded");

    const char payload[] = "hello, transport\n";
    const size_t plen = sizeof(payload) - 1;

    rc = transport_send(t, payload, plen);
    CHECK(rc == 0, "payload sent");

    struct collector coll = {0};
    int got = pump_until(t, &coll, 1, 2000);
    CHECK(got == 1, "one complete message collected");
    /* The transport strips the trailing newline as part of framing,
     * so the collector sees "hello, transport" without the \n. */
    CHECK(coll.len == plen - 1, "collected length matches payload minus framing newline");
    CHECK(memcmp(coll.buf, payload, plen - 1) == 0, "bytes match verbatim");

    /* Before stopping, verify reap reports still-running. */
    int status = -1;
    int reap = transport_stdio_reap(t, &status);
    CHECK(reap == 0, "reap reports child still running");

    rc = transport_stop(t);
    CHECK(rc == 0, "transport stopped");
    CHECK(transport_stdio_pid(t) == -1, "pid cleared after stop");

    transport_destroy(t);
}

static void test_destroy_without_start(void) {
    /* Creating and destroying a transport without ever starting it
     * must not crash or leak fds. */
    char *argv[] = { (char *)"./tests/fake_echo", NULL };
    struct transport *t = transport_stdio_new("./tests/fake_echo", argv);
    CHECK(t != NULL, "transport created");
    transport_destroy(t);
}

/* no-op callback: we only care about EOF detection in this test. */
static void ignore_message(void *vctx, const char *line, size_t len) {
    (void)vctx; (void)line; (void)len;
}

static void test_exec_failure(void) {
    /* A nonexistent command should fail either at fork time (rare)
     * or at exec time (the child exits 127 immediately). The parent
     * sees start() succeed (fork worked) but subsequent pumps hit
     * EOF quickly, and reap reports exit 127. */
    char *argv[] = { (char *)"/nonexistent/mcpbridge-fake-binary", NULL };
    struct transport *t = transport_stdio_new(
        "/nonexistent/mcpbridge-fake-binary", argv);
    CHECK(t != NULL, "transport created");

    int rc = transport_start(t);
    CHECK(rc == 0, "fork path succeeded (exec failure is in the child)");

    /* Poll for readability (which will show up as POLLHUP once the
     * child has exited), then pump until we see the EOF return. */
    int saw_eof = 0;
    for (int i = 0; i < 100 && !saw_eof; i++) {
        int fd = transport_poll_fd(t);
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 50);
        if (pr == 0) continue;
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        int r = transport_pump(t, ignore_message, NULL);
        if (r == 1) {
            saw_eof = 1;
            break;
        }
        if (r < 0) break;
    }
    CHECK(saw_eof, "pump sees EOF after child exec failure");

    int status = 0;
    /* The child may take a moment to be reaped; give it up to 1s. */
    int reap = 0;
    for (int i = 0; i < 100; i++) {
        reap = transport_stdio_reap(t, &status);
        if (reap == 1) break;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 10 * 1000 * 1000L };
        nanosleep(&ts, NULL);
    }
    CHECK(reap == 1, "child reaped");
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 127,
          "child exited 127 (exec failure)");

    transport_destroy(t);
}

int main(void) {
    install_watchdog();

    test_round_trip();
    test_destroy_without_start();
    test_exec_failure();

    if (fail_count > 0) {
        fprintf(stderr, "%d transport_stdio_test assertion(s) failed\n", fail_count);
        return 1;
    }
    puts("transport_stdio_test: ok");
    return 0;
}
