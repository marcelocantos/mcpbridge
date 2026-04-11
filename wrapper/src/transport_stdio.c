/* Copyright 2026 Marcelo Cantos
 * SPDX-License-Identifier: Apache-2.0 */

#include "transport_stdio.h"

#include "log.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct stdio_self {
    const char       *cmd;
    char *const      *argv;
    pid_t             pid;        /* -1 if not started or already reaped */
    int               stdin_fd;   /* parent-side write end of child's stdin */
    int               stdout_fd;  /* parent-side read  end of child's stdout */
};

/* ---------- fd plumbing helpers ---------- */

/* Set FD_CLOEXEC on an fd. We inherit it across exec only for the
 * child's own ends of the pipes, which are dup2'd into stdin/stdout
 * and therefore explicitly cleared. All other fds we open stay
 * FD_CLOEXEC so they do not leak into the child. */
static int set_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    if (flags == -1) {
        return -1;
    }
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

/* Create a pipe with both ends FD_CLOEXEC. We can't use pipe2 —
 * it's Linux/BSD-only and not strictly POSIX. */
static int make_pipe_cloexec(int fds[2]) {
    if (pipe(fds) == -1) {
        return -1;
    }
    if (set_cloexec(fds[0]) == -1 || set_cloexec(fds[1]) == -1) {
        int saved = errno;
        close(fds[0]);
        close(fds[1]);
        errno = saved;
        return -1;
    }
    return 0;
}

