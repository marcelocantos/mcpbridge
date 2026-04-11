/* Copyright 2026 Marcelo Cantos
 * SPDX-License-Identifier: Apache-2.0 */

/* Integration test for the stdio transport.
 *
 * Spawns the fake_echo child via the transport, writes a payload,
 * reads it back, then stops cleanly and checks the exit status. If
 * anything hangs for longer than a short deadline, the test aborts
 * the process itself so a regression shows up as a timeout rather
 * than a silent hang. */

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

/* Read up to want bytes, blocking up to timeout_ms via poll().
 * Returns the number of bytes read, 0 on EOF, -1 on timeout. */
static ssize_t read_with_timeout(struct transport *t,
                                 void *buf, size_t want, int timeout_ms) {
    int fd = transport_read_fd(t);
    char *p = buf;
    size_t got = 0;
    while (got < want) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr == 0) {
            return -1; /* timeout */
        }
        if (pr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        ssize_t r = transport_read(t, p + got, want - got);
        if (r == 0) {
            return (ssize_t)got;
        }
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        got += (size_t)r;
    }
    return (ssize_t)got;
}

static void test_round_trip(void) {
    char *argv[] = { (char *)"./tests/fake_echo", NULL };
    struct transport *t = transport_stdio_new("./tests/fake_echo", argv);
    CHECK(t != NULL, "transport created");

    int rc = transport_start(t);
    CHECK(rc == 0, "transport started");
    CHECK(transport_read_fd(t) >= 0, "read_fd set");
    CHECK(transport_write_fd(t) >= 0, "write_fd set");
    CHECK(transport_stdio_pid(t) > 0, "pid recorded");

    const char payload[] = "hello, transport\n";
    const size_t plen = sizeof(payload) - 1;

    ssize_t w = transport_write(t, payload, plen);
    CHECK(w == (ssize_t)plen, "payload written");

    char buf[64] = {0};
    ssize_t r = read_with_timeout(t, buf, plen, 2000);
    CHECK(r == (ssize_t)plen, "payload read back");
    CHECK(memcmp(buf, payload, plen) == 0, "bytes match verbatim");

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

static void test_exec_failure(void) {
    /* An nonexistent command should fail either at fork time (rare)
     * or at exec time (the child exits 127 immediately). The parent
     * sees start() succeed (fork worked) but subsequent reads hit
     * EOF quickly, and reap reports exit 127. */
    char *argv[] = { (char *)"/nonexistent/mcpbridge-fake-binary", NULL };
    struct transport *t = transport_stdio_new(
        "/nonexistent/mcpbridge-fake-binary", argv);
    CHECK(t != NULL, "transport created");

    int rc = transport_start(t);
    CHECK(rc == 0, "fork path succeeded (exec failure is in the child)");

    char buf[16];
    ssize_t r = read_with_timeout(t, buf, sizeof(buf), 2000);
    CHECK(r == 0, "read sees EOF after child exec failure");

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
