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
 * The vtable is deliberately message-oriented rather than
 * byte-oriented: transports surface complete MCP messages (JSON-RPC
 * envelopes, one per call) rather than raw byte streams. This lets
 * each transport own its own framing — newline splitting for stdio,
 * SSE event parsing and HTTP POST construction for http — without
 * leaking those details to the event loop.
 *
 * Vtable shape:
 *
 *   - start / stop: lifecycle.
 *   - poll_fd: the fd the event loop should add to poll() for
 *     readability, or -1 if nothing is currently poll-able (e.g.
 *     during a reconnect window).
 *   - pump: read whatever is available on the transport, frame it,
 *     and invoke the supplied callback once per complete inbound
 *     MCP message. The callback receives the message body (without
 *     any trailing newline or transport-level framing).
 *   - send: ship one complete outbound MCP message. The caller
 *     supplies bytes in the canonical MCP wire form (JSON-RPC
 *     envelope with trailing newline); the transport strips or
 *     rewraps as its medium requires.
 *   - destroy: release all resources. */

#include <stddef.h>
#include <sys/types.h>

/* Callback invoked by pump() once per complete inbound MCP message.
 * `line` points into a transport-owned buffer and is only valid for
 * the duration of the call — callers that need to retain it must
 * copy. `len` is the message length in bytes, not including any
 * transport-level framing. */
typedef void (*transport_on_message)(void *ctx, const char *line, size_t len);

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
     * reconnect). The event loop adds this to its pollset for POLLIN;
     * on readiness it calls pump(). */
    int (*poll_fd)(void *self);

    /* Read whatever is available, frame it, and invoke on_msg once
     * per complete inbound MCP message. Returns:
     *    1 — EOF: the transport's read side is closed for good.
     *    0 — progress or EAGAIN: call again on next readability.
     *   -1 — error (errno set). */
    int (*pump)(void *self, transport_on_message on_msg, void *ctx);

    /* Send one complete MCP message. Bytes are in canonical MCP wire
     * form (newline-terminated JSON-RPC envelope). The transport
     * strips or rewraps framing as its medium requires; the caller
     * never needs to know. Returns 0 on success, -1 on error. This
     * call blocks until the whole message is out (or fails). */
    int (*send)(void *self, const void *bytes, size_t n);

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
static inline int transport_poll_fd(struct transport *t) {
    return t->ops->poll_fd(t->self);
}
static inline int transport_pump(struct transport *t,
                                 transport_on_message on_msg, void *ctx) {
    return t->ops->pump(t->self, on_msg, ctx);
}
static inline int transport_send(struct transport *t,
                                 const void *bytes, size_t n) {
    return t->ops->send(t->self, bytes, n);
}
static inline void transport_destroy(struct transport *t) {
    if (t != NULL && t->ops != NULL && t->ops->destroy != NULL) {
        t->ops->destroy(t->self);
    }
}

#endif /* MCPBRIDGE_TRANSPORT_H */
