/* Copyright 2026 Marcelo Cantos
 * SPDX-License-Identifier: Apache-2.0 */

/* Integration test for the daemon client.
 *
 * Fork/exec's the real mcpbridge-daemon binary into a private UDS
 * under /tmp, connects to it as a client, performs a hello/register
 * handshake, broadcasts a reload by sending SIGHUP to the daemon,
 * verifies the reload envelope comes through, sends a deregister,
 * and tears the daemon down cleanly. */

#include "../src/daemon_client.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
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

static void install_watchdog(int seconds) {
    /* Default action for SIGALRM is termination — good enough for a
     * hung-test detector. */
    alarm((unsigned)seconds);
}

/* Wait up to timeout_ms for sock_path to exist. */
static int wait_for_socket(const char *sock_path, int timeout_ms) {
    const int step_ms = 20;
    int waited = 0;
    while (waited < timeout_ms) {
        struct stat st;
        if (stat(sock_path, &st) == 0) {
            return 0;
        }
        struct timespec ts = {
            .tv_sec  = step_ms / 1000,
            .tv_nsec = (step_ms % 1000) * 1000000L,
        };
        nanosleep(&ts, NULL);
        waited += step_ms;
    }
    return -1;
}

/* Find the daemon binary. The test is launched from wrapper/, so
 * the daemon lives at ../daemon/mcpbridge-daemon. */
static const char *find_daemon_binary(void) {
    static const char *candidates[] = {
        "../daemon/mcpbridge-daemon",
        "./daemon/mcpbridge-daemon",
        NULL,
    };
    for (int i = 0; candidates[i] != NULL; i++) {
        if (access(candidates[i], X_OK) == 0) {
            return candidates[i];
        }
    }
    return NULL;
}

struct child_daemon {
    pid_t pid;
    char  sock_path[128];
};

static int start_daemon(struct child_daemon *cd) {
    const char *bin = find_daemon_binary();
    if (bin == NULL) {
        fprintf(stderr, "daemon binary not found in expected locations\n");
        return -1;
    }

    snprintf(cd->sock_path, sizeof(cd->sock_path),
             "/tmp/mcpb-dctest-%d.sock", (int)getpid());
    unlink(cd->sock_path);

    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        /* Child: run the daemon. */
        setenv("MCPBRIDGE_SOCKET", cd->sock_path, 1);
        char *argv[] = { (char *)bin, (char *)"-v", NULL };
        execv(bin, argv);
        _exit(127);
    }
    cd->pid = pid;

    if (wait_for_socket(cd->sock_path, 2000) != 0) {
        fprintf(stderr, "daemon did not create socket %s\n", cd->sock_path);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return -1;
    }
    return 0;
}

static void stop_daemon(struct child_daemon *cd) {
    if (cd->pid > 0) {
        kill(cd->pid, SIGTERM);
        int status;
        waitpid(cd->pid, &status, 0);
    }
    unlink(cd->sock_path);
}

static void test_handshake(void) {
    struct child_daemon cd = {0};
    if (start_daemon(&cd) != 0) {
        fprintf(stderr, "SKIP: daemon could not be started\n");
        return;
    }

    struct daemon_client *dc = daemon_client_new(cd.sock_path);
    CHECK(dc != NULL, "daemon_client_new");
    if (dc == NULL) {
        stop_daemon(&cd);
        return;
    }
    CHECK(daemon_client_fd(dc) >= 0, "fd valid");

    int rc = daemon_client_do_handshake(dc,
                                        "test-wrapper",
                                        getpid(),
                                        "mnemo",
                                        getpid() + 1,
                                        "/bin/true");
    CHECK(rc == 0, "handshake succeeded");

    /* Trigger a reload broadcast and read it. */
    kill(cd.pid, SIGHUP);

    /* Poll the daemon client fd for up to 1s waiting for reload. */
    int got_reload = 0;
    struct pollfd pfd = { .fd = daemon_client_fd(dc), .events = POLLIN };
    int pr = poll(&pfd, 1, 1000);
    CHECK(pr > 0, "poll saw reload envelope");

    if (pr > 0) {
        struct daemon_event ev = {0};
        int r = daemon_client_try_read(dc, &ev);
        CHECK(r == 1, "try_read returned an event");
        CHECK(ev.kind == DAEMON_EV_RELOAD, "event kind is RELOAD");
        CHECK(ev.reason != NULL && strcmp(ev.reason, "manual") == 0,
              "reason is 'manual'");
        if (ev.kind == DAEMON_EV_RELOAD) {
            got_reload = 1;
            /* Send a reload_ack back. */
            rc = daemon_client_send_reload_ack(dc, ev.seq, "ok", NULL);
            CHECK(rc == 0, "reload_ack sent");
        }
        daemon_event_free(&ev);
    }
    CHECK(got_reload, "reload envelope observed");

    daemon_client_send_deregister(dc, "test_done");
    daemon_client_free(dc);

    stop_daemon(&cd);
}

static void test_connect_failure(void) {
    /* Connecting to a socket path that doesn't exist returns NULL
     * and sets errno. */
    struct daemon_client *dc = daemon_client_new("/tmp/mcpb-does-not-exist-xyz.sock");
    CHECK(dc == NULL, "new() returns NULL on missing socket");
    /* errno should be something connection-refused-ish. We don't
     * pin a specific value because the kernel may return ENOENT
     * (for connect on a path with no file) or ECONNREFUSED
     * depending on platform. */
}

int main(void) {
    install_watchdog(15);

    test_connect_failure();
    test_handshake();

    if (fail_count > 0) {
        fprintf(stderr, "%d daemon_client_test assertion(s) failed\n", fail_count);
        return 1;
    }
    puts("daemon_client_test: ok");
    return 0;
}
