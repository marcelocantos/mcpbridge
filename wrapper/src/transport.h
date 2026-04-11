/* Copyright 2026 Marcelo Cantos
 * SPDX-License-Identifier: Apache-2.0 */

#ifndef MCPBRIDGE_TRANSPORT_H
#define MCPBRIDGE_TRANSPORT_H

/* Transport abstraction.
 *
 * A transport is "a thing the wrapper talks to in order to reach the
 * wrapped MCP server." Two kinds are planned:
 *
 *   - stdio: a child process spawned via fork/exec, speaking
 *     newline-delimited JSON-RPC over stdin/stdout.
 *   - http:  a localhost HTTP endpoint speaking the MCP Streamable
 *     HTTP transport (POST for requests, SSE for server messages).
 *
 * The vtable is deliberately small: setup (start), teardown (stop),
 * poll fds (read_fd / write_fd), and bulk byte I/O (read / write).
 * Everything else — SIGCHLD handling, request framing, id tracking
 * — lives elsewhere.
 *
 * For stdio, read/write are plain pass-throughs to read(2)/write(2)
 * on the parent-side pipe fds. For HTTP, they hide the streaming
 * framing so the event loop sees a byte stream of MCP messages
 * regardless of the transport beneath. */

#include <stddef.h>
#include <sys/types.h>

struct transport_ops {
    /* Start the transport. Returns 0 on success, -1 on failure
     * (e.g. fork/exec failure, connection refused). The transport
     * object is left in a safe-to-destroy state on failure. */
    int (*start)(void *self);

    /* Stop the transport cleanly. For stdio this closes the child's
     * stdin, waits briefly for graceful exit, then escalates to
     * SIGTERM and SIGKILL as needed. Returns 0 on success. Safe to
     * call on a transport that was never started. */
    int (*stop)(void *self);

    /* Return the fd the event loop should poll for readability, or
     * -1 if no readable fd is currently available (e.g. during a
     * reconnect). */
    int (*read_fd)(void *self);

    /* Return the fd for writability, or -1. */
    int (*write_fd)(void *self);

    /* Read up to n bytes into buf. Returns the number of bytes
     * read, 0 on EOF, or -1 on error (errno set). */
    ssize_t (*read)(void *self, void *buf, size_t n);

    /* Write n bytes from buf. Returns the number of bytes written
     * or -1 on error. Short writes are possible on non-blocking fds
     * and the caller must retry. */
    ssize_t (*write)(void *self, const void *buf, size_t n);

    /* Release all resources. The self pointer is invalid after this
     * call. Safe to call on a never-started transport and on one
     * that has already been stopped. */
    void (*destroy)(void *self);
};

struct transport {
    const struct transport_ops *ops;
    void                       *self;
};

/* Thin inline helpers. These exist so call sites read as
 * `transport_start(t)` rather than `t->ops->start(t->self)`. */
static inline int transport_start(struct transport *t) {
    return t->ops->start(t->self);
}
static inline int transport_stop(struct transport *t) {
    return t->ops->stop(t->self);
}
static inline int transport_read_fd(struct transport *t) {
    return t->ops->read_fd(t->self);
}
static inline int transport_write_fd(struct transport *t) {
    return t->ops->write_fd(t->self);
}
static inline ssize_t transport_read(struct transport *t, void *buf, size_t n) {
    return t->ops->read(t->self, buf, n);
}
static inline ssize_t transport_write(struct transport *t,
                                      const void *buf, size_t n) {
    return t->ops->write(t->self, buf, n);
}
static inline void transport_destroy(struct transport *t) {
    if (t != NULL && t->ops != NULL && t->ops->destroy != NULL) {
        t->ops->destroy(t->self);
    }
}

#endif /* MCPBRIDGE_TRANSPORT_H */