static void close_fd(int *fd) {
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

/* ---------- vtable ---------- */

static int stdio_start(void *vself) {
    struct stdio_self *s = vself;
    if (s->pid != -1) {
        errno = EALREADY;
        return -1;
    }
    if (s->cmd == NULL || s->argv == NULL) {
        errno = EINVAL;
        return -1;
    }

    int in_pipe[2]  = {-1, -1}; /* wrapper writes to in_pipe[1]; child reads in_pipe[0] */
    int out_pipe[2] = {-1, -1}; /* child writes to out_pipe[1]; wrapper reads out_pipe[0] */

    if (make_pipe_cloexec(in_pipe) == -1) {
        log_error("stdio_start: pipe(stdin) failed: %s", strerror(errno));
        return -1;
    }
    if (make_pipe_cloexec(out_pipe) == -1) {
        int saved = errno;
        log_error("stdio_start: pipe(stdout) failed: %s", strerror(saved));
        close(in_pipe[0]);
        close(in_pipe[1]);
        errno = saved;
        return -1;
    }

    pid_t pid = fork();
    if (pid == -1) {
        int saved = errno;
        log_error("stdio_start: fork failed: %s", strerror(saved));
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        errno = saved;
        return -1;
    }

    if (pid == 0) {
        /* Child. Plumb stdin/stdout and exec. Never return. */
        if (dup2(in_pipe[0], STDIN_FILENO) == -1) {
            _exit(127);
        }
        if (dup2(out_pipe[1], STDOUT_FILENO) == -1) {
            _exit(127);
        }
        /* Close the duplicated originals — they're FD_CLOEXEC but
         * we're pre-exec, so we need to close them explicitly. */
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        /* stderr is inherited unchanged. */
        execvp(s->cmd, s->argv);
        /* exec failed. Write a minimal message directly to our
         * inherited stderr so the user sees something useful. */
        const char msg1[] = "mcpbridge: exec failed: ";
        (void)!write(STDERR_FILENO, msg1, sizeof(msg1) - 1);
        (void)!write(STDERR_FILENO, s->cmd, strlen(s->cmd));
        (void)!write(STDERR_FILENO, "\n", 1);
        _exit(127);
    }

    /* Parent. Close the child ends and keep the parent ends. */
    close(in_pipe[0]);
    close(out_pipe[1]);

    s->stdin_fd  = in_pipe[1];
    s->stdout_fd = out_pipe[0];
    s->pid       = pid;

    log_debug("stdio_start: spawned %s (pid %d)", s->cmd, (int)pid);
    return 0;
}

/* Wait for the child to exit with a bounded, crude poll. Returns 1
 * if reaped, 0 if still alive after the deadline. */
static int waitpid_with_deadline(pid_t pid, int *status, int timeout_ms) {
    const int step_ms = 10;
    int waited = 0;
    while (waited < timeout_ms) {
        pid_t r = waitpid(pid, status, WNOHANG);
        if (r == pid) {
            return 1;
        }
        if (r == -1) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        /* r == 0: still running. */
        struct timespec ts = {
            .tv_sec  = step_ms / 1000,
            .tv_nsec = (step_ms % 1000) * 1000000L,
        };
        nanosleep(&ts, NULL);
        waited += step_ms;
    }
    return 0;
}

static int stdio_stop(void *vself) {
    struct stdio_self *s = vself;
    /* Close stdin first so the child sees EOF and (hopefully) exits
     * gracefully. Then escalate. */
    close_fd(&s->stdin_fd);

    if (s->pid == -1) {
        close_fd(&s->stdout_fd);
        return 0;
    }

    int status = 0;
    int r;

    /* 200 ms grace for a clean exit on EOF. */
    r = waitpid_with_deadline(s->pid, &status, 200);
    if (r == -1 && errno != ECHILD) {
        log_warn("stdio_stop: waitpid error: %s", strerror(errno));
    }
    if (r == 1 || (r == -1 && errno == ECHILD)) {
        goto done;
    }

    /* Still alive — politely ask it to leave. */
    log_debug("stdio_stop: SIGTERM pid %d", (int)s->pid);
    kill(s->pid, SIGTERM);
    r = waitpid_with_deadline(s->pid, &status, 500);
    if (r == 1 || (r == -1 && errno == ECHILD)) {
        goto done;
    }

    /* Refuses to die — force it. */
    log_warn("stdio_stop: SIGKILL pid %d", (int)s->pid);
    kill(s->pid, SIGKILL);
    r = waitpid_with_deadline(s->pid, &status, 500);
    if (r == 0) {
        log_error("stdio_stop: child %d did not die even after SIGKILL",
                  (int)s->pid);
    }

done:
    s->pid = -1;
    close_fd(&s->stdout_fd);
    return 0;
}

static int stdio_read_fd(void *vself) {
    struct stdio_self *s = vself;
    return s->stdout_fd;
}

static int stdio_write_fd(void *vself) {
    struct stdio_self *s = vself;
    return s->stdin_fd;
}

static ssize_t stdio_read(void *vself, void *buf, size_t n) {
    struct stdio_self *s = vself;
    if (s->stdout_fd < 0) {
        errno = EBADF;
        return -1;
    }
    ssize_t r;
    do {
        r = read(s->stdout_fd, buf, n);
    } while (r == -1 && errno == EINTR);
    return r;
}

static ssize_t stdio_write(void *vself, const void *buf, size_t n) {
    struct stdio_self *s = vself;
    if (s->stdin_fd < 0) {
        errno = EBADF;
        return -1;
    }
    ssize_t r;
    do {
        r = write(s->stdin_fd, buf, n);
    } while (r == -1 && errno == EINTR);
    return r;
}

static void stdio_destroy(void *vself) {
    struct stdio_self *s = vself;
    if (s == NULL) {
        return;
    }
    /* stop is idempotent; calling it here ensures we never leak an
     * unreaped child if the caller forgot to stop first. */
    stdio_stop(s);
    free(s);
}

static const struct transport_ops stdio_ops = {
    .start    = stdio_start,
    .stop     = stdio_stop,
    .read_fd  = stdio_read_fd,
    .write_fd = stdio_write_fd,
    .read     = stdio_read,
    .write    = stdio_write,
    .destroy  = stdio_destroy,
};

/* ---------- public constructors ---------- */

struct transport *transport_stdio_new(const char *cmd, char *const argv[]) {
    if (cmd == NULL || argv == NULL) {
        errno = EINVAL;
        return NULL;
    }
    struct stdio_self *s = xcalloc(1, sizeof(*s));
    s->cmd       = cmd;
    s->argv      = argv;
    s->pid       = -1;
    s->stdin_fd  = -1;
    s->stdout_fd = -1;

    struct transport *t = xcalloc(1, sizeof(*t));
    t->ops  = &stdio_ops;
    t->self = s;
    return t;
}

pid_t transport_stdio_pid(const struct transport *t) {
    if (t == NULL || t->ops != &stdio_ops) {
        return -1;
    }
    const struct stdio_self *s = t->self;
    return s->pid;
}

int transport_stdio_reap(struct transport *t, int *status) {
    if (t == NULL || t->ops != &stdio_ops) {
        errno = EINVAL;
        return -1;
    }
    struct stdio_self *s = t->self;
    if (s->pid == -1) {
        return 0;
    }
    int st = 0;
    pid_t r;
    do {
        r = waitpid(s->pid, &st, WNOHANG);
    } while (r == -1 && errno == EINTR);
    if (r == 0) {
        return 0;
    }
    if (r == -1) {
        if (errno == ECHILD) {
            /* Nothing to reap — treat as already-dead. */
            s->pid = -1;
            if (status != NULL) {
                *status = 0;
            }
            return 1;
        }
        return -1;
    }
    s->pid = -1;
    if (status != NULL) {
        *status = st;
    }
    return 1;
}
