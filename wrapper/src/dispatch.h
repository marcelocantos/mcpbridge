/* Copyright 2026 Marcelo Cantos
 * SPDX-License-Identifier: Apache-2.0 */

#ifndef MCPBRIDGE_DISPATCH_H
#define MCPBRIDGE_DISPATCH_H

/* Dispatch layer.
 *
 * Sits between the event loop (which owns the fds) and the FSM +
 * parser. Takes parsed MCP messages in, produces "send this" calls
 * out, and raises FSM events when something state-relevant happens.
 *
 * Responsibilities in this minimum-viable slice (🎯T1.6):
 *
 *   - Forward MCP messages between upstream (agent) and child based
 *     on the FSM's current state.
 *   - Track in-flight requests so the DRAINING state can eventually
 *     reach IN_FLIGHT_ZERO.
 *   - Queue upstream messages while the FSM is not RUNNING so the
 *     agent never sees its requests lost or out-of-order across a
 *     child restart.
 *   - Detect the first initialize response from the child and emit
 *     INITIALIZE_OK (or INITIALIZE_FAILED on an error response).
 *
 * Explicitly NOT in this slice (deferred to 🎯T1.6a):
 *   - Cached initialize replay on child restart (so the new child
 *     can be re-initialised without upstream involvement).
 *   - tools/list caching, diffing, and notifications/tools/list_changed
 *     emission.
 *
 * The dispatch module is pure with respect to I/O: it takes a sink
 * (three callbacks) at construction time and invokes them instead
 * of performing reads or writes itself. This makes it unit-testable
 * without forks, pipes, or temp files. */

#include "fsm.h"

#include <stddef.h>

struct mcp_msg;

/* Callbacks the dispatch layer uses to emit its decisions. The ctx
 * pointer is passed back unchanged on every call. */
struct dispatch_sink {
    /* Write bytes to the upstream agent (the MCP client). */
    void (*send_upstream)(void *ctx, const void *bytes, size_t n);
    /* Write bytes to the current child transport. */
    void (*send_child)(void *ctx, const void *bytes, size_t n);
    /* Raise an FSM event (e.g. INITIALIZE_OK, IN_FLIGHT_ZERO). */
    void (*emit_event)(void *ctx, enum fsm_event ev);
    void *ctx;
};

struct dispatch;

/* Construct a dispatch module bound to an FSM and a sink. Neither
 * pointer is copied; both must outlive the returned dispatch. */
struct dispatch *dispatch_new(struct fsm *fsm,
                              const struct dispatch_sink *sink);

void dispatch_free(struct dispatch *d);

/* Handle a complete message received from the upstream agent. */
void dispatch_on_upstream(struct dispatch *d, const struct mcp_msg *m);

/* Handle a complete message received from the child. */
void dispatch_on_child(struct dispatch *d, const struct mcp_msg *m);

/* Inform the dispatch layer that the FSM has just transitioned. The
 * event loop calls this after every fsm_step. Used to drain queued
 * upstream messages on a transition into RUNNING. */
void dispatch_on_state_change(struct dispatch *d, enum fsm_state new_state);

/* Current in-flight request count, for tests and observability. */
int dispatch_in_flight(const struct dispatch *d);

/* Whether the first initialize response has been observed. */
int dispatch_initialized(const struct dispatch *d);

#endif /* MCPBRIDGE_DISPATCH_H */
